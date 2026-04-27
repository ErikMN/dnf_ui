#!/usr/bin/env bash
set -e

# Print colors:
FMT_RED=$(printf '\033[31m')
FMT_GREEN=$(printf '\033[32m')
FMT_BLUE=$(printf '\033[34m')
# FMT_YELLOW=$(printf '\033[33m')
# FMT_WHITE=$(printf '\033[37m')
# FMT_BOLD=$(printf '\033[1m')
FMT_RESET=$(printf '\033[0m')

color_print() {
  local color="$1"
  shift
  echo -e "${color}$*${FMT_RESET}"
}

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-run"
THEME_MODE="${THEME:-default}"
DOCKER_NETWORK_MODE="${DOCKER_NETWORK_MODE:-}"
CACHE_MODE="${CACHE_MODE:-persistent}"
CACHE_VOLUME_NAME="${CACHE_VOLUME_NAME:-dnfui-repo-cache}"

# Optional GTK theme override for Docker UI testing:
THEME_OPTS=()
case "$THEME_MODE" in
"" | "default")
  color_print "$FMT_BLUE" "*** GTK theme: default ***"
  ;;
"dark")
  color_print "$FMT_BLUE" "*** GTK theme: dark ***"
  THEME_OPTS=(-e GTK_THEME=Adwaita:dark)
  ;;
"light")
  color_print "$FMT_BLUE" "*** GTK theme: light ***"
  THEME_OPTS=(-e GTK_THEME=Adwaita)
  ;;
*)
  color_print "$FMT_RED" "*** Invalid THEME value: $THEME_MODE. Use dark, light, or default. ***"
  exit 1
  ;;
esac

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"

# Detect Wayland or X11:
if [ "$XDG_SESSION_TYPE" = "wayland" ] && [ -n "$WAYLAND_DISPLAY" ]; then
  color_print "$FMT_BLUE" "*** Wayland detected ***"
  DISPLAY_OPTS=(
    -e WAYLAND_DISPLAY="$WAYLAND_DISPLAY"
    -e XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR"
    -v "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY:$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
  )
else
  color_print "$FMT_BLUE" "*** X11 detected ***"
  xhost +local:docker >/dev/null 2>&1 || true
  DISPLAY_OPTS=(
    -e DISPLAY="$DISPLAY"
    -v /tmp/.X11-unix:/tmp/.X11-unix
  )
fi

# Ensure image exists:
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  color_print "$FMT_RED" "*** Docker image missing. Run ./docker_setup.sh first. ***"
  exit 1
fi

DOCKER_NETWORK_OPTS=()
if [ -n "$DOCKER_NETWORK_MODE" ]; then
  color_print "$FMT_BLUE" "*** Docker network: $DOCKER_NETWORK_MODE ***"
  DOCKER_NETWORK_OPTS=(--network "$DOCKER_NETWORK_MODE")
else
  color_print "$FMT_BLUE" "*** Docker network: default ***"
fi

CACHE_OPTS=()
case "$CACHE_MODE" in
"persistent")
  color_print "$FMT_BLUE" "*** DNF cache: persistent volume $CACHE_VOLUME_NAME ***"
  CACHE_OPTS=(-v "$CACHE_VOLUME_NAME:/var/cache/libdnf5")
  ;;
"empty")
  color_print "$FMT_BLUE" "*** DNF cache: empty tmpfs ***"
  CACHE_OPTS=(--tmpfs /var/cache/libdnf5)
  ;;
*)
  color_print "$FMT_RED" "*** Invalid CACHE_MODE value: $CACHE_MODE. Use persistent or empty. ***"
  exit 1
  ;;
esac

# Run the build inside the container:
color_print "$FMT_GREEN" "*** Running build inside container... ***"
docker run --rm -it \
  --name "$CONTAINER_NAME" \
  --init \
  "${DOCKER_NETWORK_OPTS[@]}" \
  -w /workspace \
  "${DISPLAY_OPTS[@]}" \
  "${THEME_OPTS[@]}" \
  --device /dev/dri \
  -e GSETTINGS_BACKEND=memory \
  -e FINAL \
  -e ASAN \
  -e DEBUG_TRACE \
  -v "$HOST_DIR:/workspace" \
  "${CACHE_OPTS[@]}" \
  "$IMAGE_NAME" \
  bash /workspace/docker/docker_gui_service_run_inner.sh
