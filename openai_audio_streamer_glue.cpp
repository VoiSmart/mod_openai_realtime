#include "openai_audio_streamer_glue.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <ixwebsocket/IXWebSocket.h>
#include <switch_buffer.h>
#include <switch_json.h>

#include "base64.h"
#include "playback_queue.h"
#include "stream_protocol.h"

namespace {

// A 20 ms mono PCM16 frame at 8 kHz contains 160 samples, or 320 bytes.
constexpr std::size_t FRAME_SIZE_8000 = 320;
constexpr std::size_t MAX_AUDIO_CHUNK_SAMPLES = 16384;
constexpr std::size_t MAX_PLAYBACK_QUEUE_SECONDS = 180;
constexpr int MAX_JSON_DEPTH = 128;
constexpr int MAX_STREAM_BUFFER_MS = 1000;
constexpr int MAX_HEARTBEAT_SECONDS = 3600;
constexpr std::size_t MEDIA_BUG_STEREO_BUFFER_BYTES = SWITCH_RECOMMENDED_BUFFER_SIZE * 2U;
// Overload guard: peer messages beyond this are dropped before copying/parsing.
constexpr std::size_t MAX_WS_MESSAGE_BYTES = std::size_t{8} * 1024U * 1024U;
constexpr std::size_t PLAYBACK_BUFFER_BYTES = 128000;

struct StreamConfig {
    // String pointers are borrowed only for the synchronous stream_data_init construction path;
    // AudioStreamer copies every value it needs and never retains the pointers.
    const char *websocket_uri;
    uint32_t capture_input_rate;
    uint32_t capture_output_rate;
    uint32_t playback_input_rate;
    uint32_t playback_output_rate;
    int channels;
    responseHandler_t response_handler;
    bool disable_per_message_deflate;
    int heartbeat_seconds;
    bool suppress_log;
    int capture_packet_count;
    ix::WebSocketHttpHeaders extra_headers;
    bool disable_reconnect;
    const char *tls_ca_file;
    const char *tls_key_file;
    const char *tls_cert_file;
    bool disable_tls_hostname_validation;
    bool disable_audio_files;
    bool start_muted;
    bool raw_audio_mode;
};

// Persistent buffers for stream_frame to avoid per-frame heap allocations
struct StreamBuffers {
    std::vector<uint8_t> flush_buffer;
    std::vector<spx_int16_t> resample_buffer;
    std::vector<uint8_t> data_buf;

    StreamBuffers() {
        flush_buffer.reserve(SWITCH_RECOMMENDED_BUFFER_SIZE);
        resample_buffer.reserve(SWITCH_RECOMMENDED_BUFFER_SIZE / sizeof(spx_int16_t));
        // switch_core_media_bug_read checks buflen before doubling output for SMBF_STEREO.
        data_buf.resize(MEDIA_BUG_STEREO_BUFFER_BYTES);
    }
};

class AudioStreamer {
  public:
    AudioStreamer(const char *session_id, const StreamConfig& config, private_t *context)
        : m_sessionId(session_id), m_notify(config.response_handler), m_suppress_log(config.suppress_log),
          m_playFile(0), m_playback_input_rate(config.playback_input_rate),
          m_playback_output_rate(config.playback_output_rate),
          m_playback_queue(static_cast<std::size_t>(config.playback_output_rate) * MAX_PLAYBACK_QUEUE_SECONDS,
                           MAX_AUDIO_CHUNK_SAMPLES),
          m_disable_audiofiles(config.disable_audio_files), m_raw_audio_mode(config.raw_audio_mode),
          m_context(context) {

        ix::SocketTLSOptions tlsOptions;

        webSocket.setUrl(config.websocket_uri);

        // NONE disables certificate validation; SYSTEM selects the platform CA bundle.
        if (config.tls_ca_file) {
            tlsOptions.caFile = config.tls_ca_file;
        }

        if (config.tls_key_file) {
            tlsOptions.keyFile = config.tls_key_file;
        }

        if (config.tls_cert_file) {
            tlsOptions.certFile = config.tls_cert_file;
        }

        tlsOptions.disable_hostname_validation = config.disable_tls_hostname_validation;
        webSocket.setTLSOptions(tlsOptions);

        // Keep idle connections alive through intermediaries that enforce inactivity timeouts.
        if (config.heartbeat_seconds)
            webSocket.setPingInterval(config.heartbeat_seconds);

        // Per-message deflate is enabled by default.
        if (config.disable_per_message_deflate)
            webSocket.disablePerMessageDeflate();

        if (!config.extra_headers.empty())
            webSocket.setExtraHeaders(config.extra_headers);

        if (config.disable_reconnect)
            webSocket.disableAutomaticReconnection();

        webSocket.setOnMessageCallback(
            [this](const ix::WebSocketMessagePtr& message) { handleWebSocketMessage(message); });

        if (m_playback_input_rate != m_playback_output_rate) {
            int err = 0;
            m_resampler =
                speex_resampler_init(1, m_playback_input_rate, m_playback_output_rate, SWITCH_RESAMPLE_QUALITY, &err);
            if (!m_resampler || err != RESAMPLER_ERR_SUCCESS) {
                // convertRawAudio drops incoming audio in this state rather than playing it at the wrong rate
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "(%s) Error initializing playback resampler %u -> %u: %s\n", m_sessionId.c_str(),
                                  m_playback_input_rate, m_playback_output_rate, speex_resampler_strerror(err));
            }
        }
    }

    ~AudioStreamer() {
        disconnect();
        deleteFiles();
        if (m_resampler) {
            speex_resampler_destroy(m_resampler);
            m_resampler = nullptr;
        }
    }

