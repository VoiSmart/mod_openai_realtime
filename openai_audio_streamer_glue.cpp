#include <string>
#include <cstring>
#include "openai_audio_streamer_glue.h"
#include <ixwebsocket/IXWebSocket.h>
#include <sstream>
#include <queue>
#include <algorithm>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include <switch_json.h>
#include <switch_buffer.h>
#include <unordered_map>
#include <unordered_set>
#include "base64.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include "stream_protocol.h"

#define FRAME_SIZE_8000 320 /* 1000x0.02 (20ms)= 160 x(16bit= 2 bytes) 320 frame size*/
#define MAX_AUDIO_CHUNK_SAMPLES                                                                                        \
    16384 /* max samples per queue entry (~32KB), keeps chunks within playback buffer capacity */
#define MAX_PLAYBACK_QUEUE_SECONDS 180 /* overload guard: incoming playback audio beyond this backlog is dropped */
#define MAX_WS_MESSAGE_BYTES                                                                                           \
    (8 * 1024 * 1024)              /* overload guard: peer messages beyond this are dropped before copying/parsing */
#define MAX_JSON_DEPTH 128         /* guards the recursive cJSON parser against deeply nested peer JSON */
#define MAX_STREAM_BUFFER_MS 1000  /* upper bound for the STREAM_BUFFER_SIZE capture aggregation window */
#define MAX_HEARTBEAT_SECONDS 3600 /* upper bound for the STREAM_HEART_BEAT ping interval */

// Persistent buffers for stream_frame to avoid per-frame heap allocations
struct StreamBuffers {
    std::vector<uint8_t> flush_buffer;
    std::vector<spx_int16_t> resample_buffer;
    std::vector<uint8_t> data_buf;

    StreamBuffers() {
        flush_buffer.reserve(SWITCH_RECOMMENDED_BUFFER_SIZE);
        resample_buffer.reserve(SWITCH_RECOMMENDED_BUFFER_SIZE / sizeof(spx_int16_t));
        data_buf.resize(SWITCH_RECOMMENDED_BUFFER_SIZE);
    }
};

