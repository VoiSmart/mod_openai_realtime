# Tests

Run commands from the repository root.

## Complete suite

Requires Docker and the IXWebSocket submodule. FreeSWITCH and build dependencies run inside the container;
no OpenAI key or baresip is needed.

```sh
git submodule update --init --recursive
TEST_ARTIFACT_DIR="$PWD/build/test-artifacts" ./tests/run-integration.sh
```

`run-integration.sh` runs both the unit and integration suites. It builds this checkout's
module with AddressSanitizer and UndefinedBehaviorSanitizer, then tests it against FreeSWITCH and a local
WebSocket mock. The container is removed when the run finishes.

To select integration tests, pass module, class or method names (build and unit tests still run):

```sh
./tests/run-integration.sh test_module.ModuleIntegrationTest.test_barge_in_discards_buffered_audio
```

Multiple names are accepted; omit them to run the full suite.

`TEST_ARTIFACT_DIR` saves integration logs, mock events, and recorded WAVs on the host, on success or failure.
The directory is created automatically and is ignored by Git at the path above. Without this variable, those
files disappear with the container; only terminal output remains. Use a different directory per run to keep
previous results.

The script pulls the published GHCR `integration` image. If access requires authentication, use
`docker login ghcr.io`. If the pull fails, it uses a cached copy or builds the base locally; a cold build can
take up to an hour because it compiles FreeSWITCH.

Set `INTEGRATION_BASE_IMAGE` to use a local image or a published digest. To match CI's exact environment, use the
digest in [tests.yml](../.github/workflows/tests.yml). `TEST_IMAGE` overrides the checkout runner image name
(default: `mod-openai-realtime-tests:local`); it must differ from the base image.

`run-ci.sh` and `integration/run.sh` are internal container entry points. Do not run `run-ci.sh` on the host:
it installs the module into FreeSWITCH.

## Fast suite

Requires a C/C++ toolchain, CMake 3.18+, and Python 3.9+. Docker and FreeSWITCH are not needed.

```sh
./tests/run-unit.sh
./tests/run-unit.sh --sanitizers
```

CTest covers protocol parsing, Base64, the playback queue, ESL framing/timeouts, and mock event handling.
Normal and sanitized builds use `build/tests` and `build/tests-sanitized`. Override the path with `BUILD_DIR`
and parallelism with `CMAKE_BUILD_PARALLEL_LEVEL`. Sanitized builds also need the compiler's sanitizer runtimes.
To rerun only the Python tests after building:

```sh
ctest --test-dir build/tests --output-on-failure -R '^python$'
```

## Integration coverage and limits

Calls use FreeSWITCH's `null/` endpoint. Assertions inspect APIs, ESL events, backend messages, logs, and PCM16
recordings to cover JSON/raw transport, playback/resampling, interruptions, pause/mute, stereo capture,
reconnects, API errors, invalid input, headers, and logging. Lifecycle tests cover overlapping stop/hangup,
unload refusal with active streams, and unload/reload after stop. A test-only preload probe controls race timing;
it is not shipped with the module.

ASan/UBSan cover the module and its compiled IXWebSocket code, not all of FreeSWITCH or SpeexDSP. LeakSanitizer
is disabled inside FreeSWITCH; TSan is not run. The suite does not certify real OpenAI behavior, SIP/RTP networking,
runtime WSS certificate verification, stereo playback codecs, mid-call codec changes, or slow-peer backpressure.

## Writing integration tests

- Add `test_*` methods to `ModuleIntegrationTest`, or subclass `ModuleIntegrationBase` in a `test_*.py` file.
  Reuse `start_stream`, `trigger_response` and `stop_stream`; the base class handles cleanup and log checks.
- Declare expected module errors with `expect_module_errors(...)`. Unexpected errors fail the test.
- Wait for events or observable state changes. Use sleeps only when elapsed time is part of the behavior measured.
- Assert the result: received messages, recorded audio or state changes, not just command success.
  A caller tone also enters the recording; restore silence before checking playback silence.

## Other CI checks

The test command does not run lint or static analysis. [Build](../.github/workflows/build.yml) separately checks
Release builds with TLS on/off; [Static Checks](../.github/workflows/code-checks.yml) runs clang-format,
clang-tidy, cppcheck, ShellCheck, actionlint, and Ruff. To use CI's Ruff version locally:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install ruff==0.12.12
.venv/bin/ruff check tests
.venv/bin/ruff format --check tests
```

Use `.venv/bin/ruff format tests` to format, or `.venv/bin/ruff check --fix tests` to apply lint fixes.
Alternatively, activate with `. .venv/bin/activate` and run `ruff` without the path prefix.

Integration workflows retain failure artifacts for seven days. CI image build/publish details are in
[CI Image Checks](../.github/workflows/ci-image-checks.yml) and [CI Images](../.github/workflows/ci-images.yml).
