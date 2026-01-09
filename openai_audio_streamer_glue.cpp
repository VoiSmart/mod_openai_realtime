#include <string>
#include <cstring>
#include "mod_openai_audio_stream.h"
#include <ixwebsocket/IXWebSocket.h>
#include <sstream>
#include <queue>
#include <vector>

#include <switch_json.h>
#include <fstream>
#include <switch_buffer.h>
#include <unordered_map>
#include <unordered_set>
#include "base64.h"

#define FRAME_SIZE_8000  320 /* 1000x0.02 (20ms)= 160 x(16bit= 2 bytes) 320 frame size*/

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

    AudioStreamer(const char* uuid, const char* wsUri, responseHandler_t callback, int deflate, int heart_beat,
                    bool suppressLog, const char* extra_headers, bool no_reconnect,
                    const char* tls_cafile, const char* tls_keyfile, const char* tls_certfile,
                    bool tls_disable_hostname_validation, uint32_t session_sampling, bool disable_audiofiles): m_sessionId(uuid), m_notify(callback), 
        m_suppress_log(suppressLog), m_extra_headers(extra_headers), m_playFile(0), m_disable_audiofiles(disable_audiofiles) {

        ix::WebSocketHttpHeaders headers;
        ix::SocketTLSOptions tlsOptions;
        if (m_extra_headers) {
            cJSON *headers_json = cJSON_Parse(m_extra_headers);
            if (headers_json) {
                cJSON *iterator = headers_json->child;
                while (iterator) {
                    if (iterator->type == cJSON_String && iterator->valuestring != nullptr) {
                        headers[iterator->string] = iterator->valuestring;
                    }
                    iterator = iterator->next;
                }
                cJSON_Delete(headers_json);
            }
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
        if(heart_beat)
            webSocket.setPingInterval(heart_beat);

        // Per message deflate connection is enabled by default. You can tweak its parameters or disable it
        if(deflate)
            webSocket.disablePerMessageDeflate();

        // Set extra headers if any
        if(!headers.empty())
            webSocket.setExtraHeaders(headers);

        if(no_reconnect)
            webSocket.disableAutomaticReconnection();

        // Setup a callback to be fired when a message or an event (open, close, error) is received
        webSocket.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg){
            if (msg->type == ix::WebSocketMessageType::Message)
            {
                eventCallback(MESSAGE, msg->str.c_str());

            } else if (msg->type == ix::WebSocketMessageType::Open)
            {
                cJSON *root;
                root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "status", "connected");
                char *json_str = cJSON_PrintUnformatted(root);

                eventCallback(CONNECT_SUCCESS, json_str);

                cJSON_Delete(root);
                switch_safe_free(json_str);

            } else if (msg->type == ix::WebSocketMessageType::Error)
            {
                //A message will be fired when there is an error with the connection. The message type will be ix::WebSocketMessageType::Error.
                // Multiple fields will be inuse on the event to describe the error.
                cJSON *root, *message;
                root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "status", "error");
                message = cJSON_CreateObject();
                cJSON_AddNumberToObject(message, "retries", msg->errorInfo.retries);
                cJSON_AddStringToObject(message, "error", msg->errorInfo.reason.c_str());
                cJSON_AddNumberToObject(message, "wait_time", msg->errorInfo.wait_time);
                cJSON_AddNumberToObject(message, "http_status", msg->errorInfo.http_status);
                cJSON_AddItemToObject(root, "message", message);

                char *json_str = cJSON_PrintUnformatted(root);

                eventCallback(CONNECT_ERROR, json_str);

                cJSON_Delete(root);
                switch_safe_free(json_str);
            }
            else if (msg->type == ix::WebSocketMessageType::Close)
                {
                // The server can send an explicit code and reason for closing.
                // This data can be accessed through the closeInfo object.
                cJSON *root, *message;
                root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "status", "disconnected");
                message = cJSON_CreateObject();
                cJSON_AddNumberToObject(message, "code", msg->closeInfo.code);
                cJSON_AddStringToObject(message, "reason", msg->closeInfo.reason.c_str());
                cJSON_AddItemToObject(root, "message", message);
                char *json_str = cJSON_PrintUnformatted(root);

                eventCallback(CONNECTION_DROPPED, json_str);

                cJSON_Delete(root);
                switch_safe_free(json_str);
            }
        });

        int err = 0;
        out_sample_rate = session_sampling; 
        m_resampler = speex_resampler_init(1, in_sample_rate, out_sample_rate, 5, &err);

        // Now that our callback is setup, we can start our background thread and receive messages
        webSocket.start();
    }

    switch_media_bug_t *get_media_bug(switch_core_session_t *session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if(!channel) {
            return nullptr;
        }
        auto *bug = static_cast<switch_media_bug_t *>(switch_channel_get_private(channel, MY_BUG_NAME));
        return bug;
    }

    inline void media_bug_close(switch_core_session_t *session) {
        auto *bug = get_media_bug(session);
        if(bug) {
            auto* tech_pvt = static_cast<private_t*>(switch_core_media_bug_get_user_data(bug));
            tech_pvt->close_requested = 1;
            switch_core_media_bug_close(&bug, SWITCH_FALSE);
        }
    }

    void eventCallback(notifyEvent_t event, const char* message) {
        switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());
        if(psession) {
            switch (event) {
                case CONNECT_SUCCESS:
                    m_notify(psession, EVENT_CONNECT, message);
                    break;
                case CONNECTION_DROPPED:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection closed\n");
                    m_notify(psession, EVENT_DISCONNECT, message);
                    break;
                case CONNECT_ERROR:
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO, "connection error\n");
                    m_notify(psession, EVENT_ERROR, message);

                    media_bug_close(psession);

                    break;
                case MESSAGE:
                    std::string msg(message);
                    if(processMessage(psession, msg) != SWITCH_TRUE) {
                        m_notify(psession, EVENT_JSON, msg.c_str());
                    }

                    if (!m_suppress_log) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_DEBUG, "Received message: %s\n", msg.c_str());
                    }
                    break;
            }
            switch_core_session_rwunlock(psession);
        }
    }

    
    std::vector<int16_t> resampleRawAudio(const std::string& input_raw) {
        size_t in_samples = input_raw.size() / 2;

        double scaled = static_cast<double>(in_samples) * out_sample_rate / in_sample_rate;
        size_t out_samples = static_cast<size_t>(scaled) + 1;

        std::vector<int16_t> in_buffer(in_samples);
        std::vector<int16_t> out_buffer(out_samples);

        std::memcpy(in_buffer.data(), input_raw.data(), input_raw.size());

        if (in_samples > UINT32_MAX || out_samples > UINT32_MAX) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "Too many samples to resample: in=%zu, out=%zu\n",
                              in_samples, out_samples);
            return {};
        }

        spx_uint32_t in_len = static_cast<spx_uint32_t>(in_samples);
        spx_uint32_t out_len = static_cast<spx_uint32_t>(out_samples);

        int err = speex_resampler_process_int(m_resampler, 0,
                                              in_buffer.data(), &in_len,
                                              out_buffer.data(), &out_len);

        if (err != RESAMPLER_ERR_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                              "Resampling failed with error code: %d\n", err);
            return {}; // empty on error
        }

        out_buffer.resize(out_len);  // resize to actual resampled size
        return out_buffer;
    }

    // create wav file from raw audio 
    // rawAudio passed as constant reference because it is never edited
    std::string createWavFromRaw(const std::string& rawAudio) {

        const int numChannels = 1; // mono
        const int bitsPerSample = 16; // pcm16
        int byteRate = in_sample_rate * numChannels * bitsPerSample / 8;
        int blockAlign = numChannels * bitsPerSample / 8;
        uint32_t dataSize = static_cast<uint32_t>(rawAudio.size());
        uint32_t chunkSize = 36 + dataSize;

        std::ostringstream wavStream; // write in string like stream

        // creating wav header
        // riff header
        wavStream.write("RIFF", 4); // chunk id
        wavStream.write(reinterpret_cast<const char*>(&chunkSize), 4); // chunk Size
        wavStream.write("WAVE", 4); 

        // this subchunk contains format infos
        wavStream.write("fmt ", 4); 
        uint32_t subchunk1Size = 16; 
        wavStream.write(reinterpret_cast<const char*>(&subchunk1Size), 4); 
        uint16_t audioFormat = 1; // 1 is pcm
        wavStream.write(reinterpret_cast<const char*>(&audioFormat), 2);
        wavStream.write(reinterpret_cast<const char*>(&numChannels), 2);
        wavStream.write(reinterpret_cast<const char*>(&in_sample_rate), 4);
        wavStream.write(reinterpret_cast<const char*>(&byteRate), 4);
        wavStream.write(reinterpret_cast<const char*>(&blockAlign), 2);
        wavStream.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

        // data subchunk contains audio data
        wavStream.write("data", 4);
        wavStream.write(reinterpret_cast<const char*>(&dataSize), 4);
        wavStream.write(rawAudio.data(), dataSize);

        return wavStream.str();

    }

    switch_bool_t processMessage(switch_core_session_t* session, std::string& message) {
        cJSON* json = cJSON_Parse(message.c_str());
        switch_bool_t status = SWITCH_FALSE;
        if (!json) {
            return status;
        }

        const char* jsType = cJSON_GetObjectCstr(json, "type");
        if (!m_suppress_log) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "processMessage type: %s\n", jsType ? jsType : "null");
        }

        if(jsType && strstr(jsType, "error")) { 
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) processMessage - error: %s\n", m_sessionId.c_str(), message.c_str());

        } else if(jsType && strcmp(jsType, "input_audio_buffer.speech_started") == 0) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%s) processMessage - user speech started, stopping openai audio playback\n", m_sessionId.c_str());
            clear_audio_queue();
            // also clear the private_t playback buffer used in write frame
            playback_clear_requested = true;

        } else if (jsType && strcmp(jsType, "input_audio_buffer.speech_stopped") == 0) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%s) processMessage - user speech stopped\n", m_sessionId.c_str());
            // Do not clear playback_clear_requested here; it should remain true until new audio is received.

        } else if(jsType && strcmp(jsType, "response.output_audio.delta") == 0) {
            const char* jsonAudio = cJSON_GetObjectCstr(json, "delta");
            playback_clear_requested = false;
            m_response_audio_done = false;
            
            if(jsonAudio && strlen(jsonAudio) > 0) {
                std::string rawAudio;
                try {
                    rawAudio = base64_decode(jsonAudio);
                } catch (const std::exception& e) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) processMessage - base64 decode error: %s\n", m_sessionId.c_str(), e.what());
                    cJSON_Delete(json);
                    return status;
                }

                if (!m_disable_audiofiles) {
                    char filePath[256];
                    std::string fileType = ".wav";
                    switch_snprintf(filePath, 256, "%s%s%s_%d.tmp%s", SWITCH_GLOBAL_dirs.temp_dir,
                                    SWITCH_PATH_SEPARATOR, m_sessionId.c_str(), m_playFile++, fileType.c_str());
                    std::ofstream fstream(filePath, std::ofstream::binary); // For debugging purposes; can be disabled via the disable_audiofiles flag
                    std::string wavData = createWavFromRaw(rawAudio);
                    fstream.write(wavData.data(), wavData.size());
                    fstream.flush(); // flush buffer to os level write
                    fstream.close();
                    m_Files.insert(filePath);
                    cJSON* jsonFile = cJSON_CreateString(filePath);
                    cJSON_AddItemToObject(json, "file", jsonFile); 

                    char *jsonString = cJSON_PrintUnformatted(json);
                    m_notify(session, EVENT_PLAY, jsonString);
                    message.assign(jsonString);
                    free(jsonString);
                }

                auto resampled = resampleRawAudio(rawAudio); 
                push_audio_queue(resampled);
                status = SWITCH_TRUE;

            } else {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "(%s) processMessage - response.output_audio.delta no audio data\n", m_sessionId.c_str());
            }
        } else if(jsType && strcmp(jsType, "response.output_audio.done") == 0) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) processMessage - audio done\n", m_sessionId.c_str());
            m_response_audio_done = true;
        } 
        cJSON_Delete(json);
        return status;
    }

    // managing queue, check if empty before popping or peeking
    
    void push_audio_queue(const std::vector<int16_t>& audio_data) {
        std::lock_guard<std::mutex> lock(m_audio_queue_mutex);
        m_audio_queue.push(audio_data);
    }

    bool pop_audio_queue(std::vector<int16_t> &out_audio) {
        std::lock_guard<std::mutex> lock(m_audio_queue_mutex);
        if (m_audio_queue.empty()) {
            return false;
        }
        out_audio = m_audio_queue.front();
        m_audio_queue.pop();
        return true;
    }

    void clear_audio_queue() {
        std::lock_guard<std::mutex> lock(m_audio_queue_mutex);
        while (!m_audio_queue.empty()) {
            m_audio_queue.pop();
        }
    }

    ~AudioStreamer() {
        if (m_resampler) {
            speex_resampler_destroy(m_resampler);
            m_resampler = nullptr;
        }
    }


    void disconnect() {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "disconnecting...\n");
        webSocket.stop();
    }

    bool isConnected() {
        return (webSocket.getReadyState() == ix::ReadyState::Open);
    }

    void writeAudioDelta(uint8_t* buffer, size_t len) {
        if(!this->isConnected()) return;
        // Convert the buffer to PCM16 and then base64 encode it.

        std::string base64Audio = base64_encode(buffer, len, false);
        if (base64Audio.empty()) return;

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "input_audio_buffer.append");
        cJSON_AddStringToObject(root, "audio", base64Audio.c_str());

        char *jsonStr = cJSON_PrintUnformatted(root);
        webSocket.sendUtf8Text(ix::IXWebSocketSendData(jsonStr, strlen(jsonStr)));

        cJSON_Delete(root);
        switch_safe_free(jsonStr);
    }

    void writeBinary(uint8_t* buffer, size_t len) {
        if(!this->isConnected()) return;
        webSocket.sendBinary(ix::IXWebSocketSendData(reinterpret_cast<const char *>(buffer), len));
    }

    void writeText(const char* text) {  // Openai only accepts json not utf8 plain text
        if(!this->isConnected()) return;
        webSocket.sendUtf8Text(ix::IXWebSocketSendData(text, strlen(text)));
    }

    void deleteFiles() {
        if(m_playFile >0) {
            for (const auto &fileName: m_Files) {
                remove(fileName.c_str());
            }
        }
    }

    bool clear_requested() {
        return playback_clear_requested;
    }

    bool is_openai_speaking() {
        return m_openai_speaking;
    }

    bool is_response_audio_done() {
        return m_response_audio_done;
    }

    void openai_speech_started() {
        m_openai_speaking = true;
        switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());

        if (psession) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Openai started speaking\n", m_sessionId.c_str());
            const char *payload = "{\"status\":\"started\"}";
            m_notify(psession, EVENT_OPENAI_SPEECH_STARTED, payload);
            switch_core_session_rwunlock(psession);
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Openai speech started - could not locate session\n", m_sessionId.c_str());
        }
     }

    void openai_speech_stopped() {
        m_openai_speaking = false;
        switch_core_session_t* psession = switch_core_session_locate(m_sessionId.c_str());

        if (psession) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "(%s) Openai stopped speaking\n", m_sessionId.c_str());
            const char *payload = "{\"status\":\"stopped\"}";
            m_notify(psession, EVENT_OPENAI_SPEECH_STOPPED, payload);
            switch_core_session_rwunlock(psession);
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "(%s) Openai speech stopped - could not locate session\n", m_sessionId.c_str());
        }
     }