class AudioStreamer {
  public:
    AudioStreamer(const char *uuid, const char *wsUri, responseHandler_t callback, int deflate, int heart_beat,
                  bool suppressLog, const char *extra_headers, bool no_reconnect, const char *tls_cafile,
                  const char *tls_keyfile, const char *tls_certfile, bool tls_disable_hostname_validation,
                  uint32_t session_sampling, uint32_t playback_sampling, bool disable_audiofiles, bool raw_audio_mode,
                  private_t *context)
        : m_sessionId(uuid), m_notify(callback), m_suppress_log(suppressLog), m_playFile(0),
          m_disable_audiofiles(disable_audiofiles), m_raw_audio_mode(raw_audio_mode), m_context(context) {

        in_sample_rate = playback_sampling;

        ix::WebSocketHttpHeaders headers;
        ix::SocketTLSOptions tlsOptions;
        if (extra_headers) {
            cJSON *headers_json = cJSON_Parse(extra_headers);
            if (!headers_json || headers_json->type != cJSON_Object) {
                // misconfigured headers lead to hard-to-diagnose auth failures: make it visible
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "(%s) Extra headers are not a valid JSON object, ignoring them\n",
                                  m_sessionId.c_str());
            } else {
                for (cJSON *iterator = headers_json->child; iterator; iterator = iterator->next) {
                    // iterator->string is null for array elements or malformed properties
                    if (iterator->type == cJSON_String && iterator->valuestring != nullptr &&
                        iterator->string != nullptr && *iterator->string != '\0') {
                        headers[iterator->string] = iterator->valuestring;
                    } else {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                                          "(%s) Skipping extra header with invalid name or non-string value\n",
                                          m_sessionId.c_str());
                    }
                }
            }
            cJSON_Delete(headers_json);
        }

        webSocket.setUrl(wsUri);

        // Setup eventual TLS options.
        // tls_cafile may hold the special values
        // NONE, which disables validation and SYSTEM which uses
        // the system CAs bundle
        if (tls_cafile) {
            tlsOptions.caFile = tls_cafile;
        }

        if (tls_keyfile) {
            tlsOptions.keyFile = tls_keyfile;
        }

        if (tls_certfile) {
            tlsOptions.certFile = tls_certfile;
        }

        tlsOptions.disable_hostname_validation = tls_disable_hostname_validation;
        webSocket.setTLSOptions(tlsOptions);

        // Optional heart beat, sent every xx seconds when there is not any traffic
        // to make sure that load balancers do not kill an idle connection.
        if (heart_beat)
            webSocket.setPingInterval(heart_beat);

        // Per message deflate connection is enabled by default. You can tweak its parameters or disable it
        if (deflate)
            webSocket.disablePerMessageDeflate();

        // Set extra headers if any
        if (!headers.empty())
            webSocket.setExtraHeaders(headers);

        if (no_reconnect)
            webSocket.disableAutomaticReconnection();

        // Setup a callback to be fired when a message or an event (open, close, error) is received
        webSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            if (msg->type == ix::WebSocketMessageType::Message) {
                if (msg->str.size() > MAX_WS_MESSAGE_BYTES) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                      "(%s) Dropping oversized %s WebSocket message (%zu bytes, max %d)\n",
                                      m_sessionId.c_str(), msg->binary ? "binary" : "text", msg->str.size(),
                                      MAX_WS_MESSAGE_BYTES);
                    return;
                }
                if (msg->binary) {
                    if (m_raw_audio_mode) {
                        if (!m_disable_audiofiles) {
                            saveDebugAudioFile(msg->str, true);
                        }
                        auto converted = convertRawAudio(msg->str);
                        if (!converted.empty()) {
                            m_response_audio_done = false;
                            push_audio_queue(converted);
                        }
                    } else {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                                          "(%s) Received binary WebSocket frame (%zu bytes) but raw audio mode is not "
                                          "enabled, ignoring\n",
                                          m_sessionId.c_str(), msg->str.size());
                    }
                } else {
                    eventCallback(MESSAGE, msg->str.c_str());
                }

            } else if (msg->type == ix::WebSocketMessageType::Open) {
                // A new connection starts a new PCM stream: forget any half-sample carried over
                m_has_pending_raw_byte = false;

                // On OOM the event is still fired, just without a JSON body
                cJSON *root = cJSON_CreateObject();
                char *json_str = nullptr;
                if (root) {
                    cJSON_AddStringToObject(root, "status", "connected");
                    json_str = cJSON_PrintUnformatted(root);
                }

                eventCallback(CONNECT_SUCCESS, json_str);

                cJSON_Delete(root);
                switch_safe_free(json_str);

            } else if (msg->type == ix::WebSocketMessageType::Error) {
                // A message will be fired when there is an error with the connection. The message type will be
                // ix::WebSocketMessageType::Error.
                //  Multiple fields will be inuse on the event to describe the error.
                cJSON *root = cJSON_CreateObject();
                char *json_str = nullptr;
                if (root) {
                    cJSON_AddStringToObject(root, "status", "error");
                    cJSON *message = cJSON_CreateObject();
                    if (message) {
                        cJSON_AddNumberToObject(message, "retries", msg->errorInfo.retries);
                        cJSON_AddStringToObject(message, "error", msg->errorInfo.reason.c_str());
                        cJSON_AddNumberToObject(message, "wait_time", msg->errorInfo.wait_time);
                        cJSON_AddNumberToObject(message, "http_status", msg->errorInfo.http_status);
                        cJSON_AddItemToObject(root, "message", message);
                    }
                    json_str = cJSON_PrintUnformatted(root);
                }

                eventCallback(CONNECT_ERROR, json_str);

                cJSON_Delete(root);
                switch_safe_free(json_str);
            } else if (msg->type == ix::WebSocketMessageType::Close) {
                // The server can send an explicit code and reason for closing.
                // This data can be accessed through the closeInfo object.
                cJSON *root = cJSON_CreateObject();
                char *json_str = nullptr;
                if (root) {
                    cJSON_AddStringToObject(root, "status", "disconnected");
                    cJSON *message = cJSON_CreateObject();
                    if (message) {
                        cJSON_AddNumberToObject(message, "code", msg->closeInfo.code);
                        cJSON_AddStringToObject(message, "reason", msg->closeInfo.reason.c_str());
                        cJSON_AddItemToObject(root, "message", message);
                    }
                    json_str = cJSON_PrintUnformatted(root);
                }

                eventCallback(CONNECTION_DROPPED, json_str);

                cJSON_Delete(root);
                switch_safe_free(json_str);
            }
        });

        out_sample_rate = session_sampling;
        if (in_sample_rate != out_sample_rate) {
            int err = 0;
            m_resampler = speex_resampler_init(1, in_sample_rate, out_sample_rate, SWITCH_RESAMPLE_QUALITY, &err);
            if (!m_resampler || err != RESAMPLER_ERR_SUCCESS) {
                // convertRawAudio drops incoming audio in this state rather than playing it at the wrong rate
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                                  "(%s) Error initializing playback resampler %d -> %d: %s\n", m_sessionId.c_str(),
                                  in_sample_rate, out_sample_rate, speex_resampler_strerror(err));
            }
        }

        // The WebSocket starts only after the media bug and channel private have been published.
        // This closes the startup window where callbacks could not find their own session context.
    }

    bool start() {
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

                    // Any aggregated capture residue belongs to the dropped connection: have the
                    // media thread discard it instead of mixing it with audio sent after a reconnect
                    request_capture_reset();

                    if (!webSocket.isAutomaticReconnectionEnabled()) {
                        // No more audio can arrive: let write_frame drain the tail, then tear down
                        m_response_audio_done = true;
                        m_terminal_close = true;
                    }

                    break;
                case CONNECT_ERROR:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection error\n");
                    m_notify(psession, EVENT_ERROR, message);

                    if (!webSocket.isAutomaticReconnectionEnabled()) {
                        request_media_bug_close();
                    }

                    break;
                case MESSAGE:
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
            switch_core_session_rwunlock(psession);
        }
    }

    std::vector<int16_t> convertRawAudio(const std::string& input_raw) {
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
        size_t usable_bytes = size;
        size_t in_samples = usable_bytes / 2;

        if (!m_resampler) {
            if (in_sample_rate != out_sample_rate) {
                // the playback resampler failed to initialize: dropping the audio is safer
                // than feeding the channel at the wrong rate
                return {};
            }
            std::vector<int16_t> buffer(in_samples);
            std::memcpy(buffer.data(), data, usable_bytes);
            return buffer;
        }

        double scaled = static_cast<double>(in_samples) * out_sample_rate / in_sample_rate;
        size_t out_samples = static_cast<size_t>(scaled) + 1;

        if (in_samples > UINT32_MAX || out_samples > UINT32_MAX) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Too many samples to resample: in=%zu, out=%zu\n",
                              in_samples, out_samples);
            return {};
        }

        std::vector<int16_t> in_buffer(in_samples);
        std::vector<int16_t> out_buffer(out_samples);

        std::memcpy(in_buffer.data(), data, usable_bytes);

        spx_uint32_t in_len = static_cast<spx_uint32_t>(in_samples);
        spx_uint32_t out_len = static_cast<spx_uint32_t>(out_samples);

        int err = speex_resampler_process_int(m_resampler, 0, in_buffer.data(), &in_len, out_buffer.data(), &out_len);

        if (err != RESAMPLER_ERR_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Resampling failed with error code: %d\n", err);
            return {}; // empty on error
        }

        out_buffer.resize(out_len); // resize to actual resampled size
        return out_buffer;
    }

    // create wav file from raw audio
    // rawAudio passed as constant reference because it is never edited
    // Note: the header multi-byte fields and PCM samples are written host-endian; this debug
    // helper assumes a little-endian host, which matches all supported deployment platforms.
    std::string createWavFromRaw(const std::string& rawAudio) {

        const int numChannels = 1;    // mono
        const int bitsPerSample = 16; // pcm16
        int byteRate = in_sample_rate * numChannels * bitsPerSample / 8;
        int blockAlign = numChannels * bitsPerSample / 8;
        uint32_t dataSize = static_cast<uint32_t>(rawAudio.size());
        uint32_t chunkSize = 36 + dataSize;

        std::ostringstream wavStream; // write in string like stream

        // creating wav header
        // riff header
        wavStream.write("RIFF", 4);                                     // chunk id
        wavStream.write(reinterpret_cast<const char *>(&chunkSize), 4); // chunk Size
        wavStream.write("WAVE", 4);

        // this subchunk contains format infos
        wavStream.write("fmt ", 4);
        uint32_t subchunk1Size = 16;
        wavStream.write(reinterpret_cast<const char *>(&subchunk1Size), 4);
        uint16_t audioFormat = 1; // 1 is pcm
        wavStream.write(reinterpret_cast<const char *>(&audioFormat), 2);
        wavStream.write(reinterpret_cast<const char *>(&numChannels), 2);
        wavStream.write(reinterpret_cast<const char *>(&in_sample_rate), 4);
        wavStream.write(reinterpret_cast<const char *>(&byteRate), 4);
        wavStream.write(reinterpret_cast<const char *>(&blockAlign), 2);
        wavStream.write(reinterpret_cast<const char *>(&bitsPerSample), 2);

        // data subchunk contains audio data
        wavStream.write("data", 4);
        wavStream.write(reinterpret_cast<const char *>(&dataSize), 4);
        wavStream.write(rawAudio.data(), dataSize);

        return wavStream.str();
    }

    // Returns the created file path, or an empty string on failure (no event is fired then)
    std::string saveDebugAudioFile(const std::string& rawAudio, bool notifyPlaybackEvent = false) {
        // Build the path as a std::string so a long temp dir or session id cannot be truncated
        // into a colliding name by a fixed-size buffer
        const std::string filePath = std::string(SWITCH_GLOBAL_dirs.temp_dir) + SWITCH_PATH_SEPARATOR + m_sessionId +
                                     "_" + std::to_string(m_playFile++) + ".tmp.wav";

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

        switch_core_session_t *psession = switch_core_session_locate(m_sessionId.c_str());
        if (notifyPlaybackEvent && psession) {
            cJSON *payload = cJSON_CreateObject();
            if (payload) {
                cJSON_AddStringToObject(payload, "file", filePath.c_str());
                char *jsonString = cJSON_PrintUnformatted(payload);
                if (jsonString) {
                    m_notify(psession, EVENT_PLAY, jsonString);
                    free(jsonString);
                }
                cJSON_Delete(payload);
            }
        }
        if (psession) {
            switch_core_session_rwunlock(psession);
        }

        return filePath;
    }

    switch_bool_t processMessage(switch_core_session_t *session, std::string& message) {
        switch_bool_t status = SWITCH_FALSE;
        if (stream_protocol::json_depth_exceeded(message.c_str(), MAX_JSON_DEPTH)) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "(%s) processMessage - dropping JSON nested deeper than %d levels\n", m_sessionId.c_str(),
                              MAX_JSON_DEPTH);
            // report as handled so the raw payload is not forwarded to event subscribers
            return SWITCH_TRUE;
        }
        cJSON *json = cJSON_Parse(message.c_str());
        if (!json) {
            return status;
        }

        const char *jsType = cJSON_GetObjectCstr(json, "type");
        if (!m_suppress_log) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "processMessage type: %s\n",
                              jsType ? jsType : "null");
        }
        if (!jsType) {
            cJSON_Delete(json);
            return status;
        }

        switch (stream_protocol::classify_json_message(jsType)) {
            case stream_protocol::JsonMessageType::Error:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                  "(%s) processMessage - error: %s\n", m_sessionId.c_str(), message.c_str());
                break;

            case stream_protocol::JsonMessageType::SpeechStarted:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                                  "(%s) processMessage - user speech started, stopping openai audio playback\n",
                                  m_sessionId.c_str());
                clear_audio_queue();
                // also clear the private_t playback buffer used in write frame
                request_playback_clear();
                break;

            case stream_protocol::JsonMessageType::SpeechStopped:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                                  "(%s) processMessage - user speech stopped\n", m_sessionId.c_str());
                break;

            case stream_protocol::JsonMessageType::AudioDelta: {
                const char *jsonAudio = cJSON_GetObjectCstr(json, "delta");
                m_response_audio_done = false;

                if (jsonAudio && strlen(jsonAudio) > 0) {
                    std::string rawAudio;
                    try {
                        rawAudio = base64_decode(jsonAudio);
                    } catch (const std::exception& e) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                          "(%s) processMessage - base64 decode error: %s\n", m_sessionId.c_str(),
                                          e.what());
                        cJSON_Delete(json);
                        return status;
                    }

                    // The audio payload was already decoded: strip the base64 from the copies used for
                    // events and logs (the README documents EVENT_PLAY as replacing it with the file path)
                    cJSON_DeleteItemFromObject(json, "delta");

                    bool notify_play = false;
                    if (!m_disable_audiofiles) {
                        std::string filePath = saveDebugAudioFile(rawAudio);
                        if (!filePath.empty()) {
                            cJSON *jsonFile = cJSON_CreateString(filePath.c_str());
                            if (jsonFile) {
                                cJSON_AddItemToObject(json, "file", jsonFile);
                                notify_play = true;
                            }
                        }
                    }

                    char *jsonString = cJSON_PrintUnformatted(json);
                    if (jsonString) {
                        if (notify_play) {
                            m_notify(session, EVENT_PLAY, jsonString);
                        }
                        message.assign(jsonString);
                        free(jsonString);
                    }

                    auto resampled = convertRawAudio(rawAudio);
                    if (!resampled.empty()) {
                        push_audio_queue(resampled);
                        status = SWITCH_TRUE;
                    }

                } else {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                      "(%s) processMessage - response.output_audio.delta no audio data\n",
                                      m_sessionId.c_str());
                }
                break;
            }

            case stream_protocol::JsonMessageType::AudioDone:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                                  "(%s) processMessage - audio done\n", m_sessionId.c_str());
                m_response_audio_done = true;
                break;

            case stream_protocol::JsonMessageType::Unhandled:
                break;
        }
        cJSON_Delete(json);
        return status;
    }

    // managing queue, check if empty before popping or peeking

    void push_audio_queue(const std::vector<int16_t>& audio_data) {
        std::lock_guard<std::mutex> lock(m_audio_queue_mutex);
        const size_t total = audio_data.size();
        if (m_audio_queue_samples + total > max_playback_queue_samples()) {
            if (!m_queue_overflow_logged) {
                m_queue_overflow_logged = true;
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                                  "(%s) push_audio_queue: playback backlog exceeds %d seconds, dropping incoming "
                                  "audio until it drains\n",
                                  m_sessionId.c_str(), MAX_PLAYBACK_QUEUE_SECONDS);
            }
            return;
        }
        if (total <= MAX_AUDIO_CHUNK_SAMPLES) {
            m_audio_queue.push(audio_data);
        } else {
            size_t num_chunks = (total + MAX_AUDIO_CHUNK_SAMPLES - 1) / MAX_AUDIO_CHUNK_SAMPLES;
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
                              "(%s) push_audio_queue: re-chunking %zu samples into %zu chunks (max %d samples each)\n",
                              m_sessionId.c_str(), total, num_chunks, MAX_AUDIO_CHUNK_SAMPLES);
            for (size_t offset = 0; offset < total; offset += MAX_AUDIO_CHUNK_SAMPLES) {
                size_t end = std::min(offset + MAX_AUDIO_CHUNK_SAMPLES, total);
                m_audio_queue.emplace(audio_data.begin() + offset, audio_data.begin() + end);
            }
        }
        m_audio_queue_samples += total;
    }

    bool pop_audio_queue(std::vector<int16_t>& out_audio) {
        std::lock_guard<std::mutex> lock(m_audio_queue_mutex);
        if (m_audio_queue.empty()) {
            return false;
        }
        out_audio = std::move(m_audio_queue.front());
        m_audio_queue.pop();
        m_audio_queue_samples -= out_audio.size();
        if (m_queue_overflow_logged && m_audio_queue_samples < max_playback_queue_samples() / 2) {
            m_queue_overflow_logged = false;
        }
        return true;
    }

    void clear_audio_queue() {
        std::lock_guard<std::mutex> lock(m_audio_queue_mutex);
        while (!m_audio_queue.empty()) {
            m_audio_queue.pop();
        }
        m_audio_queue_samples = 0;
        m_queue_overflow_logged = false;
    }

    ~AudioStreamer() {
        disconnect();
        if (m_resampler) {
            speex_resampler_destroy(m_resampler);
            m_resampler = nullptr;
        }
    }

    void disconnect() noexcept {
        if (!m_started.exchange(false)) {
            return;
        }
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "disconnecting...\n");
        try {
            webSocket.stop();
        } catch (const std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) failed to stop WebSocket thread: %s\n",
                              m_sessionId.c_str(), e.what());
        }
    }

    bool isConnected() {
        return (webSocket.getReadyState() == ix::ReadyState::Open);
    }

    // For all write methods, success means the payload was accepted by the WebSocket client while
    // connected; delivery to the server is not confirmed.
    bool writeAudioDelta(const uint8_t *buffer, size_t len) {
        if (!this->isConnected())
            return false;
        // Convert the buffer to PCM16 and then base64 encode it.

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
        if (!this->isConnected())
            return false;
        return webSocket.sendBinary(ix::IXWebSocketSendData(reinterpret_cast<const char *>(buffer), len)).success;
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

    bool writeText(const char *text) { // Openai only accepts json not utf8 plain text
        if (!this->isConnected())
            return false;
        return webSocket.sendUtf8Text(ix::IXWebSocketSendData(text, strlen(text))).success;
    }

    void deleteFiles() {
        if (m_playFile > 0) {
            for (const auto& fileName : m_Files) {
                remove(fileName.c_str());
            }
        }
    }

    void request_playback_clear() {
        m_playback_clear_gen.fetch_add(1, std::memory_order_release);
    }

    void request_capture_reset() {
        m_capture_reset_pending.store(true, std::memory_order_release);
    }

    // Returns true once per pending reset request; media (stream_frame) thread only
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

    bool is_openai_speaking() {
        return m_openai_speaking;
    }

    bool suppress_log() const {
        return m_suppress_log;
    }

    bool is_response_audio_done() {
        return m_response_audio_done;
    }

    bool is_terminally_closed() {
        return m_terminal_close;
    }

    void openai_speech_started() {
        m_openai_speaking = true;
        switch_core_session_t *psession = switch_core_session_locate(m_sessionId.c_str());

        if (psession) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Openai started speaking\n",
                              m_sessionId.c_str());
            const char *payload = "{\"status\":\"started\"}";
            m_notify(psession, EVENT_OPENAI_SPEECH_STARTED, payload);
            switch_core_session_rwunlock(psession);
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "(%s) Openai speech started - could not locate session\n", m_sessionId.c_str());
        }
    }

    void openai_speech_stopped() {
        m_openai_speaking = false;
        switch_core_session_t *psession = switch_core_session_locate(m_sessionId.c_str());

        if (psession) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Openai stopped speaking\n",
                              m_sessionId.c_str());
            const char *payload = "{\"status\":\"stopped\"}";
            m_notify(psession, EVENT_OPENAI_SPEECH_STOPPED, payload);
            switch_core_session_rwunlock(psession);
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "(%s) Openai speech stopped - could not locate session\n", m_sessionId.c_str());
        }
    }

  private:
    size_t max_playback_queue_samples() const {
        return static_cast<size_t>(out_sample_rate) * MAX_PLAYBACK_QUEUE_SECONDS;
    }

    std::string m_sessionId;
    responseHandler_t m_notify;
    ix::WebSocket webSocket;
    bool m_suppress_log;
    int m_playFile;
    std::unordered_set<std::string> m_Files;

    int in_sample_rate = 24000;  // playback sample rate (default: OpenAI 24kHz)
    int out_sample_rate = 16000; // output default sample rate
    SpeexResamplerState *m_resampler = nullptr;
    std::queue<std::vector<int16_t>> m_audio_queue;
    std::mutex m_audio_queue_mutex;
    size_t m_audio_queue_samples = 0;     // guarded by m_audio_queue_mutex
    bool m_queue_overflow_logged = false; // guarded by m_audio_queue_mutex
    std::atomic<uint64_t> m_playback_clear_gen{0};
    uint64_t m_playback_clear_gen_seen = 0; // media (write_frame) thread only
    bool m_disable_audiofiles = false;      // disable saving audio files if true
    bool m_openai_speaking = false;         // media (write_frame) thread only
    std::atomic<bool> m_response_audio_done{false};
    std::atomic<bool> m_terminal_close{false};        // connection closed and no reconnection will be attempted
    std::atomic<bool> m_send_failure_logged{false};   // rate-limits the dropped-audio warning to once per episode
    std::atomic<bool> m_capture_reset_pending{false}; // drop stale capture residue after a connection drop
    std::atomic<bool> m_started{false};
    bool m_raw_audio_mode = false;
    private_t *m_context = nullptr;      // owner context; valid until the WebSocket thread has been joined
    uint8_t m_pending_raw_byte = 0;      // raw mode: trailing odd byte carried to the next binary frame
    bool m_has_pending_raw_byte = false; // WebSocket thread only
};

