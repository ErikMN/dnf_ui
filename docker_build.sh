#!/usr/bin/env bash
set -e

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-run"
HOST_DIR="$(pwd)"

# Detect Wayland or X11:
if [ "$XDG_SESSION_TYPE" = "wayland" ] && [ -n "$WAYLAND_DISPLAY" ]; then
  echo "*** Wayland detected ***"
  DISPLAY_OPTS=(
    -e WAYLAND_DISPLAY="$WAYLAND_DISPLAY"
    -e XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR"
    -v "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY:$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
  )
else
  echo "*** X11 detected ***"
  xhost +local:docker >/dev/null 2>&1 || true
  DISPLAY_OPTS=(
    -e DISPLAY="$DISPLAY"
    -v /tmp/.X11-unix:/tmp/.X11-unix
  )
fi

# Ensure image exists:
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  echo "*** Docker image missing. Run ./docker_setup.sh first. ***"
  exit 1
fi

echo "*** Running build inside container..."
docker run --rm -it \
  --name "$CONTAINER_NAME" \
  --init \
  -w /workspace \
  "${DISPLAY_OPTS[@]}" \
  --device /dev/dri \
  -e GSETTINGS_BACKEND=memory \
  -v "$HOST_DIR:/workspace" \
  "$IMAGE_NAME" \
  bash -c "make clean && make -j$(nproc) && ./dnf_ui"
