#!/usr/bin/env bash
set -e

# Docker wrapper for the session bus forced preview failure smoke test.

FMT_RED=$(printf '\033[31m')
FMT_GREEN=$(printf '\033[32m')
FMT_RESET=$(printf '\033[0m')

color_print() {
  local color="$1"
  shift
  echo -e "${color}$*${FMT_RESET}"
}

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-service-preview-failure-test"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"

if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  color_print "$FMT_RED" "*** Docker image missing. Run ./docker_setup.sh first. ***"
  exit 1
fi

color_print "$FMT_GREEN" "*** Running transaction service preview failure test inside container... ***"

docker run --rm -it \
  --name "$CONTAINER_NAME" \
  --init \
  -w /workspace \
  -e FINAL \
  -e ASAN \
  -e DEBUG_TRACE \
  -e DNFUI_MESON_BUILD_ROOT=/tmp/dnfui-build \
  -e SERVICE_TEST_INSTALL_SPEC=cowsay \
  -v "$HOST_DIR:/workspace" \
  "$IMAGE_NAME" \
  bash -c 'BUILD_DIR="$(./utils/meson_build.sh build-dir)" && ./utils/meson_build.sh service tests && SERVICE_BIN="$BUILD_DIR/src/service/dnfui-service" ./utils/test_transaction_service_preview_failure.sh'