namespace {

using LifecycleMutex = std::recursive_mutex;

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

class LifecycleLockScope {
  public:
    explicit LifecycleLockScope(switch_core_session_t *session) : m_handle(acquire_lifecycle_lock(session)) {}
    ~LifecycleLockScope() {
        release_lifecycle_lock(m_handle);
    }
    explicit operator bool() const {
        return m_handle != nullptr;
    }

  private:
    LifecycleLockHandle *m_handle;
};

switch_status_t stream_data_init(private_t *tech_pvt, switch_core_session_t *session, char *wsUri, uint32_t sampling,
                                 uint32_t playback_target_rate, int desiredSampling, int playback_sampling,
                                 int channels, responseHandler_t responseHandler, int deflate, int heart_beat,
                                 bool suppressLog, int rtp_packets, const char *extra_headers, bool no_reconnect,
                                 const char *tls_cafile, const char *tls_keyfile, const char *tls_certfile,
                                 bool tls_disable_hostname_validation, bool disable_audiofiles,
                                 switch_bool_t start_muted, bool raw_audio_mode) {
    int err; // speex

    switch_memory_pool_t *pool = switch_core_session_get_pool(session);

    memset(tech_pvt, 0, sizeof(private_t));

    strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID - 1);
    tech_pvt->sessionId[MAX_SESSION_ID - 1] = '\0';
    tech_pvt->sampling = desiredSampling;
    tech_pvt->rtp_packets = rtp_packets;
    tech_pvt->channels = channels;
    switch_atomic_set(&tech_pvt->audio_paused, 0);
    switch_atomic_set(&tech_pvt->user_audio_muted, start_muted ? 1 : 0);
    switch_atomic_set(&tech_pvt->openai_audio_muted, 0);
    switch_atomic_set(&tech_pvt->close_requested, 0);

