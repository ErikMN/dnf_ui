#!/usr/bin/env bash
set -e

# Docker wrapper for SRPM and RPM builds from the tracked working tree.

# Print colors:
FMT_RED=$(printf '\033[31m')
FMT_GREEN=$(printf '\033[32m')
FMT_RESET=$(printf '\033[0m')

color_print() {
  local color="$1"
  shift
  echo -e "${color}$*${FMT_RESET}"
}

IMAGE_NAME="dnfui-dev"
MODE="${1:-}"

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

case "$MODE" in
srpm)
  BUILD_SCRIPT="./packaging/build_srpm.sh"
  ;;
rpm)
  BUILD_SCRIPT="./packaging/build_rpm.sh"
  ;;
*)
  echo "*** Use docker_rpm_build.sh with srpm or rpm ***" >&2
  exit 1
  ;;
esac

# Ensure image exists:
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  color_print "$FMT_RED" "*** Docker image missing. Run ./docker_setup.sh first. ***"
  exit 1
fi

if [ "$MODE" = "srpm" ]; then
  color_print "$FMT_GREEN" "*** Building source RPM inside container... ***"
else
  color_print "$FMT_GREEN" "*** Building RPMs inside container... ***"
fi

docker run --rm \
  --name "dnfui-$MODE-build" \
  --init \
  -w /workspace \
  -e HOME=/tmp/dnfui-home \
  -e HOST_UID="$HOST_UID" \
  -e HOST_GID="$HOST_GID" \
  -e BUILD_SCRIPT="$BUILD_SCRIPT" \
  -v "$HOST_DIR:/workspace" \
  "$IMAGE_NAME" \
  bash -lc '
    set -e

    if ! getent group "$HOST_GID" >/dev/null; then
      groupadd -g "$HOST_GID" dnfui-builder
    fi

    BUILDER_GROUP="$(getent group "$HOST_GID" | cut -d: -f1)"

    if ! getent passwd "$HOST_UID" >/dev/null; then
      useradd -u "$HOST_UID" -g "$BUILDER_GROUP" -M -d "$HOME" -s /bin/bash dnfui-builder
    fi

    BUILDER_USER="$(getent passwd "$HOST_UID" | cut -d: -f1)"

    mkdir -p "$HOME"
    chown "$HOST_UID:$HOST_GID" "$HOME"

    runuser -u "$BUILDER_USER" -- env HOME="$HOME" bash -lc "$BUILD_SCRIPT"
  '