private:
    std::string m_sessionId;
    responseHandler_t m_notify;
    ix::WebSocket webSocket;
    bool m_suppress_log;
    const char* m_extra_headers;
    int m_playFile;
    std::unordered_set<std::string> m_Files;

    int in_sample_rate = 24000; //OpenAI sample rate
    int out_sample_rate = 16000; // output default sample rate
    SpeexResamplerState *m_resampler = nullptr; 
    std::queue<std::vector<int16_t>> m_audio_queue;
    std::mutex m_audio_queue_mutex;
    bool playback_clear_requested = false; 
    bool m_disable_audiofiles = false; // disable saving audio files if true
    bool m_openai_speaking = false;
    bool m_response_audio_done = false;
};


namespace {

        switch_status_t stream_data_init(private_t *tech_pvt, switch_core_session_t *session, char *wsUri,
                                         uint32_t sampling, int desiredSampling, int channels, responseHandler_t responseHandler,
                                         int deflate, int heart_beat, bool suppressLog, int rtp_packets, const char* extra_headers,
                                         bool no_reconnect, const char *tls_cafile, const char *tls_keyfile,
                                         const char *tls_certfile, bool tls_disable_hostname_validation, bool disable_audiofiles,
                                         switch_bool_t start_muted)
        {
            int err; //speex

            switch_memory_pool_t *pool = switch_core_session_get_pool(session);

            memset(tech_pvt, 0, sizeof(private_t));

            strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session), MAX_SESSION_ID - 1);
            tech_pvt->sessionId[MAX_SESSION_ID - 1] = '\0';
            strncpy(tech_pvt->ws_uri, wsUri, MAX_WS_URI - 1);
            tech_pvt->ws_uri[MAX_WS_URI - 1] = '\0';
            tech_pvt->sampling = desiredSampling;
            tech_pvt->responseHandler = responseHandler;
            tech_pvt->rtp_packets = rtp_packets;
            tech_pvt->channels = channels;
            tech_pvt->audio_paused = 0;
            tech_pvt->user_audio_muted = start_muted ? 1 : 0;
            tech_pvt->openai_audio_muted = 0;

            const size_t buflen = (FRAME_SIZE_8000 * desiredSampling / 8000 * channels * rtp_packets);
            const size_t playback_buflen = 128000; // 128KB may need to be decreased 

            if (switch_buffer_create(pool, &tech_pvt->playback_buffer, playback_buflen) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                                  "%s: Error creating playback buffer.\n", tech_pvt->sessionId);
                return SWITCH_STATUS_FALSE;
            }

            auto* as = new AudioStreamer(tech_pvt->sessionId, wsUri, responseHandler, deflate, heart_beat,
                                            suppressLog, extra_headers, no_reconnect,
                                            tls_cafile, tls_keyfile, tls_certfile, tls_disable_hostname_validation, sampling, disable_audiofiles); 

            tech_pvt->pAudioStreamer = static_cast<void *>(as);
            tech_pvt->stream_buffers = static_cast<void *>(new StreamBuffers());

            switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, pool);

            if (switch_buffer_create(pool, &tech_pvt->sbuffer, buflen) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                    "%s: Error creating switch buffer.\n", tech_pvt->sessionId);
                return SWITCH_STATUS_FALSE;
            }

            if (static_cast<uint32_t>(desiredSampling) != sampling) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) resampling from %u to %u\n", tech_pvt->sessionId, sampling, desiredSampling);
                tech_pvt->resampler = speex_resampler_init(channels, sampling, desiredSampling, SWITCH_RESAMPLE_QUALITY, &err);
                if (0 != err) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Error initializing resampler: %s.\n", speex_resampler_strerror(err));
                    return SWITCH_STATUS_FALSE;
                }
            }
            else {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) no resampling needed for this call\n", tech_pvt->sessionId);
            }

            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_data_init\n", tech_pvt->sessionId);

        return SWITCH_STATUS_SUCCESS;
    }

    void destroy_tech_pvt(private_t* tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "%s destroy_tech_pvt\n", tech_pvt->sessionId);
        if (tech_pvt->resampler) {
            speex_resampler_destroy(tech_pvt->resampler);
            tech_pvt->resampler = nullptr;
        }
        if (tech_pvt->mutex) {
            switch_mutex_destroy(tech_pvt->mutex);
            tech_pvt->mutex = nullptr;
        }
        if (tech_pvt->pAudioStreamer) {
            auto* as = static_cast<AudioStreamer*>(tech_pvt->pAudioStreamer);
            delete as;
            tech_pvt->pAudioStreamer = nullptr;
        }
        if (tech_pvt->stream_buffers) {
            auto* sb = static_cast<StreamBuffers*>(tech_pvt->stream_buffers);
            delete sb;
            tech_pvt->stream_buffers = nullptr;
        }
    }

    void finish(private_t* tech_pvt) {
        std::shared_ptr<AudioStreamer> aStreamer;
        aStreamer.reset(static_cast<AudioStreamer*>(tech_pvt->pAudioStreamer));
        tech_pvt->pAudioStreamer = nullptr;

        std::thread t([aStreamer]{
            aStreamer->disconnect();
        });
        t.detach();
    }

}