    const size_t buflen = static_cast<size_t>(FRAME_SIZE_8000) * desiredSampling / 8000 * channels * rtp_packets;
    const size_t playback_buflen = 128000; // 128KB may need to be decreased

    if (switch_buffer_create(pool, &tech_pvt->playback_buffer, playback_buflen) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "%s: Error creating playback buffer.\n", tech_pvt->sessionId);
        return SWITCH_STATUS_FALSE;
    }

    // The allocations can throw: contain any exception here so it never crosses the extern "C"
    // boundary. tech_pvt was memset to zero, so the caller's destroy_tech_pvt() safely tears down
    // whatever was already built.
    try {
        tech_pvt->pAudioStreamer = static_cast<void *>(new AudioStreamer(
            tech_pvt->sessionId, wsUri, responseHandler, deflate, heart_beat, suppressLog, extra_headers, no_reconnect,
            tls_cafile, tls_keyfile, tls_certfile, tls_disable_hostname_validation, playback_target_rate,
            playback_sampling, disable_audiofiles, raw_audio_mode, tech_pvt));
        tech_pvt->stream_buffers = static_cast<void *>(new StreamBuffers());
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

    if (static_cast<uint32_t>(desiredSampling) != sampling) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) resampling from %u to %u\n",
                          tech_pvt->sessionId, sampling, desiredSampling);
        tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
        if (0 != err) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
            return SWITCH_STATUS_FALSE;
        }
    } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "(%s) no resampling needed for this call\n", tech_pvt->sessionId);
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_data_init\n",
                      tech_pvt->sessionId);

    return SWITCH_STATUS_SUCCESS;
}