    bool start() {
        // start_capture publishes the media bug and channel private before callbacks can run.
        try {
            webSocket.start();
            m_started = true;
            return true;
        } catch (const std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) failed to start WebSocket thread: %s\n",
                              m_sessionId.c_str(), e.what());
            return false;
        }
    }

    bool pop_audio_queue(std::vector<int16_t>& out_audio) {
        return m_playback_queue.pop(out_audio);
    }

    bool isConnected() const {
        return (webSocket.getReadyState() == ix::ReadyState::Open);
    }

    bool sendAudio(const uint8_t *buffer, size_t len) {
        const bool sent = m_raw_audio_mode ? writeBinary(buffer, len) : writeAudioDelta(buffer, len);
        if (!sent) {
            if (!m_send_failure_logged.exchange(true)) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                                  "(%s) sendAudio: send failed or not connected, dropping caller audio\n",
                                  m_sessionId.c_str());
            }
        } else if (m_send_failure_logged.exchange(false)) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "(%s) sendAudio: sending recovered\n",
                              m_sessionId.c_str());
        }
        return sent;
    }

    bool writeText(const char *text) {
        if (!isConnected())
            return false;
        return webSocket.sendUtf8Text(ix::IXWebSocketSendData(text, strlen(text))).success;
    }

    bool capture_reset_pending() const {
        return m_capture_reset_pending.load(std::memory_order_acquire);
    }

    // Returns true once per pending reset request; media (stream_frame) thread only.
    bool consume_capture_reset() {
        return m_capture_reset_pending.exchange(false, std::memory_order_acq_rel);
    }

    // Returns true once per pending clear request; media (write_frame) thread only
    bool consume_playback_clear() {
        const uint64_t gen = m_playback_clear_gen.load(std::memory_order_acquire);
        if (gen == m_playback_clear_gen_seen) {
            return false;
        }
        m_playback_clear_gen_seen = gen;
        return true;
    }

    bool is_openai_speaking() const {
        return m_openai_speaking;
    }

    bool suppress_log() const {
        return m_suppress_log;
    }

    bool is_response_audio_done() const {
        return m_response_audio_done;
    }

    bool is_terminally_closed() const {
        return m_terminal_close;
    }

    void openai_speech_started(switch_core_session_t *session) {
        m_openai_speaking = true;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) OpenAI started speaking\n",
                          m_sessionId.c_str());
        const char *payload = "{\"status\":\"started\"}";
        m_notify(session, EVENT_OPENAI_SPEECH_STARTED, payload);
    }

    void openai_speech_stopped(switch_core_session_t *session) {
        m_openai_speaking = false;
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) OpenAI stopped speaking\n",
                          m_sessionId.c_str());
        const char *payload = "{\"status\":\"stopped\"}";
        m_notify(session, EVENT_OPENAI_SPEECH_STOPPED, payload);
    }

  private:
    void handleWebSocketMessage(const ix::WebSocketMessagePtr& message) {
        switch (message->type) {
            case ix::WebSocketMessageType::Message:
                handleDataMessage(message);
                break;
            case ix::WebSocketMessageType::Open:
                handleConnectionOpen();
                break;
            case ix::WebSocketMessageType::Error:
                handleConnectionError(message->errorInfo);
                break;
            case ix::WebSocketMessageType::Close:
                handleConnectionClose(message->closeInfo);
                break;
            default:
                break;
        }
    }

    void handleDataMessage(const ix::WebSocketMessagePtr& message) {
        if (message->str.size() > MAX_WS_MESSAGE_BYTES) {
            if (!m_oversized_message_logged) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "(%s) Dropping oversized %s WebSocket message (%zu bytes, max %zu); "
                                  "further oversized-message logs suppressed until reconnect\n",
                                  m_sessionId.c_str(), message->binary ? "binary" : "text", message->str.size(),
                                  MAX_WS_MESSAGE_BYTES);
                m_oversized_message_logged = true;
            }
            if (message->binary && m_raw_audio_mode) {
                resetPlaybackDecoderState();
            }
            return;
        }

        if (!message->binary) {
            if (message->str.find('\0') != std::string::npos) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "(%s) Dropping text WebSocket message containing a NUL byte\n", m_sessionId.c_str());
                return;
            }
            eventCallback(MESSAGE, message->str.c_str());
            return;
        }

        if (!m_raw_audio_mode) {
            if (!m_unexpected_binary_logged) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                                  "(%s) Received binary WebSocket frame (%zu bytes) but raw audio mode is not enabled, "
                                  "ignoring; further binary-frame logs suppressed until reconnect\n",
                                  m_sessionId.c_str(), message->str.size());
                m_unexpected_binary_logged = true;
            }
            return;
        }

        std::string aligned_audio;
        auto converted = convertRawAudio(message->str, m_disable_audiofiles ? nullptr : &aligned_audio);
        if (!m_disable_audiofiles && !aligned_audio.empty()) {
            saveDebugAudioFile(aligned_audio, true);
        }
        if (!converted.empty()) {
            m_response_audio_done = false;
            push_audio_queue(std::move(converted));
        }
    }

    void handleConnectionOpen() {
        m_oversized_message_logged = false;
        m_unexpected_binary_logged = false;
        // A new connection starts a new PCM stream.
        resetPlaybackDecoderState();

        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "status", "connected");
        }
        dispatchConnectionEvent(CONNECT_SUCCESS, root);
    }

    void handleConnectionError(const ix::WebSocketErrorInfo& error) {
        const char *reason = error.reason.empty() ? "unknown error" : error.reason.c_str();
        if (m_suppress_log) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "(%s) WebSocket connection error (details suppressed)\n", m_sessionId.c_str());
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "(%s) WebSocket connection error: %s (HTTP status %d, retries %u, retry wait %g ms)\n",
                              m_sessionId.c_str(), reason, error.http_status, error.retries, error.wait_time);
        }

        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "status", "error");
            cJSON *message = cJSON_CreateObject();
            if (message) {
                cJSON_AddNumberToObject(message, "retries", error.retries);
                cJSON_AddStringToObject(message, "error", reason);
                cJSON_AddNumberToObject(message, "wait_time", error.wait_time);
                cJSON_AddNumberToObject(message, "http_status", error.http_status);
                cJSON_AddItemToObject(root, "message", message);
            }
        }
        dispatchConnectionEvent(CONNECT_ERROR, root);
    }

    void handleConnectionClose(const ix::WebSocketCloseInfo& close) {
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "status", "disconnected");
            cJSON *message = cJSON_CreateObject();
            if (message) {
                cJSON_AddNumberToObject(message, "code", close.code);
                cJSON_AddStringToObject(message, "reason", close.reason.c_str());
                cJSON_AddItemToObject(root, "message", message);
            }
        }
        dispatchConnectionEvent(CONNECTION_DROPPED, root);
    }

    void dispatchConnectionEvent(notifyEvent_t event, cJSON *payload) {
        // Takes ownership of payload. On OOM the event is still fired without a JSON body.
        char *json = payload ? cJSON_PrintUnformatted(payload) : nullptr;
        eventCallback(event, json);
        cJSON_Delete(payload);
        switch_safe_free(json);
    }

    inline void request_media_bug_close() {
        // The context outlives the WebSocket thread. Targeting it directly means a late callback
        // can only close its own generation, never a newer bug found through the channel private.
        if (m_context) {
            switch_atomic_set(&m_context->close_requested, 1);
        }
    }

    void eventCallback(notifyEvent_t event, const char *message) {
        switch_core_session_t *psession = switch_core_session_locate(m_sessionId.c_str());
        if (psession) {
            switch (event) {
                case CONNECT_SUCCESS:
                    m_notify(psession, EVENT_CONNECT, message);
                    break;
                case CONNECTION_DROPPED:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection closed\n");
                    m_notify(psession, EVENT_DISCONNECT, message);
                    break;
                case CONNECT_ERROR:
                    m_notify(psession, EVENT_ERROR, message);
                    break;
                case MESSAGE: {
                    std::string msg(message);
                    if (processMessage(psession, msg) != SWITCH_TRUE) {
                        m_notify(psession, EVENT_JSON, msg.c_str());
                    }

                    if (!m_suppress_log) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG,
                                          "Received message: %s\n", msg.c_str());
                    }
                    break;
                }
            }
            switch_core_session_rwunlock(psession);
        }

        if (event == CONNECTION_DROPPED) {
            // Any aggregated capture residue belongs to the dropped connection: have the
            // media thread discard it instead of mixing it with audio sent after a reconnect.
            request_capture_reset();
            if (!webSocket.isAutomaticReconnectionEnabled()) {
                // No more audio can arrive: let write_frame drain the tail, then tear down.
                m_response_audio_done = true;
                m_terminal_close = true;
            }
        } else if (event == CONNECT_ERROR) {
            request_capture_reset();
            if (!webSocket.isAutomaticReconnectionEnabled()) {
                request_media_bug_close();
            }
        }
    }

    std::vector<int16_t> convertRawAudio(const std::string& input_raw, std::string *aligned_raw = nullptr) {
        if (aligned_raw) {
            aligned_raw->clear();
        }
        // Frames are segments of a continuous PCM16 stream: an odd frame length must not shift
        // the sample alignment of the following frames, so carry the trailing byte over
        const char *data = input_raw.data();
        size_t size = input_raw.size();
        std::string stitched;
        if (m_has_pending_raw_byte) {
            stitched.reserve(size + 1);
            stitched.push_back(static_cast<char>(m_pending_raw_byte));
            stitched.append(input_raw);
            data = stitched.data();
            size = stitched.size();
            m_has_pending_raw_byte = false;
        }
        if (size % 2 != 0) {
            m_pending_raw_byte = static_cast<uint8_t>(data[size - 1]);
            m_has_pending_raw_byte = true;
            --size;
        }
        if (size == 0) {
            return {};
        }
        if (aligned_raw) {
            aligned_raw->assign(data, size);
        }
        size_t in_samples = size / 2;

        if (!m_resampler) {
            if (m_playback_input_rate != m_playback_output_rate) {
                // the playback resampler failed to initialize: dropping the audio is safer
                // than feeding the channel at the wrong rate
                return {};
            }
            std::vector<int16_t> buffer(in_samples);
            std::memcpy(buffer.data(), data, size);
            return buffer;
        }

        double scaled = static_cast<double>(in_samples) * m_playback_output_rate / m_playback_input_rate;
        size_t out_samples = static_cast<size_t>(scaled) + 1;

        if (in_samples > UINT32_MAX || out_samples > UINT32_MAX) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Too many samples to resample: in=%zu, out=%zu\n",
                              in_samples, out_samples);
            return {};
        }

        std::vector<int16_t> in_buffer(in_samples);
        std::vector<int16_t> out_buffer(out_samples);

        std::memcpy(in_buffer.data(), data, size);

        spx_uint32_t in_len = static_cast<spx_uint32_t>(in_samples);
        spx_uint32_t out_len = static_cast<spx_uint32_t>(out_samples);

        int err = speex_resampler_process_int(m_resampler, 0, in_buffer.data(), &in_len, out_buffer.data(), &out_len);

        if (err != RESAMPLER_ERR_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Resampling failed with error code: %d\n", err);
            return {};
        }

        out_buffer.resize(out_len);
        return out_buffer;
    }

    // WAV fields and PCM samples use host byte order; supported deployments are little-endian.
    std::string createWavFromRaw(const std::string& rawAudio) {
        const uint16_t numChannels = 1;
        const uint16_t bitsPerSample = 16;
        const uint16_t audioFormat = 1;
        const uint32_t formatChunkSize = 16;
        const uint32_t byteRate = m_playback_input_rate * numChannels * bitsPerSample / 8;
        const uint16_t blockAlign = numChannels * bitsPerSample / 8;
        const uint32_t dataSize = static_cast<uint32_t>(rawAudio.size());
        const uint32_t chunkSize = 36 + dataSize;

        std::ostringstream wavStream;

        wavStream.write("RIFF", 4);
        wavStream.write(reinterpret_cast<const char *>(&chunkSize), 4);
        wavStream.write("WAVE", 4);

        wavStream.write("fmt ", 4);
        wavStream.write(reinterpret_cast<const char *>(&formatChunkSize), 4);
        wavStream.write(reinterpret_cast<const char *>(&audioFormat), 2);
        wavStream.write(reinterpret_cast<const char *>(&numChannels), 2);
        wavStream.write(reinterpret_cast<const char *>(&m_playback_input_rate), 4);
        wavStream.write(reinterpret_cast<const char *>(&byteRate), 4);
        wavStream.write(reinterpret_cast<const char *>(&blockAlign), 2);
        wavStream.write(reinterpret_cast<const char *>(&bitsPerSample), 2);

        wavStream.write("data", 4);
        wavStream.write(reinterpret_cast<const char *>(&dataSize), 4);
        wavStream.write(rawAudio.data(), dataSize);

        return wavStream.str();
    }

    // Returns the created file path, or an empty string on failure (no event is fired then)
    std::string saveDebugAudioFile(const std::string& rawAudio, bool notifyPlaybackEvent = false) {
        // Build the path as a std::string so a long temp dir or session id cannot be truncated
        // into a colliding name by a fixed-size buffer
        std::string filePath = std::string(SWITCH_GLOBAL_dirs.temp_dir) + SWITCH_PATH_SEPARATOR + m_sessionId + "_" +
                               std::to_string(m_playFile++) + ".tmp.wav";

        // The file holds call audio and lives in a possibly shared temp dir: create it exclusively
        // with owner-only permissions, without following symlinks or overwriting existing files
        int fd = open(filePath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) saveDebugAudioFile - cannot create %s: %s\n",
                              m_sessionId.c_str(), filePath.c_str(), strerror(errno));
            return "";
        }
        // Track the file as soon as it exists so cleanup removes it even on a failed write
        m_Files.insert(filePath);

        std::string wavData = createWavFromRaw(rawAudio);
        size_t written = 0;
        while (written < wavData.size()) {
            ssize_t n = write(fd, wavData.data() + written, wavData.size() - written);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "(%s) saveDebugAudioFile - write to %s failed: %s\n", m_sessionId.c_str(),
                                  filePath.c_str(), strerror(errno));
                close(fd);
                return "";
            }
            written += static_cast<size_t>(n);
        }
        close(fd);

        if (notifyPlaybackEvent) {
            switch_core_session_t *psession = switch_core_session_locate(m_sessionId.c_str());
            if (!psession) {
                return filePath;
            }
            cJSON *payload = cJSON_CreateObject();
            if (payload) {
                cJSON_AddStringToObject(payload, "file", filePath.c_str());
                char *jsonString = cJSON_PrintUnformatted(payload);
                if (jsonString) {
                    m_notify(psession, EVENT_PLAY, jsonString);
                    std::free(jsonString);
                }
                cJSON_Delete(payload);
            }
            switch_core_session_rwunlock(psession);
        }

        return filePath;
    }

    void handleSpeechStarted(switch_core_session_t *session) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                          "(%s) processMessage - user speech started, stopping openai audio playback\n",
                          m_sessionId.c_str());
        m_playback_queue.clear();
        request_playback_clear();
        m_response_audio_done = true;
        resetPlaybackDecoderState();
    }

    switch_bool_t handleAudioDelta(switch_core_session_t *session, cJSON *json, std::string& message) {
        const char *json_audio = cJSON_GetObjectCstr(json, "delta");
        if (!json_audio || *json_audio == '\0') {
            resetPlaybackDecoderState();
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "(%s) processMessage - response.output_audio.delta no audio data\n", m_sessionId.c_str());
            return SWITCH_FALSE;
        }

        std::string raw_audio;
        try {
            raw_audio = base64_decode_strict(json_audio);
        } catch (const std::exception& e) {
            resetPlaybackDecoderState();
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "(%s) processMessage - base64 decode error: %s\n", m_sessionId.c_str(), e.what());
            return SWITCH_FALSE;
        }

        // The audio payload was already decoded: strip the base64 from the copies used for
        // events and logs (the README documents EVENT_PLAY as replacing it with the file path).
        for (cJSON *item = json->child; item;) {
            cJSON *next = item->next;
            if (item->string && strcasecmp(item->string, "delta") == 0) {
                cJSON_Delete(cJSON_DetachItemViaPointer(json, item));
            }
            item = next;
        }

        std::string aligned_audio;
        auto resampled = convertRawAudio(raw_audio, m_disable_audiofiles ? nullptr : &aligned_audio);
        bool notify_play = false;
        if (!m_disable_audiofiles && !aligned_audio.empty()) {
            const std::string file_path = saveDebugAudioFile(aligned_audio);
            if (!file_path.empty()) {
                cJSON *json_file = cJSON_CreateString(file_path.c_str());
                if (json_file) {
                    cJSON_AddItemToObject(json, "file", json_file);
                    notify_play = true;
                }
            }
        }

        char *serialized = cJSON_PrintUnformatted(json);
        if (serialized) {
            if (notify_play) {
                m_notify(session, EVENT_PLAY, serialized);
            }
            message.assign(serialized);
            std::free(serialized);
        }

        if (resampled.empty()) {
            return SWITCH_FALSE;
        }
        m_response_audio_done = false;
        push_audio_queue(std::move(resampled));
        return SWITCH_TRUE;
    }

    switch_bool_t processMessage(switch_core_session_t *session, std::string& message) {
        if (stream_protocol::json_depth_exceeded(message.c_str(), MAX_JSON_DEPTH)) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "(%s) processMessage - dropping JSON nested deeper than %d levels\n", m_sessionId.c_str(),
                              MAX_JSON_DEPTH);
            return SWITCH_TRUE; // handled: do not forward the untrusted payload
        }

        cJSON *json = cJSON_ParseWithOpts(message.c_str(), nullptr, 1);
        if (!json) {
            return SWITCH_FALSE;
        }

        switch_bool_t status = SWITCH_FALSE;
        const char *json_type = cJSON_GetObjectCstr(json, "type");
        if (!m_suppress_log) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "processMessage type: %s\n",
                              json_type ? json_type : "null");
        }

        switch (stream_protocol::classify_json_message(json_type)) {
            case stream_protocol::JsonMessageType::Error:
                if (m_suppress_log) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "(%s) WebSocket error response received (payload suppressed)\n",
                                      m_sessionId.c_str());
                } else {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "(%s) WebSocket error response: %s\n", m_sessionId.c_str(), message.c_str());
                }
                break;
            case stream_protocol::JsonMessageType::SpeechStarted:
                handleSpeechStarted(session);
                break;
            case stream_protocol::JsonMessageType::SpeechStopped:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                                  "(%s) processMessage - user speech stopped\n", m_sessionId.c_str());
                break;
            case stream_protocol::JsonMessageType::AudioDelta:
                status = handleAudioDelta(session, json, message);
                break;
            case stream_protocol::JsonMessageType::AudioDone:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                                  "(%s) processMessage - audio done\n", m_sessionId.c_str());
                m_response_audio_done = true;
                resetPlaybackDecoderState();
                break;
            case stream_protocol::JsonMessageType::Unhandled:
                break;
        }

        cJSON_Delete(json);
        return status;
    }

    void push_audio_queue(std::vector<int16_t> audio_data) {
        const auto result = m_playback_queue.push(std::move(audio_data));
        if (result == audio_stream::PlaybackQueue::PushResult::OverflowStarted) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                              "(%s) push_audio_queue: playback queue reached its %zu-second capacity; dropping "
                              "incoming audio that does not fit\n",
                              m_sessionId.c_str(), MAX_PLAYBACK_QUEUE_SECONDS);
        }
    }

    void resetPlaybackDecoderState() {
        m_pending_raw_byte = 0;
        m_has_pending_raw_byte = false;
        if (m_resampler) {
            const int result = speex_resampler_reset_mem(m_resampler);
            if (result != RESAMPLER_ERR_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) failed to reset playback resampler: %s\n",
                                  m_sessionId.c_str(), speex_resampler_strerror(result));
            }
        }
    }

    void disconnect() noexcept {
        if (!m_started.exchange(false)) {
            return;
        }
        try {
            webSocket.stop();
        } catch (const std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) failed to stop WebSocket thread: %s\n",
                              m_sessionId.c_str(), e.what());
        }
    }

    // For all write methods, success means the payload was accepted by the WebSocket client while
    // connected; delivery to the server is not confirmed.
    bool writeAudioDelta(const uint8_t *buffer, size_t len) {
        if (!isConnected())
            return false;

        std::string base64Audio = base64_encode(buffer, len, false);
        if (base64Audio.empty())
            return false;

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "input_audio_buffer.append");
        cJSON_AddStringToObject(root, "audio", base64Audio.c_str());

        char *jsonStr = cJSON_PrintUnformatted(root);
        const bool sent = jsonStr && webSocket.sendUtf8Text(ix::IXWebSocketSendData(jsonStr, strlen(jsonStr))).success;

        cJSON_Delete(root);
        switch_safe_free(jsonStr);
        return sent;
    }

    bool writeBinary(const uint8_t *buffer, size_t len) {
        if (!isConnected())
            return false;
        return webSocket.sendBinary(ix::IXWebSocketSendData(reinterpret_cast<const char *>(buffer), len)).success;
    }

    void deleteFiles() {
        for (const auto& fileName : m_Files) {
            std::remove(fileName.c_str());
        }
    }

    void request_playback_clear() {
        m_playback_clear_gen.fetch_add(1, std::memory_order_release);
    }

    void request_capture_reset() {
        m_capture_reset_pending.store(true, std::memory_order_release);
    }
    std::string m_sessionId;
    responseHandler_t m_notify;
    ix::WebSocket webSocket;
    bool m_suppress_log;
    int m_playFile;
    std::unordered_set<std::string> m_Files;

    uint32_t m_playback_input_rate;
    uint32_t m_playback_output_rate;
    SpeexResamplerState *m_resampler = nullptr;
    audio_stream::PlaybackQueue m_playback_queue;
    std::atomic<uint64_t> m_playback_clear_gen{0};
    uint64_t m_playback_clear_gen_seen = 0; // media (write_frame) thread only
    bool m_disable_audiofiles = false;
    bool m_openai_speaking = false; // media (write_frame) thread only
    std::atomic<bool> m_response_audio_done{false};
    std::atomic<bool> m_terminal_close{false};        // connection closed and no reconnection will be attempted
    std::atomic<bool> m_send_failure_logged{false};   // rate-limits the dropped-audio warning to once per episode
    std::atomic<bool> m_capture_reset_pending{false}; // discard capture predating the next successful connection
    std::atomic<bool> m_started{false};
    bool m_raw_audio_mode = false;
    private_t *m_context = nullptr;      // owner context; valid until the WebSocket thread has been joined
    uint8_t m_pending_raw_byte = 0;      // trailing odd PCM16 byte carried to the next playback message
    bool m_has_pending_raw_byte = false; // WebSocket thread only
    // WebSocket-thread-only latches, reset for each connection.
    bool m_oversized_message_logged = false;
    bool m_unexpected_binary_logged = false;
};

