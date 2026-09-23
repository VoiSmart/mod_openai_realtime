#!/bin/sh
set -eu

project_dir=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
parallel_jobs=${CMAKE_BUILD_PARALLEL_LEVEL:-}

case "$#" in
    0)
        sanitizers=OFF
        default_build_dir="${project_dir}/build/tests"
        ;;
    1)
        if [ "$1" != "--sanitizers" ]; then
            echo "usage: $0 [--sanitizers]" >&2
            exit 2
        fi
        sanitizers=ON
        default_build_dir="${project_dir}/build/tests-sanitized"
        ;;
    *)
        echo "usage: $0 [--sanitizers]" >&2
        exit 2
        ;;
esac

build_dir=${BUILD_DIR:-${default_build_dir}}

if [ -z "${parallel_jobs}" ]; then
    if command -v nproc >/dev/null 2>&1; then
        parallel_jobs=$(nproc)
    elif command -v sysctl >/dev/null 2>&1; then
        parallel_jobs=$(sysctl -n hw.ncpu 2>/dev/null || printf '2\n')
    else
        parallel_jobs=2
    fi
fi

case "${parallel_jobs}" in
    ''|*[!0-9]*|0)
        echo "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer" >&2
        exit 2
        ;;
esac

cmake -S "${project_dir}/tests" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_INTEGRATION_TESTS=OFF \
    "-DENABLE_TEST_SANITIZERS=${sanitizers}"
cmake --build "${build_dir}" --parallel "${parallel_jobs}"
(
    cd "${build_dir}"
    ctest --output-on-failure --no-tests=error
)
