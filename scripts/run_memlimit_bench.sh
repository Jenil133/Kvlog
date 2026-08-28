#!/usr/bin/env bash
# Grows a kvlog database well past the memory available to it and records
# mixed read/write throughput at each step, once per block-cache size.
#
# Builds the release configuration inside the everpure-dev Docker image,
# then runs kvlog_bench --growth=1 in a container whose cgroup memory limit
# is MEMORY. The page cache is charged to that cgroup too, so once the
# dataset outgrows the limit, reads that miss the block cache go to disk.
# The database lives in the container's own file system (not the bind
# mount), so its pages count against the limit like on a real Linux host.
#
# Usage: scripts/run_memlimit_bench.sh            # full run (long; quiet machine)
#        SMOKE=1 scripts/run_memlimit_bench.sh    # seconds; checks the pipeline
#
# Settings (environment variables, defaults in brackets):
#   MEMORY        cgroup memory limit, docker syntax            [512m]
#   CPUS          docker --cpus                                 [2]
#   MAX_BYTES     stop growing at this logical dataset size     [4294967296 = 4 GiB]
#   STEP_BYTES    logical bytes added per step                  [268435456 = 256 MiB]
#   OPS           mixed ops measured per step                   [200000]
#   READ_PCT      read share of the mixed ops                   [90]
#   KEY_DIST      uniform | zipfian                             [zipfian]
#   VALUE_SIZE    bytes per value                               [1000]
#   CACHE_SIZES   block-cache capacities to compare, in bytes   ["8388608 134217728"]
#   OUT_DIR       CSV directory, relative to the project        [bench/results]
#   IMAGE         Docker image                                  [everpure-dev:latest]
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-everpure-dev:latest}"
MEMORY="${MEMORY:-512m}"
CPUS="${CPUS:-2}"
MAX_BYTES="${MAX_BYTES:-4294967296}"
STEP_BYTES="${STEP_BYTES:-268435456}"
OPS="${OPS:-200000}"
READ_PCT="${READ_PCT:-90}"
KEY_DIST="${KEY_DIST:-zipfian}"
VALUE_SIZE="${VALUE_SIZE:-1000}"
CACHE_SIZES="${CACHE_SIZES:-8388608 134217728}"
OUT_DIR="${OUT_DIR:-bench/results}"
BUILD_DIR="build/linux-gcc-release-bench"
TAG=""

if [[ "${SMOKE:-0}" == "1" ]]; then
  MAX_BYTES=8388608
  STEP_BYTES=2097152
  OPS=5000
  VALUE_SIZE=200
  CACHE_SIZES="1048576 4194304"
  TAG="smoke-"
fi

docker image inspect "$IMAGE" >/dev/null

mount=(--mount "type=bind,source=${PROJECT_DIR},target=/src")

echo "== building kvlog_bench (Release, g++) in ${BUILD_DIR}"
# CMake in the image is 3.28; configure with explicit flags instead of the
# presets so the preset schema version does not matter.
docker run --rm "${mount[@]}" --cpus "$CPUS" -w /src "$IMAGE" bash -euc "
  cmake -S /src -B /src/${BUILD_DIR} -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++ -DKVLOG_BUILD_TESTS=OFF -DKVLOG_BUILD_TOOLS=OFF >/dev/null
  cmake --build /src/${BUILD_DIR} --target kvlog_bench -j 2"

mkdir -p "${PROJECT_DIR}/${OUT_DIR}"
stamp="$(date -u +%Y%m%dT%H%M%SZ)"
for cache in $CACHE_SIZES; do
  csv="${OUT_DIR}/${TAG}growth-mem${MEMORY}-cache${cache}-${KEY_DIST}-r${READ_PCT}-${stamp}.csv"
  echo "== growth run: memory limit ${MEMORY}, block cache ${cache} bytes -> ${csv}"
  # --memory-swap equal to --memory: no swap, so the limit is real.
  docker run --rm "${mount[@]}" --cpus "$CPUS" --memory "$MEMORY" --memory-swap "$MEMORY" \
    -w /src "$IMAGE" bash -euc "
    /src/${BUILD_DIR}/bench/kvlog_bench --db=/var/tmp/kvlog_bench --growth=1 \
      --growth_max_bytes=${MAX_BYTES} --growth_step_bytes=${STEP_BYTES} --ops=${OPS} \
      --read_pct=${READ_PCT} --key_dist=${KEY_DIST} --value_size=${VALUE_SIZE} \
      --block_cache_bytes=${cache} --csv=/src/${csv} --stats=1
    echo \"memory.peak: \$(cat /sys/fs/cgroup/memory.peak 2>/dev/null || echo unavailable)\""
done
echo "== done; CSV files in ${OUT_DIR}"
echo "   tables: python3 scripts/csv_to_markdown.py ${OUT_DIR}/${TAG}growth-*-${stamp}.csv"