extern "C" {
    int validate_ws_uri(const char* url, char* wsUri) {
        const char* hostStart = nullptr;
        const char* hostEnd = nullptr;
        const char* portStart = nullptr;

        // Check scheme
        if (strncmp(url, "ws://", 5) == 0) {
            hostStart = url + 5;
        } else if (strncmp(url, "wss://", 6) == 0) {
            hostStart = url + 6;
        } else {
            return 0;
        }

        // Find host end or port start
        hostEnd = hostStart;
        while (*hostEnd && *hostEnd != ':' && *hostEnd != '/') {
            if (!std::isalnum(*hostEnd) && *hostEnd != '-' && *hostEnd != '.') {
                return 0;
            }
            ++hostEnd;
        }

        // Check if host is empty
        if (hostStart == hostEnd) {
            return 0;
        }

        // Check for port
        if (*hostEnd == ':') {
            portStart = hostEnd + 1;
            while (*portStart && *portStart != '/') {
                if (!std::isdigit(*portStart)) {
                    return 0;
                }
                ++portStart;
            }
        }

        // Copy valid URI to wsUri
        std::strncpy(wsUri, url, MAX_WS_URI);
        return 1;
    }

    switch_status_t is_valid_utf8(const char *str) {
        switch_status_t status = SWITCH_STATUS_FALSE;
        while (*str) {
            if ((*str & 0x80) == 0x00) {
                // 1-byte character
                str++;
            } else if ((*str & 0xE0) == 0xC0) {
                // 2-byte character
                if ((str[1] & 0xC0) != 0x80) {
                    return status;
                }
                str += 2;
            } else if ((*str & 0xF0) == 0xE0) {
                // 3-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80) {
                    return status;
                }
                str += 3;
            } else if ((*str & 0xF8) == 0xF0) {
                // 4-byte character
                if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 || (str[3] & 0xC0) != 0x80) {
                    return status;
                }
                str += 4;
            } else {
                // invalid character
                return status;
            }
        }
        return SWITCH_STATUS_SUCCESS;
    }

    
    switch_status_t stream_session_send_json(switch_core_session_t *session, const char* base64_input) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = static_cast<switch_media_bug_t *>(switch_channel_get_private(channel, MY_BUG_NAME));
        cJSON *json_obj = nullptr;
        char *json_unformatted = nullptr;
        switch_status_t status = SWITCH_STATUS_FALSE;
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_send_json failed: no media bug found.\n");
            return SWITCH_STATUS_FALSE;
        }

        auto *tech_pvt = static_cast<private_t*>(switch_core_media_bug_get_user_data(bug));
        if (!tech_pvt) { 
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_send_json failed to retrieve session data.\n");
            return SWITCH_STATUS_FALSE;
        }
        AudioStreamer *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
        if (!pAudioStreamer) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_send_json failed: AudioStreamer websocket is null.\n");
            return SWITCH_STATUS_FALSE;
        }

        if (!base64_input || strlen(base64_input) == 0) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_send_json failed: input is empty.\n");
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
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_send_json base64 decode failed.\n");
            return SWITCH_STATUS_FALSE;
        }

        json_obj = cJSON_Parse(decoded_str.c_str());
        if (!json_obj) {
            const char* err = cJSON_GetErrorPtr();
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "stream_session_send_json failed: invalid JSON. Error near: %s\n",
                              err ? err : "unknown");
            return SWITCH_STATUS_FALSE;
        }

        json_unformatted = cJSON_PrintUnformatted(json_obj);
        if (!json_unformatted) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
                              "stream_session_send_json failed: cJSON_PrintUnformatted returned null\n");
            return SWITCH_STATUS_FALSE;
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                          "stream_session_send_json: sending JSON: %s\n", json_unformatted); 
        pAudioStreamer->writeText(json_unformatted);
        status = SWITCH_STATUS_SUCCESS;

        if (json_unformatted) free(json_unformatted);
        if (json_obj) cJSON_Delete(json_obj);
        return status;
    }


    switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = static_cast<switch_media_bug_t *>(switch_channel_get_private(channel, MY_BUG_NAME));
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_pauseresume failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }
        auto *tech_pvt = static_cast<private_t*>(switch_core_media_bug_get_user_data(bug));

        if (!tech_pvt) return SWITCH_STATUS_FALSE;

        switch_core_media_bug_flush(bug);
        tech_pvt->audio_paused = pause;
        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_set_user_mute(switch_core_session_t *session, int mute) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = static_cast<switch_media_bug_t *>(switch_channel_get_private(channel, MY_BUG_NAME));
        switch_status_t status = SWITCH_STATUS_FALSE;
        if (!bug) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_set_user_mute failed because no bug\n");
            return status;
        }
        auto *tech_pvt = static_cast<private_t*>(switch_core_media_bug_get_user_data(bug));
        if (!tech_pvt) {
            return status;
        }

        status = SWITCH_STATUS_SUCCESS;
        switch_core_media_bug_flush(bug);
        const int last_state = tech_pvt->user_audio_muted;
        tech_pvt->user_audio_muted = mute ? 1 : 0;
        if (last_state == tech_pvt->user_audio_muted) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                              "User audio is already %s\n", tech_pvt->user_audio_muted ? "muted" : "unmuted");
            return status;
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                          "User audio %s\n", tech_pvt->user_audio_muted ? "muted" : "unmuted");

        if (tech_pvt->user_audio_muted) {
            if (tech_pvt->mutex) {
                switch_mutex_lock(tech_pvt->mutex);
            }

            if (tech_pvt->sbuffer) {
                switch_buffer_zero(tech_pvt->sbuffer);
            }

            AudioStreamer *streamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
            if (streamer && streamer->isConnected()) {
                size_t channels = tech_pvt->channels > 0 ? static_cast<size_t>(tech_pvt->channels) : 1;
                size_t sample_rate = tech_pvt->sampling > 0 ? static_cast<size_t>(tech_pvt->sampling) : 24000; // 24 KHz is currently the only supported rate by openai
                size_t bytes = channels * sample_rate * sizeof(int16_t);
                std::vector<uint8_t> silence(bytes, 0);
                streamer->writeAudioDelta(silence.data(), silence.size());
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
                                  "Sent %zu bytes of silence after muting user audio\n", silence.size());
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
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "stream_session_set_openai_mute failed because no bug\n");
            return SWITCH_STATUS_FALSE;
        }

        auto *tech_pvt = static_cast<private_t*>(switch_core_media_bug_get_user_data(bug));
        if (!tech_pvt) {
            return SWITCH_STATUS_FALSE;
        }

        switch_core_media_bug_flush(bug);
        auto last_state = tech_pvt->openai_audio_muted;
        tech_pvt->openai_audio_muted = mute ? 1 : 0;
        if (last_state == tech_pvt->openai_audio_muted) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                              "OpenAI audio is already %s\n", tech_pvt->openai_audio_muted ? "muted" : "unmuted");
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
                              "OpenAI audio %s\n", tech_pvt->openai_audio_muted ? "muted" : "unmuted");
        }

        return SWITCH_STATUS_SUCCESS;
    }

    switch_status_t stream_session_init(switch_core_session_t *session,
                                        responseHandler_t responseHandler,
                                        uint32_t samples_per_second,
                                        char *wsUri,
                                        int sampling,
                                        int channels,
                                        switch_bool_t start_muted,
                                        void **ppUserData)
    {
        int deflate = 0, heart_beat = 0;
        bool suppressLog = false;
        const char* buffer_size;
        const char* extra_headers;
        int rtp_packets = 1;
        bool no_reconnect = false;
        const char* tls_cafile = NULL;;
        const char* tls_keyfile = NULL;;
        const char* tls_certfile = NULL;;
        const char* openai_api_key = NULL;;
        bool tls_disable_hostname_validation = false;
        bool disable_audiofiles = false;

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

        const char* heartBeat = switch_channel_get_variable(channel, "STREAM_HEART_BEAT");
        if (heartBeat) {
            char *endptr;
            long value = strtol(heartBeat, &endptr, 10);
            if (*endptr == '\0' && value <= INT_MAX && value >= INT_MIN) {
                heart_beat = (int) value;
            }
        }

        if ((buffer_size = switch_channel_get_variable(channel, "STREAM_BUFFER_SIZE"))) {
            int bSize = atoi(buffer_size);
            if(bSize % 20 != 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "%s: Buffer size of %s is not a multiple of 20ms. Using default 20ms.\n",
                                  switch_channel_get_name(channel), buffer_size);
            } else if(bSize >= 20){
                rtp_packets = bSize/20;
            }
        }

        if (openai_api_key) {
            char headers_buf[1024] = {0};
            snprintf(headers_buf, sizeof(headers_buf),
                     "{\"Authorization\": \"Bearer %s\"}", openai_api_key); 
            extra_headers = headers_buf;
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "OPENAI_API_KEY is not set. Assuming you set STREAM_EXTRA_HEADERS variable.\n");
            extra_headers = switch_channel_get_variable(channel, "STREAM_EXTRA_HEADERS");
        }

        // allocate per-session tech_pvt
        auto* tech_pvt = static_cast<private_t *>(switch_core_session_alloc(session, sizeof(private_t)));

        if (!tech_pvt) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "error allocating memory!\n");
            return SWITCH_STATUS_FALSE;
        }
        if (SWITCH_STATUS_SUCCESS != stream_data_init(tech_pvt, session, wsUri, samples_per_second, sampling, channels, responseHandler, deflate, heart_beat,
                                                        suppressLog, rtp_packets, extra_headers, no_reconnect, tls_cafile, tls_keyfile, tls_certfile, tls_disable_hostname_validation, disable_audiofiles, start_muted)) {
            destroy_tech_pvt(tech_pvt);
            return SWITCH_STATUS_FALSE;
        }

        *ppUserData = tech_pvt;

        return SWITCH_STATUS_SUCCESS;
    }

    switch_bool_t stream_frame(switch_media_bug_t *bug) {
        auto *tech_pvt = static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
        if (!tech_pvt || tech_pvt->audio_paused || tech_pvt->user_audio_muted) return SWITCH_TRUE;

        if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS) {
            return SWITCH_TRUE;
        }

        auto *pAudioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);

        if (!pAudioStreamer || !pAudioStreamer->isConnected()) {
            switch_mutex_unlock(tech_pvt->mutex);
            return SWITCH_TRUE;
        }

        // Get persistent buffers (allocated once per session, reused across all frames)
        auto *bufs = static_cast<StreamBuffers*>(tech_pvt->stream_buffers);

        auto flush_sbuffer = [&]() {
            switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
            if (inuse > 0) {
                bufs->flush_buffer.resize(inuse);
                switch_buffer_read(tech_pvt->sbuffer, bufs->flush_buffer.data(), inuse);
                switch_buffer_zero(tech_pvt->sbuffer);
                pAudioStreamer->writeAudioDelta(bufs->flush_buffer.data(), inuse);
            }
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
                if (tech_pvt->rtp_packets == 1) {
                    pAudioStreamer->writeAudioDelta(static_cast<uint8_t *>(frame.data), frame.datalen);
                } else {
                    size_t write_len = frame.datalen;
                    const uint8_t *write_data = static_cast<const uint8_t *>(frame.data);
                    switch_size_t free_space = switch_buffer_freespace(tech_pvt->sbuffer);
                    if (write_len > free_space) {
                        flush_sbuffer();
                        free_space = switch_buffer_freespace(tech_pvt->sbuffer);
                    }
                    // Only write if buffer has enough space
                    if (write_len <= free_space) {
                        switch_buffer_write(tech_pvt->sbuffer, write_data, write_len);
                        if (switch_buffer_freespace(tech_pvt->sbuffer) == 0) {
                            flush_sbuffer();
                        }
                    } else {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                            "%s: Dropping %zu bytes of audio data, buffer capacity exceeded\n",
                            tech_pvt->sessionId, write_len);
                    }
                }
                continue;
            }

            size_t available = switch_buffer_freespace(tech_pvt->sbuffer);
            spx_uint32_t in_len = frame.samples;
            spx_uint32_t out_len = available / (tech_pvt->channels * sizeof(spx_int16_t));
            if (out_len == 0) {
                flush_sbuffer();
                available = switch_buffer_freespace(tech_pvt->sbuffer);
                out_len = available / (tech_pvt->channels * sizeof(spx_int16_t));
                // Skip processing if buffer still has no space after flushing
                if (out_len == 0) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                        "%s: Buffer full, cannot process resampled frame\n",
                        tech_pvt->sessionId);
                    continue;
                }
            }

            bufs->resample_buffer.resize(out_len * tech_pvt->channels);

            if (tech_pvt->channels == 1) {
                speex_resampler_process_int(
                    tech_pvt->resampler,
                    0,
                    static_cast<const spx_int16_t *>(frame.data),
                    &in_len,
                    bufs->resample_buffer.data(),
                    &out_len
                );
            } else {
                speex_resampler_process_interleaved_int(
                    tech_pvt->resampler,
                    static_cast<const spx_int16_t *>(frame.data),
                    &in_len,
                    bufs->resample_buffer.data(),
                    &out_len
                );
            }

            size_t bytes_written = out_len * tech_pvt->channels * sizeof(spx_int16_t);
            if (bytes_written > 0) {
                // For 20ms packets, send immediately without buffering
                if (tech_pvt->rtp_packets == 1) {
                    pAudioStreamer->writeAudioDelta(reinterpret_cast<uint8_t *>(bufs->resample_buffer.data()), bytes_written);
                } else {
                    // Check if buffer has enough space before writing
                    switch_size_t free_space = switch_buffer_freespace(tech_pvt->sbuffer);
                    if (bytes_written > free_space) {
                        flush_sbuffer();
                        free_space = switch_buffer_freespace(tech_pvt->sbuffer);
                    }
                    if (bytes_written <= free_space) {
                        switch_buffer_write(
                            tech_pvt->sbuffer,
                            reinterpret_cast<const uint8_t *>(bufs->resample_buffer.data()),
                            bytes_written
                        );
                        if (switch_buffer_freespace(tech_pvt->sbuffer) == 0) {
                            flush_sbuffer();
                        }
                    } else {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                            "%s: Dropping %zu bytes of resampled audio data, buffer capacity exceeded\n",
                            tech_pvt->sessionId, bytes_written);
                    }
                }
            }
        }

        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
    }

    switch_bool_t write_frame(switch_core_session_t *session, switch_media_bug_t *bug) {
        private_t *tech_pvt = static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
        if (!tech_pvt || tech_pvt->audio_paused) {
            return SWITCH_TRUE;
        }

        switch_frame_t *frame = switch_core_media_bug_get_write_replace_frame(bug);
        auto codec = switch_core_session_get_write_codec(session);  
        if (!frame || !codec || !codec->implementation) {
            return SWITCH_TRUE;
        }

        AudioStreamer *as = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);

        if (!as || !as->isConnected()) {
            return SWITCH_TRUE;
        }

        uint32_t bytes_needed = frame->datalen;
        uint32_t bytes_per_sample = frame->datalen / frame->samples;

        if (bytes_needed > frame->buflen) { // may be useless
            bytes_needed = frame->buflen;  
        }

        uint32_t inuse = switch_buffer_inuse(tech_pvt->playback_buffer);

        // push a chunk in the audio buffer used treated as cache
        if (as->clear_requested()) {
            switch_buffer_zero(tech_pvt->playback_buffer);
            inuse = 0;
        }
        bool chunk_enqueued = false;
        if (inuse < bytes_needed * 2) {
            std::vector<int16_t> chunk;
            if (as->pop_audio_queue(chunk)) {
                switch_buffer_write(tech_pvt->playback_buffer, chunk.data(), chunk.size() * sizeof(int16_t));
                chunk_enqueued = true;
            }
        }
        if (!chunk_enqueued && inuse == 0) {
            // Openai just finished speaking for interruption or end of response
            if(as->is_openai_speaking() && as->is_response_audio_done()) { 
                as->openai_speech_stopped();
            }
            return SWITCH_TRUE;
        }

        if (inuse > bytes_needed) {
            inuse = bytes_needed;
        }

        if (tech_pvt->openai_audio_muted) {
            switch_buffer_toss(tech_pvt->playback_buffer, inuse);
        } else {
            switch_byte_t *data = static_cast<switch_byte_t *>(frame->data);

            switch_buffer_read(tech_pvt->playback_buffer, data, inuse);

            if (!as->is_openai_speaking()) {
                as->openai_speech_started();
            }

            frame->datalen = inuse;
            frame->samples = frame->datalen / bytes_per_sample;

            switch_core_media_bug_set_write_replace_frame(bug, frame);
        }

        return SWITCH_TRUE;
    }

    switch_status_t stream_session_cleanup(switch_core_session_t *session, char* text, int channelIsClosing) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        auto *bug = static_cast<switch_media_bug_t *>(switch_channel_get_private(channel, MY_BUG_NAME));
        if(bug)
        {
            auto* tech_pvt = static_cast<private_t*>(switch_core_media_bug_get_user_data(bug));
            char sessionId[MAX_SESSION_ID];

            strncpy(sessionId, tech_pvt->sessionId, MAX_SESSION_ID - 1);
            sessionId[MAX_SESSION_ID - 1] = '\0';

            switch_mutex_lock(tech_pvt->mutex);
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "(%s) stream_session_cleanup\n", sessionId);

            switch_channel_set_private(channel, MY_BUG_NAME, nullptr);
            if (!channelIsClosing) {
                switch_core_media_bug_remove(session, &bug);
            }

            auto* audioStreamer = static_cast<AudioStreamer *>(tech_pvt->pAudioStreamer);
            if(audioStreamer) {
                audioStreamer->deleteFiles();
                stream_session_send_json(session, text);
                finish(tech_pvt);
            }

            destroy_tech_pvt(tech_pvt);

            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "(%s) stream_session_cleanup: connection closed\n", sessionId);
            return SWITCH_STATUS_SUCCESS;
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "stream_session_cleanup: no bug - websocket connection already closed\n");
        return SWITCH_STATUS_FALSE;
    }
}
