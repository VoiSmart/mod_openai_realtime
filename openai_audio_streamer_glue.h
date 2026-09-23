#ifndef OPENAI_AUDIO_STREAMER_GLUE_H
#define OPENAI_AUDIO_STREAMER_GLUE_H
#include "mod_openai_audio_stream.h"

// Use C linkage when this header is included from C++, while keeping it valid for C callers.
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char message[256];
} stream_error_t;

typedef struct {
    const char *websocket_uri;
    uint32_t capture_input_rate;
    int capture_output_rate;
    int playback_input_rate;
    int channels;
    switch_bool_t start_muted;
    switch_bool_t force_raw_audio_mode;
} stream_start_options_t;

/* Errors belong to one API call; messages must not contain URI, credential or JSON payload data. */
static inline switch_status_t stream_fail(stream_error_t *error, const char *message) {
    if (error) {
        switch_copy_string(error->message, message, sizeof(error->message));
    }
    return SWITCH_STATUS_FALSE;
}

int validate_ws_uri(const char *url, char *wsUri);
switch_status_t stream_session_send_json(switch_core_session_t *session, const char *json, stream_error_t *error);
switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause, stream_error_t *error);
switch_status_t stream_session_set_user_mute(switch_core_session_t *session, int mute, stream_error_t *error);
switch_status_t stream_session_set_openai_mute(switch_core_session_t *session, int mute, stream_error_t *error);
switch_status_t stream_session_init(switch_core_session_t *session, responseHandler_t responseHandler,
                                    const stream_start_options_t *options, void **ppUserData, stream_error_t *error);
switch_status_t stream_session_start(void *pUserData);
void stream_session_release(void *pUserData);
void *stream_session_lifecycle_lock(switch_core_session_t *session);
void stream_session_lifecycle_unlock(void *handle);
switch_bool_t stream_frame(switch_media_bug_t *bug);
switch_bool_t write_frame(switch_core_session_t *session, switch_media_bug_t *bug);
switch_status_t stream_session_cleanup(switch_core_session_t *session, const char *text, stream_error_t *error);
void stream_session_close(switch_core_session_t *session, void *user_data);

#ifdef __cplusplus
}
#endif

#endif // OPENAI_AUDIO_STREAMER_GLUE_H