class StreamRuntime {
  public:
    StreamRuntime(const char *session_id, const StreamConfig& config, private_t *owner)
        : m_streamer(new AudioStreamer(session_id, config, owner)) {}

    AudioStreamer *streamer() {
        return m_streamer.get();
    }

    StreamBuffers& buffers() {
        return m_buffers;
    }

  private:
    // Members are destroyed in reverse order: the WebSocket-owning streamer stops before
    // the callback/media scratch buffers disappear.
    StreamBuffers m_buffers;
    std::unique_ptr<AudioStreamer> m_streamer;
};

StreamRuntime *stream_runtime(private_t *data) {
    return data ? static_cast<StreamRuntime *>(data->cpp_context) : nullptr;
}

AudioStreamer *audio_streamer(private_t *data) {
    StreamRuntime *runtime = stream_runtime(data);
    return runtime ? runtime->streamer() : nullptr;
}

using LifecycleMutex = std::mutex;

struct LifecycleLockHandle {
    std::string session_id;
    std::shared_ptr<LifecycleMutex> mutex;
    std::unique_lock<LifecycleMutex> lock;

    LifecycleLockHandle(std::string id, std::shared_ptr<LifecycleMutex> session_mutex)
        : session_id(std::move(id)), mutex(std::move(session_mutex)), lock(*mutex) {}
};

