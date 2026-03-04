#ifndef OPENAI_AUDIO_STREAMER_GLUE_H
#define OPENAI_AUDIO_STREAMER_GLUE_H
#include "mod_openai_audio_stream.h"

int validate_ws_uri(const char *url, char *wsUri);
switch_status_t is_valid_utf8(const char *str);
switch_status_t stream_session_send_json(switch_core_session_t *session, char *json);
switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause);
switch_status_t stream_session_set_user_mute(switch_core_session_t *session, int mute);
switch_status_t stream_session_set_openai_mute(switch_core_session_t *session, int mute);
switch_status_t stream_session_init(switch_core_session_t *session, responseHandler_t responseHandler,
                                    uint32_t samples_per_second, char *wsUri, int sampling, int channels,
                                    switch_bool_t start_muted, void **ppUserData);
switch_bool_t stream_frame(switch_media_bug_t *bug);
switch_bool_t write_frame(switch_core_session_t *session, switch_media_bug_t *bug);
switch_status_t stream_session_cleanup(switch_core_session_t *session, char *text, int channelIsClosing);

#endif // OPENAI_AUDIO_STREAMER_GLUE_H
