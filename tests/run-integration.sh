#!/bin/sh
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
published_base_image=ghcr.io/voismart/mod-openai-realtime-ci:integration
base_image=${INTEGRATION_BASE_IMAGE:-${published_base_image}}
legacy_test_image=${INTEGRATION_IMAGE-}
test_image=${TEST_IMAGE:-${legacy_test_image:-mod-openai-realtime-tests:local}}

docker_build() {
    if docker buildx version >/dev/null 2>&1; then
        # The test image must be loaded into the local daemon for the docker run below.
        docker buildx build --builder default --load "$@"
    else
        docker build "$@"
    fi
}

if [ -z "${INTEGRATION_BASE_IMAGE-}" ] && ! docker pull "${base_image}"; then
    if docker image inspect "${base_image}" >/dev/null 2>&1; then
        echo "Published integration image could not be pulled; using the cached copy." >&2
    else
        base_image=mod-openai-realtime-integration-base:local
        echo "Published integration image could not be pulled." >&2
        echo "It may not exist yet, the network may be unavailable, or you may not be logged in to ghcr.io." >&2
        echo "Building the integration base image locally." >&2
        echo "A cold build compiles FreeSWITCH and may take up to one hour." >&2
        docker_build \
            --file "${project_dir}/Dockerfile.ci" \
            --target integration \
            --tag "${base_image}" \
            "${project_dir}"
    fi
fi

if [ "${base_image}" = "${test_image}" ]; then
    echo "Integration base and test runner must use different image names." >&2
    exit 1
fi

docker_build \
    --file "${project_dir}/tests/Dockerfile" \
    --build-arg "INTEGRATION_BASE_IMAGE=${base_image}" \
    --tag "${test_image}" \
    "${project_dir}"

if [ -n "${TEST_ARTIFACT_DIR:-}" ]; then
    mkdir -p "${TEST_ARTIFACT_DIR}"
    artifact_dir=$(CDPATH='' cd -- "${TEST_ARTIFACT_DIR}" && pwd)
    docker run --rm \
        --env TEST_ARTIFACT_DIR=/test-artifacts \
        --volume "${artifact_dir}:/test-artifacts:Z" \
        "${test_image}" ./tests/run-ci.sh "$@"
else
    docker run --rm "${test_image}" ./tests/run-ci.sh "$@"
fi
