# Tests

The test suite has two layers:

- `unit`: fast tests for production code that has no FreeSWITCH dependency;
- `integration`: tests that build and load the module in a real FreeSWITCH process and connect it to a local mock
  WebSocket server.

The test suite has its own CMake project under `tests/`, so configuring the module itself behaves exactly as before.

## Unit tests

Run all unit tests:

```sh
./tests/run-unit.sh
```

Run them with AddressSanitizer and UndefinedBehaviorSanitizer when the compiler runtimes are installed:

```sh
BUILD_DIR=build/tests-sanitized ./tests/run-unit.sh --sanitizers
```

The unit executables are registered with CTest. The manual commands below default to two parallel jobs for
portability; set `CMAKE_BUILD_PARALLEL_LEVEL` to override the default:

```sh
cmake -S tests -B build/tests
cmake --build build/tests --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
ctest --test-dir build/tests --output-on-failure
```

These tests compile the same `stream_protocol.cpp` and `base64.cpp` files linked into the FreeSWITCH module. No copied
implementations or fake FreeSWITCH headers are used.

## Integration tests

The integration image is built from the `integration` target in `Dockerfile.ci`. It extends the CI SDK with the
pinned FreeSWITCH runtime, its test configuration, and Python. The module and tests are not embedded in the image:
they are built from the current checkout and executed every time the container starts.

The mock server sends known PCM16 tones which are recorded from the FreeSWITCH channel and checked for audible
duration and dominant frequency, exercising the playback queue and resampling path end to end. A compatibility
scenario also reuses `response_id` for a later response and verifies that the peer-provided ID does not suppress
valid playback audio.

```sh
./tests/run-integration.sh
```

By default, the script pulls the verified `integration` alias published by this repository. While the package is
private, authorized users can authenticate with `docker login ghcr.io`; other users automatically use a cached copy
or build the `integration` target locally. `tests/Dockerfile` then adds the current checkout without embedding it in
the base image, and `tests/run-integration.sh` starts a fresh test container.

A cold local fallback compiles FreeSWITCH and can take up to one hour; subsequent unchanged builds reuse Docker's
layer cache. To validate changes to `Dockerfile.ci`, explicitly build and select a local base image:

```sh
docker build --file Dockerfile.ci --target integration \
  --tag mod-openai-realtime-integration:local .
INTEGRATION_BASE_IMAGE=mod-openai-realtime-integration:local ./tests/run-integration.sh
```

`INTEGRATION_BASE_IMAGE` can also select another published tag or digest.
`TEST_IMAGE` controls the local runner image name. The previous `INTEGRATION_IMAGE` override remains supported as an
alias for `TEST_IMAGE`.

`tests/run-ci.sh` is the entry point shared by local Docker runs and GitHub Actions. It runs the sanitized unit suite,
builds and installs the module, and then starts the real FreeSWITCH integration suite.

## CI image lifecycle

GitHub Actions pins both the SDK and integration environments to immutable digests. Normal pull requests therefore
build only the module and test runner; they do not rebuild FreeSWITCH. Pull requests that change `Dockerfile.ci` are
also covered by the `CI Image Checks` workflow, which builds and tests the modified integration target before merge.

After such a pull request is merged, the `CI Images` workflow publishes `sdk-<commit>` and `integration-<commit>`,
tests the published integration digest, and only then updates the `sdk` and `integration` aliases. The consumer
workflow digests are updated in a follow-up pull request, so an image is never consumed merely because a moving alias
changed. Update the SDK references in `build.yml` and `code-checks.yml` together with the integration reference in
`tests.yml`, using digests from the same `CI Images` run.

Keep every image digest referenced by the default branch and at least one previous known-good version for rollback.
Unreferenced per-commit images, especially images left by failed publisher runs, can be removed according to the
repository's GHCR retention policy.

Dependabot checks the SHA-pinned GitHub Actions monthly. The Debian base is referenced through the
`DEBIAN_IMAGE` build argument, so its digest must be reviewed and updated manually; the resulting pull request must
pass `CI Image Checks` before merge.