std::mutex lifecycle_registry_mutex;
std::unordered_map<std::string, std::weak_ptr<LifecycleMutex>> lifecycle_mutexes;

LifecycleLockHandle *acquire_lifecycle_lock(switch_core_session_t *session) noexcept {
    try {
        const char *uuid = session ? switch_core_session_get_uuid(session) : nullptr;
        if (!uuid || !*uuid) {
            return nullptr;
        }

        std::shared_ptr<LifecycleMutex> session_mutex;
        {
            std::lock_guard<std::mutex> registry_lock(lifecycle_registry_mutex);
            auto& weak_mutex = lifecycle_mutexes[uuid];
            session_mutex = weak_mutex.lock();
            if (!session_mutex) {
                session_mutex = std::make_shared<LifecycleMutex>();
                weak_mutex = session_mutex;
            }
        }
        return new LifecycleLockHandle(uuid, std::move(session_mutex));
    } catch (const std::exception& e) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "failed to create stream lifecycle lock: %s\n",
                          e.what());
        return nullptr;
    }
}

void release_lifecycle_lock(LifecycleLockHandle *handle) noexcept {
    if (!handle) {
        return;
    }

    handle->lock.unlock();
    {
        std::lock_guard<std::mutex> registry_lock(lifecycle_registry_mutex);
        auto it = lifecycle_mutexes.find(handle->session_id);
        if (it != lifecycle_mutexes.end() && handle->mutex.use_count() == 1 && it->second.lock() == handle->mutex) {
            lifecycle_mutexes.erase(it);
        }
    }
    delete handle;
}

