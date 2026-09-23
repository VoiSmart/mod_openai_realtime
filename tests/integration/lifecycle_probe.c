/* Test-only interposition: stage stop before removal while hangup enters CLOSE under bug_rwlock. */
#include <dlfcn.h>
#include <fcntl.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#define PROBE_PREFIX "/tmp/mod-openai-lifecycle-"

static void marker(const char *name) {
    int fd = open(name, O_CREAT | O_WRONLY, 0600);
    switch_assert(fd >= 0);
    close(fd);
}

static void wait_before_remove(void) {
    if (unlink(PROBE_PREFIX "arm") != 0) {
        return;
    }
    marker(PROBE_PREFIX "remove-entered");
    for (int attempt = 0; attempt < 10000; ++attempt) {
        if (access(PROBE_PREFIX "continue", F_OK) == 0) {
            return;
        }
        usleep(1000);
    }
    /* A broken test must fail instead of leaving FreeSWITCH waiting indefinitely. */
    abort();
}

/* Keep the old removal entry point intercepted too, so reverting the fix fails the same test. */
switch_status_t switch_core_media_bug_remove(switch_core_session_t *session, switch_media_bug_t **bug) {
    typedef switch_status_t (*remove_fn)(switch_core_session_t *, switch_media_bug_t **);
    remove_fn real_remove = (remove_fn)dlsym(RTLD_NEXT, "switch_core_media_bug_remove");
    switch_assert(real_remove);
    wait_before_remove();
    return real_remove(session, bug);
}

switch_status_t switch_core_media_bug_remove_all_function(switch_core_session_t *session, const char *function) {
    typedef switch_status_t (*remove_all_fn)(switch_core_session_t *, const char *);
    remove_all_fn real_remove = (remove_all_fn)dlsym(RTLD_NEXT, "switch_core_media_bug_remove_all_function");
    switch_assert(real_remove);
    /* Keep in sync with MY_BUG_NAME in mod_openai_audio_stream.h. */
    if (function && strcmp(function, "audio_stream") == 0) {
        wait_before_remove();
    }
    return real_remove(session, function);
}

switch_status_t switch_core_media_bug_close(switch_media_bug_t **bug, switch_bool_t destroy) {
    typedef switch_status_t (*close_fn)(switch_media_bug_t **, switch_bool_t);
    close_fn real_close = (close_fn)dlsym(RTLD_NEXT, "switch_core_media_bug_close");
    switch_assert(real_close);
    if (access(PROBE_PREFIX "remove-entered", F_OK) == 0) {
        marker(PROBE_PREFIX "close-entered");
    }
    return real_close(bug, destroy);
}