void destroy_tech_pvt(private_t *tech_pvt) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s destroy_tech_pvt\n", tech_pvt->sessionId);
    if (tech_pvt->pAudioStreamer) {
        auto *as = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
        // Stop and join the WebSocket thread before destroying state its callbacks may use
        as->disconnect();
        delete as;
        tech_pvt->pAudioStreamer = nullptr;
    }
    if (tech_pvt->resampler) {
        speex_resampler_destroy(tech_pvt->resampler);
        tech_pvt->resampler = nullptr;
    }
    if (tech_pvt->mutex) {
        switch_mutex_destroy(tech_pvt->mutex);
        tech_pvt->mutex = nullptr;
    }
    if (tech_pvt->stream_buffers) {
        auto *sb = static_cast<StreamBuffers *>(tech_pvt->stream_buffers);
        delete sb;
        tech_pvt->stream_buffers = nullptr;
    }
}

void finish(private_t *tech_pvt) {
    auto *aStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
    tech_pvt->pAudioStreamer = nullptr;
    if (aStreamer) {
        // Stop and join before deleting callback-owned state or debug files.
        aStreamer->disconnect();
        aStreamer->deleteFiles();
        delete aStreamer;
    }
}

// Send any residual aggregated capture audio, then clear the aggregator.
// The residue is dropped if the WebSocket is not connected. Caller must hold tech_pvt->mutex.
void flush_capture_residue(private_t *tech_pvt) {
    if (!tech_pvt->sbuffer) {
        return;
    }
    switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
    if (inuse == 0) {
        return;
    }
    auto *streamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
    auto *bufs = static_cast<StreamBuffers *>(tech_pvt->stream_buffers);
    if (streamer && streamer->isConnected() && bufs) {
        bufs->flush_buffer.resize(inuse);
        switch_buffer_read(tech_pvt->sbuffer, bufs->flush_buffer.data(), inuse);
        streamer->sendAudio(bufs->flush_buffer.data(), inuse);
    }
    switch_buffer_zero(tech_pvt->sbuffer);
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

switch_status_t is_valid_utf8(const char *str) {
    return stream_protocol::is_valid_utf8(str) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

switch_status_t stream_session_send_json(switch_core_session_t *session, const char *base64_input) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    auto *bug = static_cast<switch_media_bug_t *>(switch_channel_get_private(channel, MY_BUG_NAME));
    cJSON *json_obj = nullptr;
    char *json_unformatted = nullptr;
    switch_status_t status = SWITCH_STATUS_FALSE;
    if (!bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: no media bug found.\n");
        return SWITCH_STATUS_FALSE;
    }

    auto *tech_pvt = static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed to retrieve session data.\n");
        return SWITCH_STATUS_FALSE;
    }
    AudioStreamer *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
    if (!pAudioStreamer) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: AudioStreamer websocket is null.\n");
        return SWITCH_STATUS_FALSE;
    }

    if (!base64_input || strlen(base64_input) == 0) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: input is empty.\n");
        return SWITCH_STATUS_FALSE;
    }
    std::string decoded_str;
    try {
        decoded_str = base64_decode(base64_input, false);
    } catch (const std::exception& e) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: base64 decode error: %s\n", e.what());
        return SWITCH_STATUS_FALSE;
    }
    if (decoded_str.empty()) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json base64 decode failed.\n");
        return SWITCH_STATUS_FALSE;
    }

    json_obj = cJSON_Parse(decoded_str.c_str());
    if (!json_obj) {
        const char *err = cJSON_GetErrorPtr();
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: invalid JSON. Error near: %s\n", err ? err : "unknown");
        return SWITCH_STATUS_FALSE;
    }

    json_unformatted = cJSON_PrintUnformatted(json_obj);
    if (!json_unformatted) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_send_json failed: cJSON_PrintUnformatted returned null\n");
        cJSON_Delete(json_obj);
        return SWITCH_STATUS_FALSE;
    }

    // The payload can carry sensitive data (instructions, base64 audio): honor STREAM_SUPPRESS_LOG
    if (!pAudioStreamer->suppress_log()) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "stream_session_send_json: sending JSON: %s\n", json_unformatted);
    }
    status = pAudioStreamer->writeText(json_unformatted) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;

    if (json_unformatted)
        free(json_unformatted);
    if (json_obj)
        cJSON_Delete(json_obj);
    return status;
}

switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    auto *bug = static_cast<switch_media_bug_t *>(switch_channel_get_private(channel, MY_BUG_NAME));
    if (!bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_pauseresume failed because no bug\n");
        return SWITCH_STATUS_FALSE;
    }
    auto *tech_pvt = static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));

    if (!tech_pvt)
        return SWITCH_STATUS_FALSE;

    switch_core_media_bug_flush(bug);
    switch_atomic_set(&tech_pvt->audio_paused, pause ? 1 : 0);
    return SWITCH_STATUS_SUCCESS;
}

switch_status_t stream_session_set_user_mute(switch_core_session_t *session, int mute) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    auto *bug = static_cast<switch_media_bug_t *>(switch_channel_get_private(channel, MY_BUG_NAME));
    switch_status_t status = SWITCH_STATUS_FALSE;
    if (!bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_set_user_mute failed because no bug\n");
        return status;
    }
    auto *tech_pvt = static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt) {
        return status;
    }

    status = SWITCH_STATUS_SUCCESS;
    switch_core_media_bug_flush(bug);
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
        if (tech_pvt->mutex) {
            switch_mutex_lock(tech_pvt->mutex);
        }

        // Deliver the residual pre-mute speech before injecting silence, instead of dropping it
        flush_capture_residue(tech_pvt);

        AudioStreamer *streamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
        if (streamer && streamer->isConnected()) {
            size_t channels = tech_pvt->channels > 0 ? static_cast<size_t>(tech_pvt->channels) : 1;
            size_t sample_rate = tech_pvt->sampling > 0
                                     ? static_cast<size_t>(tech_pvt->sampling)
                                     : 24000; // 24 KHz is currently the only supported rate by openai
            size_t bytes = channels * sample_rate * sizeof(int16_t);
            std::vector<uint8_t> silence(bytes, 0);
            if (streamer->sendAudio(silence.data(), silence.size())) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                                  "Sent %zu bytes of silence after muting user audio\n", silence.size());
            } else {
                status = SWITCH_STATUS_FALSE;
            }
        } else {
            status = SWITCH_STATUS_FALSE;
        }

        if (tech_pvt->mutex) {
            switch_mutex_unlock(tech_pvt->mutex);
        }
    }

    return status;
}