switch_status_t stream_data_init(private_t *tech_pvt, switch_core_session_t *session, const StreamConfig& config) {
    int err = RESAMPLER_ERR_SUCCESS;

    switch_memory_pool_t *pool = switch_core_session_get_pool(session);

    memset(tech_pvt, 0, sizeof(private_t));

    strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID - 1);
    tech_pvt->sessionId[MAX_SESSION_ID - 1] = '\0';
    tech_pvt->sampling = config.capture_output_rate;
    tech_pvt->rtp_packets = config.capture_packet_count;
    tech_pvt->channels = config.channels;
    tech_pvt->suppress_log = config.suppress_log ? SWITCH_TRUE : SWITCH_FALSE;
    switch_atomic_set(&tech_pvt->audio_paused, 0);
    switch_atomic_set(&tech_pvt->user_audio_muted, config.start_muted ? 1 : 0);
    switch_atomic_set(&tech_pvt->openai_audio_muted, 0);
    switch_atomic_set(&tech_pvt->close_requested, 0);

    const size_t buflen = static_cast<size_t>(FRAME_SIZE_8000) * config.capture_output_rate / 8000 * config.channels *
                          config.capture_packet_count;
    if (switch_buffer_create(pool, &tech_pvt->playback_buffer, PLAYBACK_BUFFER_BYTES) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "%s: Error creating playback buffer.\n", tech_pvt->sessionId);
        return SWITCH_STATUS_FALSE;
    }

    // The allocations can throw: contain any exception here so it never crosses the extern "C"
    // boundary. tech_pvt was memset to zero, so the caller's destroy_tech_pvt() safely tears down
    // whatever was already built.
    try {
        tech_pvt->cpp_context = static_cast<void *>(new StreamRuntime(tech_pvt->sessionId, config, tech_pvt));
    } catch (const std::exception& e) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "%s: failed to initialize stream context: %s\n", tech_pvt->sessionId, e.what());
        return SWITCH_STATUS_FALSE;
    }

    if (switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, pool) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error creating mutex.\n",
                          tech_pvt->sessionId);
        return SWITCH_STATUS_FALSE;
    }

    if (switch_buffer_create(pool, &tech_pvt->sbuffer, buflen) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s: Error creating switch buffer.\n",
                          tech_pvt->sessionId);
        return SWITCH_STATUS_FALSE;
    }

    if (config.capture_output_rate != config.capture_input_rate) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) resampling from %u to %u\n",
                          tech_pvt->sessionId, config.capture_input_rate, config.capture_output_rate);
        tech_pvt->resampler = speex_resampler_init(config.channels, config.capture_input_rate,
                                                   config.capture_output_rate, SWITCH_RESAMPLE_QUALITY, &err);
        if (0 != err) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
            return SWITCH_STATUS_FALSE;
        }
    } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "(%s) no resampling needed for this call\n", tech_pvt->sessionId);
    }

    return SWITCH_STATUS_SUCCESS;
}

void destroy_tech_pvt(private_t *tech_pvt) {
    if (tech_pvt->cpp_context) {
        delete stream_runtime(tech_pvt);
        tech_pvt->cpp_context = nullptr;
    }
    if (tech_pvt->resampler) {
        speex_resampler_destroy(tech_pvt->resampler);
        tech_pvt->resampler = nullptr;
    }
    // The session pool owns the mutex: a command may have read this context just
    // before CLOSE detached it and still be waiting to observe the closed state.
}

bool capture_is_paused_or_muted(private_t *tech_pvt) {
    return switch_atomic_read(&tech_pvt->audio_paused) || switch_atomic_read(&tech_pvt->user_audio_muted);
}

// Observe the reset without consuming it: only stream_frame can also flush the media-bug source.
// Callers are serialized by tech_pvt->mutex.
bool discard_capture_residue_on_pending_reset(private_t *tech_pvt, AudioStreamer *streamer) {
    if (streamer && streamer->capture_reset_pending()) {
        if (tech_pvt->sbuffer) {
            switch_buffer_zero(tech_pvt->sbuffer);
        }
        return true;
    }
    return false;
}

bool reset_capture_resampler(private_t *tech_pvt) {
    if (!tech_pvt->resampler) {
        return true;
    }
    const int result = speex_resampler_reset_mem(tech_pvt->resampler);
    if (result == RESAMPLER_ERR_SUCCESS) {
        return true;
    }
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s: Failed to reset capture resampler: %s\n",
                      tech_pvt->sessionId, speex_resampler_strerror(result));
    return false;
}

// Send any residual aggregated capture audio, then clear the aggregator.
// Returns true when a connection reset discarded the residue. Caller must hold tech_pvt->mutex.
bool flush_capture_residue(private_t *tech_pvt) {
    StreamRuntime *runtime = stream_runtime(tech_pvt);
    AudioStreamer *streamer = runtime ? runtime->streamer() : nullptr;
    if (discard_capture_residue_on_pending_reset(tech_pvt, streamer)) {
        return true;
    }
    if (!tech_pvt->sbuffer) {
        return false;
    }
    switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
    if (inuse == 0) {
        return false;
    }
    if (streamer && streamer->isConnected()) {
        StreamBuffers& buffers = runtime->buffers();
        buffers.flush_buffer.resize(inuse);
        switch_buffer_read(tech_pvt->sbuffer, buffers.flush_buffer.data(), inuse);
        streamer->sendAudio(buffers.flush_buffer.data(), inuse);
    }
    switch_buffer_zero(tech_pvt->sbuffer);
    return false;
}

// API operations keep the bug and streamer alive while CLOSE waits on the context mutex.
struct SessionContext {
    SessionContext() = default;
    SessionContext(const SessionContext&) = delete;
    SessionContext& operator=(const SessionContext&) = delete;

    switch_media_bug_t *bug = nullptr;
    private_t *data = nullptr;

    ~SessionContext() {
        if (data) {
            switch_mutex_unlock(data->mutex);
        }
    }
};

bool find_session_context(switch_core_session_t *session, const char *operation, SessionContext& context,
                          stream_error_t *error) {
    if (!session) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s failed: no session found.\n", operation);
        stream_fail(error, "Channel not found");
        return false;
    }

    switch_channel_t *channel = switch_core_session_get_channel(session);
    if (!channel) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s failed: no channel found.\n",
                          operation);
        stream_fail(error, "Channel not found");
        return false;
    }

    context.data = static_cast<private_t *>(switch_channel_get_private(channel, MY_BUG_NAME));
    if (context.data) {
        switch_mutex_lock(context.data->mutex);
        context.bug = context.data->bug;
    }
    if (!context.bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s failed: no media bug found.\n",
                          operation);
        stream_fail(error, "Stream not found");
        return false;
    }
    return true;
}

} // namespace

extern "C" {
void *stream_session_lifecycle_lock(switch_core_session_t *session) {
    return acquire_lifecycle_lock(session);
}

void stream_session_lifecycle_unlock(void *handle) {
    release_lifecycle_lock(static_cast<LifecycleLockHandle *>(handle));
}

int validate_ws_uri(const char *url, char *wsUri) {
    return stream_protocol::validate_ws_uri(url, wsUri, MAX_WS_URI) ? 1 : 0;
}

switch_status_t stream_session_send_json(switch_core_session_t *session, const char *base64_input,
                                         stream_error_t *error) {
    SessionContext context{};
    if (!find_session_context(session, "stream_session_send_json", context, error)) {
        return SWITCH_STATUS_FALSE;
    }

    AudioStreamer *streamer = audio_streamer(context.data);
    if (!streamer) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: AudioStreamer websocket is null.\n");
        return stream_fail(error, "Stream is closing");
    }

    if (!base64_input || strlen(base64_input) == 0) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: input is empty.\n");
        return stream_fail(error, "JSON payload is empty");
    }
    std::string decoded_str;
    try {
        decoded_str = base64_decode_strict(base64_input);
    } catch (const std::exception& e) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: base64 decode error: %s\n", e.what());
        return stream_fail(error, "Invalid Base64 JSON payload");
    }
    if (decoded_str.empty()) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json base64 decode failed.\n");
        return stream_fail(error, "JSON payload is empty");
    }
    if (decoded_str.find('\0') != std::string::npos) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: decoded JSON contains a NUL byte.\n");
        return stream_fail(error, "JSON payload contains a NUL byte");
    }
    if (!stream_protocol::is_valid_utf8(decoded_str.c_str())) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: decoded JSON contains invalid UTF-8.\n");
        return stream_fail(error, "JSON payload is not valid UTF-8");
    }

    if (stream_protocol::json_depth_exceeded(decoded_str.c_str(), MAX_JSON_DEPTH)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: JSON nested deeper than %d levels.\n", MAX_JSON_DEPTH);
        return stream_fail(error, "JSON payload exceeds maximum nesting depth");
    }

    const char *parse_end = decoded_str.c_str();
    cJSON *json_obj = cJSON_ParseWithOpts(decoded_str.c_str(), &parse_end, 1);
    if (!json_obj) {
        if (streamer->suppress_log()) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "stream_session_send_json failed: invalid JSON (details suppressed).\n");
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "stream_session_send_json failed: invalid JSON. Error near: %s\n",
                              parse_end ? parse_end : "unknown");
        }
        return stream_fail(error, "Payload is not a complete JSON value");
    }

    cJSON_Delete(json_obj);

    // The payload can carry sensitive data (instructions, base64 audio): honor STREAM_SUPPRESS_LOG
    if (!streamer->suppress_log()) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "stream_session_send_json: sending JSON: %s\n", decoded_str.c_str());
    }
    if (!streamer->isConnected()) {
        return stream_fail(error, "WebSocket is not connected");
    }
    return streamer->writeText(decoded_str.c_str()) ? SWITCH_STATUS_SUCCESS
                                                    : stream_fail(error, "WebSocket did not accept the JSON message");
}

switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause, stream_error_t *error) {
    SessionContext context{};
    if (!find_session_context(session, "stream_session_pauseresume", context, error)) {
        return SWITCH_STATUS_FALSE;
    }

    switch_core_media_bug_flush(context.bug);
    switch_atomic_set(&context.data->audio_paused, pause ? 1 : 0);
    return SWITCH_STATUS_SUCCESS;
}

switch_status_t stream_session_set_user_mute(switch_core_session_t *session, int mute, stream_error_t *error) {
    SessionContext context{};
    switch_status_t status = SWITCH_STATUS_FALSE;
    if (!find_session_context(session, "stream_session_set_user_mute", context, error)) {
        return status;
    }
    private_t *tech_pvt = context.data;

    status = SWITCH_STATUS_SUCCESS;
    switch_core_media_bug_flush(context.bug);
    const uint32_t new_state = mute ? 1 : 0;
    const uint32_t last_state = switch_atomic_read(&tech_pvt->user_audio_muted);
    switch_atomic_set(&tech_pvt->user_audio_muted, new_state);
    if (last_state == new_state) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "User audio is already %s\n",
                          new_state ? "muted" : "unmuted");
        return status;
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "User audio %s\n",
                      new_state ? "muted" : "unmuted");

    if (new_state) {
        // Deliver the residual pre-mute speech before injecting silence, instead of dropping it
        flush_capture_residue(tech_pvt);
        if (!reset_capture_resampler(tech_pvt)) {
            status = stream_fail(error, "User audio muted; failed to reset the capture resampler");
        }

        AudioStreamer *streamer = audio_streamer(tech_pvt);
        if (streamer && streamer->isConnected()) {
            const size_t channels = tech_pvt->channels > 0 ? static_cast<size_t>(tech_pvt->channels) : 1;
            // Official OpenAI Realtime uses 24 kHz; compatible backends may configure another rate.
            const size_t sample_rate = tech_pvt->sampling > 0 ? static_cast<size_t>(tech_pvt->sampling) : size_t{24000};
            const size_t bytes = channels * sample_rate * sizeof(int16_t);
            std::vector<uint8_t> silence(bytes, 0);
            if (streamer->sendAudio(silence.data(), silence.size())) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                                  "Sent %zu bytes of silence after muting user audio\n", silence.size());
            } else {
                status = stream_fail(error, "User audio muted; WebSocket did not accept mute silence");
            }
        } else {
            status = stream_fail(error, "User audio muted; WebSocket is not connected");
        }
    }

    return status;
}

switch_status_t stream_session_set_openai_mute(switch_core_session_t *session, int mute, stream_error_t *error) {
    SessionContext context{};
    if (!find_session_context(session, "stream_session_set_openai_mute", context, error)) {
        return SWITCH_STATUS_FALSE;
    }
    private_t *tech_pvt = context.data;

    const uint32_t new_state = mute ? 1 : 0;
    const uint32_t last_state = switch_atomic_read(&tech_pvt->openai_audio_muted);
    switch_atomic_set(&tech_pvt->openai_audio_muted, new_state);
    if (last_state == new_state) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "OpenAI audio is already %s\n",
                          new_state ? "muted" : "unmuted");
    } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "OpenAI audio %s\n",
                          new_state ? "muted" : "unmuted");
    }

    return SWITCH_STATUS_SUCCESS;
}

