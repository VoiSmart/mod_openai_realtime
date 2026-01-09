#ifndef MOD_OPENAI_AUDIO_STREAM_H
#define MOD_OPENAI_AUDIO_STREAM_H

#include <switch.h>
#include <limits.h>
#include <speex/speex_resampler.h>

#define MY_BUG_NAME "audio_stream"
#define MAX_SESSION_ID (256)
#define MAX_WS_URI (4096)

#define EVENT_CONNECT                 "mod_openai_audio_stream::connect"
#define EVENT_DISCONNECT              "mod_openai_audio_stream::disconnect"
#define EVENT_ERROR                   "mod_openai_audio_stream::error"
#define EVENT_JSON                    "mod_openai_audio_stream::json"
#define EVENT_PLAY                    "mod_openai_audio_stream::play"
#define EVENT_OPENAI_SPEECH_STARTED   "mod_openai_audio_stream::openai_speech_start"
#define EVENT_OPENAI_SPEECH_STOPPED   "mod_openai_audio_stream::openai_speech_stop"

typedef void (*responseHandler_t)(switch_core_session_t* session, const char* eventName, const char* json);

struct private_data {
    switch_mutex_t *mutex;
    char sessionId[MAX_SESSION_ID];
    SpeexResamplerState *resampler;
    responseHandler_t responseHandler;
    void *pAudioStreamer;
    char ws_uri[MAX_WS_URI];
    int sampling;
    int channels;
    int audio_paused:1;
    int user_audio_muted:1;
    int openai_audio_muted:1;
    int close_requested:1;
    switch_buffer_t *sbuffer;
    int rtp_packets;
    switch_buffer_t *playback_buffer;
    void *stream_buffers;
};

typedef struct private_data private_t;

enum notifyEvent_t {
    CONNECT_SUCCESS,
    CONNECT_ERROR,
    CONNECTION_DROPPED,
    MESSAGE
};

#endif //MOD_OPENAI_AUDIO_STREAM_H
