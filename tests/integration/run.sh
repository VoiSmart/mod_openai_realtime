#!/bin/sh
set -eu

mock_log=/tmp/mod-openai-mock-events.jsonl
mock_ready=/tmp/mod-openai-mock-ready
mock_server_log=/tmp/mod-openai-mock-server.log
freeswitch_log=/tmp/mod-openai-freeswitch.log
freeswitch_runtime_log=/usr/var/log/freeswitch/freeswitch.log
freeswitch_local_runtime_log=/usr/local/freeswitch/log/freeswitch.log
freeswitch_pid_file=/usr/var/run/freeswitch/freeswitch.pid
project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
artifact_dir=${TEST_ARTIFACT_DIR:-}

if [ -n "${artifact_dir}" ]; then
    mkdir -p "${artifact_dir}"
fi

start_freeswitch() {
    : "${LIFECYCLE_PROBE_PATH:?run through tests/run-ci.sh to build the lifecycle probe}"
    if [ "${ENABLE_INTEGRATION_SANITIZERS:-0}" = "1" ]; then
        asan_runtime=$(gcc -print-file-name=libasan.so)
        if [ ! -f "${asan_runtime}" ]; then
            echo "AddressSanitizer runtime not found: ${asan_runtime}" >&2
            return 1
        fi

        LD_PRELOAD="${asan_runtime}:${LIFECYCLE_PROBE_PATH}" \
            ASAN_OPTIONS="detect_leaks=0:halt_on_error=1" \
            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
            freeswitch -nonat -ncwait >"${freeswitch_log}" 2>&1
    else
        LD_PRELOAD="${LIFECYCLE_PROBE_PATH}" freeswitch -nonat -ncwait >"${freeswitch_log}" 2>&1
    fi
}

wait_for_freeswitch_exit() {
    attempt=0
    while kill -0 "${freeswitch_pid}" 2>/dev/null; do
        attempt=$((attempt + 1))
        if [ "${attempt}" -ge 100 ]; then
            return 1
        fi
        sleep 0.1
    done
}

stop_freeswitch() {
    teardown_status=0
    if ! kill -0 "${freeswitch_pid}" 2>/dev/null; then
        echo "FreeSWITCH exited before teardown" >&2
        return 1
    fi

    if [ "${module_loaded:-0}" = "1" ]; then
        if ! unload_result=$(fs_cli -x "unload mod_openai_audio_stream" 2>&1); then
            echo "fs_cli failed while unloading mod_openai_audio_stream: ${unload_result}" >&2
            teardown_status=1
        fi
        if [ "${teardown_status}" -eq 0 ]; then
            case "${unload_result}" in
                *"+OK"*) module_loaded=0 ;;
                *)
                    echo "could not unload mod_openai_audio_stream: ${unload_result}" >&2
                    teardown_status=1
                    ;;
            esac
        fi
    fi

    if ! fs_cli -x "fsctl shutdown now" >/dev/null 2>&1; then
        echo "fs_cli failed while shutting down FreeSWITCH" >&2
        teardown_status=1
    fi
    if wait_for_freeswitch_exit; then
        return "${teardown_status}"
    fi

    echo "FreeSWITCH did not exit after shutdown" >&2
    kill -TERM "${freeswitch_pid}" >/dev/null 2>&1 || true
    if ! wait_for_freeswitch_exit; then
        kill -KILL "${freeswitch_pid}" >/dev/null 2>&1 || true
        wait_for_freeswitch_exit || true
    fi
    return 1
}