switch_status_t stream_session_init(switch_core_session_t *session, responseHandler_t responseHandler,
                                    const stream_start_options_t *options, void **ppUserData, stream_error_t *error) {
    int deflate = 0, heart_beat = 0;
    bool suppressLog = false;
    int rtp_packets = 1;
    bool no_reconnect = false;
    const char *tls_cafile = NULL;
    const char *tls_keyfile = NULL;
    const char *tls_certfile = NULL;
    const char *openai_api_key = NULL;
    bool tls_disable_hostname_validation = false;
    bool disable_audiofiles = false;
    bool raw_audio_mode = options->force_raw_audio_mode != SWITCH_FALSE;
    StreamConfig config{};

    switch_channel_t *channel = switch_core_session_get_channel(session);

    if (switch_channel_var_true(channel, "STREAM_MESSAGE_DEFLATE")) {
        deflate = 1;
    }

    if (switch_channel_var_true(channel, "STREAM_SUPPRESS_LOG")) {
        suppressLog = true;
    }

    if (switch_channel_var_true(channel, "STREAM_NO_RECONNECT")) {
        no_reconnect = true;
    }

    tls_cafile = switch_channel_get_variable(channel, "STREAM_TLS_CA_FILE");
    tls_keyfile = switch_channel_get_variable(channel, "STREAM_TLS_KEY_FILE");
    tls_certfile = switch_channel_get_variable(channel, "STREAM_TLS_CERT_FILE");
    openai_api_key = switch_channel_get_variable(channel, "STREAM_OPENAI_API_KEY");

    if (switch_channel_var_true(channel, "STREAM_TLS_DISABLE_HOSTNAME_VALIDATION")) {
        tls_disable_hostname_validation = true;
    }
    if (switch_channel_var_true(channel, "STREAM_DISABLE_AUDIOFILES")) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Audio files will not be saved.\n");
        disable_audiofiles = true;
    }

    if (switch_channel_var_true(channel, "STREAM_RAW_AUDIO")) {
        raw_audio_mode = true;
        if (options->force_raw_audio_mode) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "STREAM_RAW_AUDIO is deprecated and unnecessary when using uuid_raw_audio_stream. "
                              "Remove the channel variable; raw audio mode is already enabled by the API.\n");
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "STREAM_RAW_AUDIO is deprecated and will be removed in the next major release. "
                              "Use uuid_raw_audio_stream <uuid> start ... to enable raw audio mode.\n");
        }
    }

    if (raw_audio_mode) {
        const char *raw_audio_source = options->force_raw_audio_mode ? "API" : "deprecated channel variable";
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                          "Raw audio mode enabled via %s, bypassing JSON+base64 encoding.\n", raw_audio_source);
    }

    const char *heartBeat = switch_channel_get_variable(channel, "STREAM_HEART_BEAT");
    if (heartBeat) {
        char *endptr;
        long value = strtol(heartBeat, &endptr, 10);
        if (*endptr == '\0' && endptr != heartBeat && value > 0 && value <= MAX_HEARTBEAT_SECONDS) {
            heart_beat = (int)value;
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "%s: STREAM_HEART_BEAT of %s is not between 1 and %d seconds. Ignoring.\n",
                              switch_channel_get_name(channel), heartBeat, MAX_HEARTBEAT_SECONDS);
        }
    }

    const char *buffer_size = switch_channel_get_variable(channel, "STREAM_BUFFER_SIZE");
    if (buffer_size) {
        char *endptr;
        long bSize = strtol(buffer_size, &endptr, 10);
        if (*endptr != '\0' || endptr == buffer_size || bSize < 20 || bSize > MAX_STREAM_BUFFER_MS || bSize % 20 != 0) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "%s: Buffer size of %s is not a multiple of 20ms between 20 and %d. Using default "
                              "20ms.\n",
                              switch_channel_get_name(channel), buffer_size, MAX_STREAM_BUFFER_MS);
        } else {
            rtp_packets = (int)(bSize / 20);
        }
    }

    // Build the transport headers once; keep allocation failures inside the C++ boundary.
    try {
        const char *configured_extra = switch_channel_get_variable(channel, "STREAM_EXTRA_HEADERS");
        if (configured_extra) {
            std::unique_ptr<cJSON, decltype(&cJSON_Delete)> headers_json(cJSON_Parse(configured_extra), cJSON_Delete);
            if (!headers_json || headers_json->type != cJSON_Object) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                                  openai_api_key ? SWITCH_LOG_WARNING : SWITCH_LOG_ERROR,
                                  "STREAM_EXTRA_HEADERS is not a valid JSON object, ignoring it.\n");
            } else {
                for (cJSON *header = headers_json->child; header; header = header->next) {
                    if (header->type != cJSON_String || !header->valuestring || !header->string || !*header->string) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                          "Skipping extra header with invalid name or non-string value\n");
                        continue;
                    }
                    if (std::strpbrk(header->string, "\r\n") || std::strpbrk(header->valuestring, "\r\n")) {
                        return stream_fail(error, "STREAM_EXTRA_HEADERS names and values must not contain CR or LF");
                    }
                    config.extra_headers[header->string] = header->valuestring;
                }
            }
        }

        if (openai_api_key) {
            if (std::strpbrk(openai_api_key, "\r\n")) {
                return stream_fail(error, "STREAM_OPENAI_API_KEY must not contain CR or LF");
            }
            // IXWebSocket compares header names case-insensitively; the API key takes precedence.
            config.extra_headers["Authorization"] = "Bearer " + std::string(openai_api_key);
        } else if (config.extra_headers.find("Authorization") == config.extra_headers.end()) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                              "STREAM_OPENAI_API_KEY is not set; no Authorization header configured.\n");
        }
    } catch (const std::exception& e) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "Failed to build WebSocket headers: %s\n", e.what());
        return stream_fail(error, "Failed to build WebSocket headers");
    }

    auto *tech_pvt = static_cast<private_t *>(switch_core_session_alloc(session, sizeof(private_t)));

    if (!tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
        return SWITCH_STATUS_FALSE;
    }
    // The playback path replaces frames on the write side: resample to the write codec rate, which
    // can differ from the read rate on asymmetric sessions
    uint32_t playback_target_rate = options->capture_input_rate;
    switch_codec_t *write_codec = switch_core_session_get_write_codec(session);
    if (write_codec && write_codec->implementation) {
        playback_target_rate = write_codec->implementation->actual_samples_per_second;
    }
    if (playback_target_rate != options->capture_input_rate) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "playback target rate %u differs from read rate %u\n", playback_target_rate,
                          options->capture_input_rate);
    }

    config.websocket_uri = options->websocket_uri;
    config.capture_input_rate = options->capture_input_rate;
    config.capture_output_rate = options->capture_output_rate;
    config.playback_input_rate = options->playback_input_rate;
    config.playback_output_rate = playback_target_rate;
    config.channels = options->channels;
    config.response_handler = responseHandler;
    config.disable_per_message_deflate = deflate != 0;
    config.heartbeat_seconds = heart_beat;
    config.suppress_log = suppressLog;
    config.capture_packet_count = rtp_packets;
    config.disable_reconnect = no_reconnect;
    config.tls_ca_file = tls_cafile;
    config.tls_key_file = tls_keyfile;
    config.tls_cert_file = tls_certfile;
    config.disable_tls_hostname_validation = tls_disable_hostname_validation;
    config.disable_audio_files = disable_audiofiles;
    config.start_muted = options->start_muted != SWITCH_FALSE;
    config.raw_audio_mode = raw_audio_mode;

    if (stream_data_init(tech_pvt, session, config) != SWITCH_STATUS_SUCCESS) {
        destroy_tech_pvt(tech_pvt);
        return SWITCH_STATUS_FALSE;
    }

    *ppUserData = tech_pvt;

    return SWITCH_STATUS_SUCCESS;
}

void stream_session_release(void *pUserData) {
    if (pUserData) {
        destroy_tech_pvt(static_cast<private_t *>(pUserData));
    }
}

switch_status_t stream_session_start(void *pUserData) {
    if (!pUserData) {
        return SWITCH_STATUS_FALSE;
    }
    auto *tech_pvt = static_cast<private_t *>(pUserData);
    AudioStreamer *streamer = audio_streamer(tech_pvt);
    return streamer && streamer->start() ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

switch_bool_t stream_frame(switch_media_bug_t *bug) {
    auto *tech_pvt = static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt || capture_is_paused_or_muted(tech_pvt))
        return SWITCH_TRUE;

    // READ holds the media bug's read_mutex; control commands hold this mutex before flushing the bug.
    // A blocking lock here would invert that order and deadlock capture against pause or user mute.
    if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
        return SWITCH_TRUE;
    }

    // A control command may have changed state after the lock-free fast-path check above.
    if (capture_is_paused_or_muted(tech_pvt)) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
    }

    StreamRuntime *runtime = stream_runtime(tech_pvt);
    AudioStreamer *streamer = runtime ? runtime->streamer() : nullptr;

    if (!streamer || !streamer->isConnected()) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
    }

    // Drop every layer that can retain capture from the previous connection.
    if (streamer->consume_capture_reset()) {
        if (tech_pvt->sbuffer) {
            switch_buffer_zero(tech_pvt->sbuffer);
        }
        switch_core_media_bug_flush(bug);
        reset_capture_resampler(tech_pvt);
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                          "(%s) discarded stale capture state after WebSocket reconnect\n", tech_pvt->sessionId);
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
    }

    StreamBuffers& buffers = runtime->buffers();

    auto send_or_buffer_audio = [tech_pvt, streamer](const uint8_t *data, size_t length) {
        if (discard_capture_residue_on_pending_reset(tech_pvt, streamer)) {
            return false;
        }

        if (tech_pvt->rtp_packets == 1) {
            streamer->sendAudio(data, length);
            return true;
        }

        while (length > 0) {
            switch_size_t free_space = switch_buffer_freespace(tech_pvt->sbuffer);
            if (free_space == 0) {
                if (flush_capture_residue(tech_pvt)) {
                    return false;
                }
                free_space = switch_buffer_freespace(tech_pvt->sbuffer);
                if (free_space == 0) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                      "%s: Audio buffer has no free space after flush\n", tech_pvt->sessionId);
                    return false;
                }
            }

            switch_size_t write_len = std::min<switch_size_t>(length, free_space);
            if (switch_buffer_write(tech_pvt->sbuffer, data, write_len) == 0) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s: Failed to write audio to stream buffer\n",
                                  tech_pvt->sessionId);
                return false;
            }

            data += write_len;
            length -= write_len;
            if (switch_buffer_freespace(tech_pvt->sbuffer) == 0) {
                if (flush_capture_residue(tech_pvt)) {
                    return false;
                }
            }
        }

        return true;
    };

    switch_frame_t frame{};
    frame.data = buffers.data_buf.data();
    frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

    bool capture_aborted = false;
    while (!capture_aborted && switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
        if (frame.datalen == 0 || frame.samples == 0) {
            continue;
        }

        if (!tech_pvt->resampler) {
            if (!send_or_buffer_audio(static_cast<const uint8_t *>(frame.data), frame.datalen)) {
                break;
            }
            continue;
        }

        spx_uint32_t input_rate = 0;
        spx_uint32_t output_rate = 0;
        speex_resampler_get_rate(tech_pvt->resampler, &input_rate, &output_rate);
        if (input_rate == 0 || output_rate == 0) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s: Invalid resampler rate %u -> %u\n",
                              tech_pvt->sessionId, input_rate, output_rate);
            continue;
        }

        const uint64_t estimated_output =
            (static_cast<uint64_t>(frame.samples) * output_rate + input_rate - 1) / input_rate;
        const spx_uint32_t output_capacity = static_cast<spx_uint32_t>(estimated_output + 1);
        buffers.resample_buffer.resize(static_cast<size_t>(output_capacity) * tech_pvt->channels);

        const auto *input = static_cast<const spx_int16_t *>(frame.data);
        spx_uint32_t remaining_samples = frame.samples;
        while (remaining_samples > 0) {
            spx_uint32_t in_len = remaining_samples;
            spx_uint32_t out_len = output_capacity;
            int result;

            if (tech_pvt->channels == 1) {
                result = speex_resampler_process_int(tech_pvt->resampler, 0, input, &in_len,
                                                     buffers.resample_buffer.data(), &out_len);
            } else {
                result = speex_resampler_process_interleaved_int(tech_pvt->resampler, input, &in_len,
                                                                 buffers.resample_buffer.data(), &out_len);
            }

            if (result != RESAMPLER_ERR_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s: Resampling failed: %s\n",
                                  tech_pvt->sessionId, speex_resampler_strerror(result));
                break;
            }

            size_t bytes_written = static_cast<size_t>(out_len) * tech_pvt->channels * sizeof(spx_int16_t);
            if (bytes_written > 0 &&
                !send_or_buffer_audio(reinterpret_cast<const uint8_t *>(buffers.resample_buffer.data()),
                                      bytes_written)) {
                capture_aborted = true;
                break;
            }

            if (in_len == 0 && out_len == 0) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "%s: Resampler made no progress with %u input samples remaining\n",
                                  tech_pvt->sessionId, remaining_samples);
                break;
            }

            input += static_cast<size_t>(in_len) * tech_pvt->channels;
            remaining_samples -= in_len;
        }
        if (capture_aborted) {
            break;
        }
    }

    switch_mutex_unlock(tech_pvt->mutex);
    return SWITCH_TRUE;
}