switch_status_t stream_session_set_openai_mute(switch_core_session_t *session, int mute) {
    switch_channel_t *channel = switch_core_session_get_channel(session);
    auto *bug = static_cast<switch_media_bug_t *>(switch_channel_get_private(channel, MY_BUG_NAME));
    if (!bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                          "stream_session_set_openai_mute failed because no bug\n");
        return SWITCH_STATUS_FALSE;
    }

    auto *tech_pvt = static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt) {
        return SWITCH_STATUS_FALSE;
    }

    switch_core_media_bug_flush(bug);
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
                                    uint32_t samples_per_second, char *wsUri, int sampling, int playback_sampling,
                                    int channels, switch_bool_t start_muted, switch_bool_t force_raw_audio_mode,
                                    void **ppUserData) {
    int deflate = 0, heart_beat = 0;
    bool suppressLog = false;
    const char *buffer_size;
    const char *extra_headers = NULL;
    int rtp_packets = 1;
    bool no_reconnect = false;
    const char *tls_cafile = NULL;
    const char *tls_keyfile = NULL;
    const char *tls_certfile = NULL;
    const char *openai_api_key = NULL;
    bool tls_disable_hostname_validation = false;
    bool disable_audiofiles = false;
    bool raw_audio_mode = force_raw_audio_mode ? true : false;
    std::string authorization_header_json;

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
        if (force_raw_audio_mode) {
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
        const char *raw_audio_source = force_raw_audio_mode ? "API" : "deprecated channel variable";
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

    if ((buffer_size = switch_channel_get_variable(channel, "STREAM_BUFFER_SIZE"))) {
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

    if (openai_api_key) {
        // Build the headers via cJSON so the key value gets JSON-escaped, and merge
        // STREAM_EXTRA_HEADERS instead of ignoring it; Authorization takes precedence.
        // The std::string work can throw: contain it so it never crosses the extern "C" boundary.
        try {
            // Built before any cJSON allocation so a throw here leaks nothing
            const std::string bearer = "Bearer " + std::string(openai_api_key);
            cJSON *headers_obj = nullptr;
            const char *configured_extra = switch_channel_get_variable(channel, "STREAM_EXTRA_HEADERS");
            if (configured_extra) {
                headers_obj = cJSON_Parse(configured_extra);
                if (!headers_obj || headers_obj->type != cJSON_Object) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                                      "STREAM_EXTRA_HEADERS is not a valid JSON object, using only Authorization.\n");
                    cJSON_Delete(headers_obj);
                    headers_obj = nullptr;
                }
            }
            if (!headers_obj) {
                headers_obj = cJSON_CreateObject();
            }
            if (headers_obj) {
                cJSON_DeleteItemFromObject(headers_obj, "Authorization");
                cJSON_AddStringToObject(headers_obj, "Authorization", bearer.c_str());
                char *printed = cJSON_PrintUnformatted(headers_obj);
                cJSON_Delete(headers_obj); // printed is an independent copy: free the tree now
                if (printed) {
                    authorization_header_json.assign(printed);
                    switch_safe_free(printed);
                    extra_headers = authorization_header_json.c_str();
                }
            }
        } catch (const std::exception& e) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "Failed to build the Authorization header: %s\n", e.what());
            return SWITCH_STATUS_FALSE;
        }
        if (!extra_headers) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "Failed to build the Authorization header.\n");
            return SWITCH_STATUS_FALSE;
        }
    } else {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
                          "OPENAI_API_KEY is not set. Assuming you set STREAM_EXTRA_HEADERS variable.\n");
        extra_headers = switch_channel_get_variable(channel, "STREAM_EXTRA_HEADERS");
    }

    // allocate per-session tech_pvt
    auto *tech_pvt = static_cast<private_t *>(switch_core_session_alloc(session, sizeof(private_t)));

    if (!tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
        return SWITCH_STATUS_FALSE;
    }
    // The playback path replaces frames on the write side: resample to the write codec rate, which
    // can differ from the read rate on asymmetric sessions
    uint32_t playback_target_rate = samples_per_second;
    switch_codec_t *write_codec = switch_core_session_get_write_codec(session);
    if (write_codec && write_codec->implementation) {
        playback_target_rate = write_codec->implementation->actual_samples_per_second;
    }
    if (playback_target_rate != samples_per_second) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "playback target rate %u differs from read rate %u\n", playback_target_rate,
                          samples_per_second);
    }

    if (SWITCH_STATUS_SUCCESS !=
        stream_data_init(tech_pvt, session, wsUri, samples_per_second, playback_target_rate, sampling,
                         playback_sampling, channels, responseHandler, deflate, heart_beat, suppressLog, rtp_packets,
                         extra_headers, no_reconnect, tls_cafile, tls_keyfile, tls_certfile,
                         tls_disable_hostname_validation, disable_audiofiles, start_muted, raw_audio_mode)) {
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
    auto *streamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
    return streamer && streamer->start() ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

switch_bool_t stream_frame(switch_media_bug_t *bug) {
    auto *tech_pvt = static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt || switch_atomic_read(&tech_pvt->audio_paused) || switch_atomic_read(&tech_pvt->user_audio_muted))
        return SWITCH_TRUE;

    if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
        return SWITCH_TRUE;
    }

    auto *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);

    if (!pAudioStreamer || !pAudioStreamer->isConnected()) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
    }

    // Discard any capture residue left over from a dropped connection
    if (pAudioStreamer->consume_capture_reset() && tech_pvt->sbuffer) {
        switch_buffer_zero(tech_pvt->sbuffer);
    }

    // Get persistent buffers (allocated once per session, reused across all frames)
    auto *bufs = static_cast<StreamBuffers *>(tech_pvt->stream_buffers);

    auto flush_sbuffer = [tech_pvt]() { flush_capture_residue(tech_pvt); };

    auto send_or_buffer_audio = [tech_pvt, pAudioStreamer, &flush_sbuffer](const uint8_t *data, size_t length) {
        if (tech_pvt->rtp_packets == 1) {
            pAudioStreamer->sendAudio(data, length);
            return true;
        }

        while (length > 0) {
            switch_size_t free_space = switch_buffer_freespace(tech_pvt->sbuffer);
            if (free_space == 0) {
                flush_sbuffer();
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
                flush_sbuffer();
            }
        }

        return true;
    };

    switch_frame_t frame{};
    frame.data = bufs->data_buf.data();
    frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

    while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) == SWITCH_STATUS_SUCCESS) {
        // Validate frame data before processing
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
        bufs->resample_buffer.resize(static_cast<size_t>(output_capacity) * tech_pvt->channels);

        const auto *input = static_cast<const spx_int16_t *>(frame.data);
        spx_uint32_t remaining_samples = frame.samples;
        while (remaining_samples > 0) {
            spx_uint32_t in_len = remaining_samples;
            spx_uint32_t out_len = output_capacity;
            int result;

            if (tech_pvt->channels == 1) {
                result = speex_resampler_process_int(tech_pvt->resampler, 0, input, &in_len,
                                                     bufs->resample_buffer.data(), &out_len);
            } else {
                result = speex_resampler_process_interleaved_int(tech_pvt->resampler, input, &in_len,
                                                                 bufs->resample_buffer.data(), &out_len);
            }

            if (result != RESAMPLER_ERR_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s: Resampling failed: %s\n",
                                  tech_pvt->sessionId, speex_resampler_strerror(result));
                break;
            }

            size_t bytes_written = static_cast<size_t>(out_len) * tech_pvt->channels * sizeof(spx_int16_t);
            if (bytes_written > 0 &&
                !send_or_buffer_audio(reinterpret_cast<const uint8_t *>(bufs->resample_buffer.data()), bytes_written)) {
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
    }

    switch_mutex_unlock(tech_pvt->mutex);
    return SWITCH_TRUE;
}