cleanup() {
    status=$?
    trap - EXIT INT TERM

    if [ -n "${freeswitch_pid:-}" ]; then
        if ! stop_freeswitch && [ "${status}" -eq 0 ]; then
            status=1
        fi
        freeswitch_pid=
    fi
    if [ -n "${mock_pid:-}" ]; then
        kill "${mock_pid}" >/dev/null 2>&1 || true
        wait "${mock_pid}" >/dev/null 2>&1 || true
    fi
    if [ -n "${artifact_dir}" ]; then
        cp "${mock_log}" "${artifact_dir}/mock-events.jsonl" 2>/dev/null || true
        cp "${mock_server_log}" "${artifact_dir}/mock-server.log" 2>/dev/null || true
        cp "${freeswitch_log}" "${artifact_dir}/freeswitch-process.log" 2>/dev/null || true
        if [ -f "${freeswitch_runtime_log}" ]; then
            cp "${freeswitch_runtime_log}" "${artifact_dir}/freeswitch-runtime.log" 2>/dev/null || true
        fi
        if [ -f "${freeswitch_local_runtime_log}" ]; then
            cp "${freeswitch_local_runtime_log}" \
                "${artifact_dir}/freeswitch-local-runtime.log" 2>/dev/null || true
        fi
    fi

    if [ "${ENABLE_INTEGRATION_SANITIZERS:-0}" = "1" ]; then
        sanitizer_pattern='AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:'
        for log in "${freeswitch_log}" "${freeswitch_runtime_log}" "${freeswitch_local_runtime_log}"; do
            if [ -f "${log}" ] && grep -Eq "${sanitizer_pattern}" "${log}"; then
                echo "Sanitizer diagnostics found in ${log}" >&2
                if [ "${status}" -eq 0 ]; then
                    status=1
                fi
            fi
        done
    fi

    exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

rm -f "${mock_log}" "${mock_ready}" "${mock_server_log}" "${freeswitch_log}" "${freeswitch_pid_file}"
python3 "${project_dir}/tests/integration/mock_openai_server.py" \
    --event-log "${mock_log}" \
    --ready-file "${mock_ready}" >"${mock_server_log}" 2>&1 &
mock_pid=$!

attempt=0
while [ ! -e "${mock_ready}" ]; do
    if ! kill -0 "${mock_pid}" 2>/dev/null; then
        wait "${mock_pid}" || true
        echo "mock WebSocket server exited before becoming ready" >&2
        cat "${mock_server_log}" >&2 || true
        exit 1
    fi
    attempt=$((attempt + 1))
    if [ "${attempt}" -ge 100 ]; then
        echo "mock WebSocket server did not start" >&2
        exit 1
    fi
    sleep 0.05
done

if ! start_freeswitch; then
    echo "FreeSWITCH failed to start" >&2
    cat "${freeswitch_log}" >&2
    exit 1
fi

if [ ! -f "${freeswitch_pid_file}" ]; then
    echo "FreeSWITCH did not create ${freeswitch_pid_file}" >&2
    exit 1
fi
freeswitch_pid=$(cat "${freeswitch_pid_file}")
case "${freeswitch_pid}" in
    ''|*[!0-9]*)
        echo "FreeSWITCH wrote an invalid PID to ${freeswitch_pid_file}" >&2
        exit 1
        ;;
esac

attempt=0
until fs_cli -x status >/dev/null 2>&1; do
    if ! kill -0 "${freeswitch_pid}" 2>/dev/null; then
        freeswitch_pid=
        echo "FreeSWITCH exited before becoming ready" >&2
        cat "${freeswitch_log}" >&2
        exit 1
    fi
    attempt=$((attempt + 1))
    if [ "${attempt}" -ge 100 ]; then
        echo "FreeSWITCH did not start" >&2
        cat "${freeswitch_log}" >&2
        exit 1
    fi
    sleep 0.1
done

FREESWITCH_RUNTIME_LOG=
for log in "${freeswitch_runtime_log}" "${freeswitch_local_runtime_log}"; do
    if [ -f "${log}" ]; then
        FREESWITCH_RUNTIME_LOG=${log}
        break
    fi
done
if [ -z "${FREESWITCH_RUNTIME_LOG}" ]; then
    echo "FreeSWITCH runtime log was not created; module log assertions would be ineffective" >&2
    exit 1
fi
export FREESWITCH_RUNTIME_LOG

if ! load_result=$(fs_cli -x "load mod_openai_audio_stream"); then
    echo "fs_cli failed while loading mod_openai_audio_stream: ${load_result}" >&2
    exit 1
fi
case "${load_result}" in
    *"+OK"*|*"already loaded"*) ;;
    *)
        echo "could not load mod_openai_audio_stream: ${load_result}" >&2
        exit 1
        ;;
esac

attempt=0
until grep -Fq "mod_openai_audio_stream API successfully loaded" "${FREESWITCH_RUNTIME_LOG}"; do
    attempt=$((attempt + 1))
    if [ "${attempt}" -ge 50 ]; then
        echo "module load was not recorded in ${FREESWITCH_RUNTIME_LOG}" >&2
        tail -200 "${FREESWITCH_RUNTIME_LOG}" >&2
        exit 1
    fi
    sleep 0.1
done
module_loaded=1

if [ "$#" -eq 0 ]; then
    set -- discover -p 'test_*.py'
fi
if ! (cd "${project_dir}/tests/integration" && python3 -m unittest "$@" -v); then
    if ! kill -0 "${mock_pid}" 2>/dev/null; then
        wait "${mock_pid}" || true
        echo "mock WebSocket server exited during the integration tests:" >&2
        cat "${mock_server_log}" >&2 || true
    fi
    echo "Mock WebSocket events after integration-test failure:" >&2
    tail -200 "${mock_log}" >&2 || true
    echo "FreeSWITCH log after integration-test failure:" >&2
    tail -200 "${freeswitch_log}" >&2 || true
    for log in "${freeswitch_runtime_log}" "${freeswitch_local_runtime_log}"; do
        if [ -f "${log}" ]; then
            tail -200 "${log}" >&2 || true
        fi
    done
    exit 1
fi

if ! kill -0 "${mock_pid}" 2>/dev/null; then
    wait "${mock_pid}" || true
    echo "mock WebSocket server exited before the integration suite completed:" >&2
    cat "${mock_server_log}" >&2 || true
    exit 1
fi