switch_bool_t write_frame(switch_core_session_t *session, switch_media_bug_t *bug) {
    private_t *tech_pvt = static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt) {
        return SWITCH_TRUE;
    }

    AudioStreamer *as = audio_streamer(tech_pvt);
    if (switch_atomic_read(&tech_pvt->audio_paused)) {
        // A paused stream cannot drain queued playback. Once a non-reconnecting peer is gone,
        // keeping the media bug alive would leave the session permanently unable to restart.
        if (as && as->is_terminally_closed()) {
            switch_atomic_set(&tech_pvt->close_requested, 1);
        }
        return SWITCH_TRUE;
    }

    switch_frame_t *frame = switch_core_media_bug_get_write_replace_frame(bug);
    auto codec = switch_core_session_get_write_codec(session);
    if (!frame || !codec || !codec->implementation) {
        return SWITCH_TRUE;
    }

    // No isConnected() check: queued audio must keep draining after the connection drops
    if (!as) {
        return SWITCH_TRUE;
    }

    if (frame->samples == 0 || frame->datalen == 0) {
        return SWITCH_TRUE;
    }

    uint32_t bytes_needed = frame->datalen;
    uint32_t bytes_per_sample = frame->datalen / frame->samples;

    if (bytes_needed > frame->buflen) {
        bytes_needed = frame->buflen;
    }

    uint32_t inuse = switch_buffer_inuse(tech_pvt->playback_buffer);

    if (as->consume_playback_clear()) {
        switch_buffer_zero(tech_pvt->playback_buffer);
        inuse = 0;
        // barge-in interrupts the response: close the speaking state so the next one emits a new start
        if (as->is_openai_speaking()) {
            as->openai_speech_stopped(session);
        }
    }
    bool chunk_enqueued = false;
    // Snapshot before the pop attempt: if the close is already visible here, all audio was already pushed
    const bool terminal_close = as->is_terminally_closed();
    while (inuse < bytes_needed * 2) {
        std::vector<int16_t> chunk;
        if (!as->pop_audio_queue(chunk)) {
            break;
        }
        if (switch_buffer_write(tech_pvt->playback_buffer, chunk.data(), chunk.size() * sizeof(int16_t)) == 0) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "write_frame: playback buffer full, dropping audio chunk\n");
            break;
        }
        inuse = switch_buffer_inuse(tech_pvt->playback_buffer);
        chunk_enqueued = true;
    }
    if (!chunk_enqueued && inuse == 0) {
        // A completed response stops speaking only after its final queued sample drains.
        if (as->is_openai_speaking() && as->is_response_audio_done()) {
            as->openai_speech_stopped(session);
        }
        if (terminal_close) {
            // Nothing left to play and no new audio can arrive: tear down the stream
            switch_atomic_set(&tech_pvt->close_requested, 1);
        }
        return SWITCH_TRUE;
    }

    if (inuse > bytes_needed) {
        inuse = bytes_needed;
    }

    if (switch_atomic_read(&tech_pvt->openai_audio_muted)) {
        switch_buffer_toss(tech_pvt->playback_buffer, inuse);
    } else {
        switch_byte_t *data = static_cast<switch_byte_t *>(frame->data);

        switch_buffer_read(tech_pvt->playback_buffer, data, inuse);
        if (inuse < bytes_needed) {
            // preserve the frame duration for the encoder/RTP path: pad the missing tail
            // with silence instead of emitting a short frame
            memset(data + inuse, 0, bytes_needed - inuse);
        }

        if (!as->is_openai_speaking()) {
            as->openai_speech_started(session);
        }

        frame->datalen = bytes_needed;
        frame->samples = bytes_needed / bytes_per_sample;

        switch_core_media_bug_set_write_replace_frame(bug, frame);
    }

    return SWITCH_TRUE;
}

void stream_session_close(switch_core_session_t *session, void *user_data) {
    auto *tech_pvt = static_cast<private_t *>(user_data);
    switch_channel_t *channel = switch_core_session_get_channel(session);

    // FreeSWITCH can invoke CLOSE under bug_rwlock. Never acquire the API lifecycle
    // lock here: start/stop hold it while entering FreeSWITCH's media-bug functions.
    switch_mutex_lock(tech_pvt->mutex);
    flush_capture_residue(tech_pvt);
    if (switch_channel_get_private(channel, MY_BUG_NAME) == tech_pvt) {
        switch_channel_set_private(channel, MY_BUG_NAME, nullptr);
    }
    tech_pvt->bug = nullptr;
    destroy_tech_pvt(tech_pvt);
    switch_mutex_unlock(tech_pvt->mutex);
}

switch_status_t stream_session_cleanup(switch_core_session_t *session, const char *text, stream_error_t *error) {
    // The API lifecycle lock serializes stop with start. CLOSE instead uses the
    // context mutex, which must be released before acquiring FreeSWITCH bug_rwlock.
    private_t *tech_pvt;
    switch_atomic_t was_paused;
    switch_status_t status = SWITCH_STATUS_SUCCESS;
    stream_error_t payload_error = {};
    {
        SessionContext context{};
        if (!find_session_context(session, "stream_session_cleanup", context, error)) {
            return SWITCH_STATUS_FALSE;
        }
        tech_pvt = context.data;
        // Keep capture stopped after releasing the mutex: no audio may follow the final JSON.
        was_paused = switch_atomic_read(&tech_pvt->audio_paused);
        switch_atomic_set(&tech_pvt->audio_paused, 1);
        flush_capture_residue(tech_pvt);
        if (text && *text) {
            status = stream_session_send_json(session, text, &payload_error);
        }
    }

    // Removal by function avoids dereferencing a bug that a concurrent hangup may
    // already have destroyed. API serialization prevents removing a new stream.
    switch_core_media_bug_remove_all_function(session, MY_BUG_NAME);
    switch_mutex_lock(tech_pvt->mutex);
    const bool removed = tech_pvt->bug == nullptr;
    if (!removed) {
        switch_atomic_set(&tech_pvt->audio_paused, was_paused);
    }
    switch_mutex_unlock(tech_pvt->mutex);
    if (!removed) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_cleanup: failed to remove media bug\n");
        return stream_fail(error, "Failed to stop stream");
    }
    if (status != SWITCH_STATUS_SUCCESS && error) {
        switch_snprintf(error->message, sizeof(error->message), "Stream stopped; final message failed: %s",
                        payload_error.message);
    }
    return status;
}
}