switch_bool_t write_frame(switch_core_session_t *session, switch_media_bug_t *bug) {
    private_t *tech_pvt = static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt || switch_atomic_read(&tech_pvt->audio_paused)) {
        return SWITCH_TRUE;
    }

    switch_frame_t *frame = switch_core_media_bug_get_write_replace_frame(bug);
    auto codec = switch_core_session_get_write_codec(session);
    if (!frame || !codec || !codec->implementation) {
        return SWITCH_TRUE;
    }

    AudioStreamer *as = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);

    // No isConnected() check: queued audio must keep draining after the connection drops
    if (!as) {
        return SWITCH_TRUE;
    }

    if (frame->samples == 0 || frame->datalen == 0) {
        return SWITCH_TRUE;
    }

    uint32_t bytes_needed = frame->datalen;
    uint32_t bytes_per_sample = frame->datalen / frame->samples;

    if (bytes_needed > frame->buflen) { // may be useless
        bytes_needed = frame->buflen;
    }

    uint32_t inuse = switch_buffer_inuse(tech_pvt->playback_buffer);

    // push a chunk in the audio buffer used treated as cache
    if (as->consume_playback_clear()) {
        switch_buffer_zero(tech_pvt->playback_buffer);
        inuse = 0;
        // barge-in interrupts the response: close the speaking state so the next one emits a new start
        if (as->is_openai_speaking()) {
            as->openai_speech_stopped();
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
        // Openai just finished speaking for interruption or end of response
        if (as->is_openai_speaking() && as->is_response_audio_done()) {
            as->openai_speech_stopped();
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
            as->openai_speech_started();
        }

        frame->datalen = bytes_needed;
        frame->samples = bytes_needed / bytes_per_sample;

        switch_core_media_bug_set_write_replace_frame(bug, frame);
    }

    return SWITCH_TRUE;
}

switch_status_t stream_session_cleanup(switch_core_session_t *session, char *text, int channelIsClosing) {
    LifecycleLockScope lifecycle_lock(session);
    if (!lifecycle_lock) {
        return SWITCH_STATUS_FALSE;
    }

    switch_channel_t *channel = switch_core_session_get_channel(session);
    auto *bug = static_cast<switch_media_bug_t *>(switch_channel_get_private(channel, MY_BUG_NAME));
    if (bug) {
        auto *tech_pvt = static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
        switch_status_t status = SWITCH_STATUS_SUCCESS;
        char sessionId[MAX_SESSION_ID];

        if (!tech_pvt) {
            // should not happen: the bug is always created with user data
            switch_channel_set_private(channel, MY_BUG_NAME, nullptr);
            if (!channelIsClosing && switch_core_media_bug_remove(session, &bug) != SWITCH_STATUS_SUCCESS) {
                switch_channel_set_private(channel, MY_BUG_NAME, bug);
            }
            return SWITCH_STATUS_FALSE;
        }

        strncpy(sessionId, tech_pvt->sessionId, MAX_SESSION_ID - 1);
        sessionId[MAX_SESSION_ID - 1] = '\0';

        switch_mutex_lock(tech_pvt->mutex);
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_session_cleanup\n",
                          sessionId);

        // Deliver the residual aggregated capture audio before the final JSON and the teardown,
        // so a final commit/response request sees all the audio captured so far
        flush_capture_residue(tech_pvt);

        if (text && *text) {
            status = stream_session_send_json(session, text);
        }

        // Detach and remove the media bug while its user data is still alive. Once remove
        // returns, no media callback can race the synchronous WebSocket teardown below.
        switch_channel_set_private(channel, MY_BUG_NAME, nullptr);
        if (!channelIsClosing && switch_core_media_bug_remove(session, &bug) != SWITCH_STATUS_SUCCESS) {
            // FreeSWITCH may refuse removal while a bug is thread-locked. Keep the context alive
            // and reachable so media callbacks cannot observe freed user data.
            switch_channel_set_private(channel, MY_BUG_NAME, bug);
            switch_mutex_unlock(tech_pvt->mutex);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "(%s) stream_session_cleanup: failed to remove media bug\n", sessionId);
            return SWITCH_STATUS_FALSE;
        }

        finish(tech_pvt);

        switch_mutex_unlock(tech_pvt->mutex);
        destroy_tech_pvt(tech_pvt);

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                          "(%s) stream_session_cleanup: connection closed\n", sessionId);
        return status;
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                      "stream_session_cleanup: no bug - websocket connection already closed\n");
    return SWITCH_STATUS_FALSE;
}
}
