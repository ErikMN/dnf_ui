#!/usr/bin/env bash
set -e

# Docker wrapper for offline backend and transaction smoke tests.
# This warms the repo cache with network access first, then reruns the offline
# tagged tests with container networking disabled.

FMT_RED=$(printf '\033[31m')
FMT_GREEN=$(printf '\033[32m')
FMT_RESET=$(printf '\033[0m')

color_print() {
  local color="$1"
  shift
  echo -e "${color}$*${FMT_RESET}"
}

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-offline-test"
CACHE_VOLUME_NAME="${CACHE_VOLUME_NAME:-dnfui-offline-cache}"
OFFLINE_REPO_SPEC="${DNFUI_TEST_OFFLINE_REPO_SPEC:-cowsay}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"

if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  color_print "$FMT_RED" "*** Docker image missing. Run ./docker_setup.sh first. ***"
  exit 1
fi

cleanup() {
  docker volume rm -f "$CACHE_VOLUME_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker volume create "$CACHE_VOLUME_NAME" >/dev/null

color_print "$FMT_GREEN" "*** Priming DNF cache online inside Docker... ***"
docker run --rm \
  --name "${CONTAINER_NAME}-warm" \
  --init \
  -w /workspace \
  -e FINAL \
  -e ASAN \
  -e DEBUG_TRACE \
  -e DNFUI_MESON_BUILD_ROOT=/tmp/dnfui-build \
  -e DNFUI_TEST_OFFLINE_REPO_SPEC="$OFFLINE_REPO_SPEC" \
  -v "$HOST_DIR:/workspace" \
  -v "$CACHE_VOLUME_NAME:/var/cache/libdnf5" \
  "$IMAGE_NAME" \
  bash -lc 'dnf5 makecache >/dev/null && dnf5 repoquery "$DNFUI_TEST_OFFLINE_REPO_SPEC" >/dev/null && ./utils/meson_build.sh service tests'

color_print "$FMT_GREEN" "*** Running offline Docker tests with networking disabled... ***"
color_print "$FMT_GREEN" "*** Cached repo package spec: $OFFLINE_REPO_SPEC ***"
docker run --rm \
  --name "${CONTAINER_NAME}-offline" \
  --network none \
  --init \
  -w /workspace \
  -e FINAL \
  -e ASAN \
  -e DEBUG_TRACE \
  -e DNFUI_MESON_BUILD_ROOT=/tmp/dnfui-build \
  -e DNFUI_TEST_OFFLINE_REPO_SPEC="$OFFLINE_REPO_SPEC" \
  -v "$HOST_DIR:/workspace" \
  -v "$CACHE_VOLUME_NAME:/var/cache/libdnf5" \
  "$IMAGE_NAME" \
  bash -lc 'BUILD_DIR="$(./utils/meson_build.sh build-dir)" && ./utils/meson_build.sh service tests && "$BUILD_DIR/test/dnfui-tests" "[offline]"'
