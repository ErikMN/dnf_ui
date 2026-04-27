#!/usr/bin/env bash
set -e

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-shell"
DOCKER_NETWORK_MODE="${DOCKER_NETWORK_MODE:-}"

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"

# Ensure image exists:
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  echo "*** Docker image missing. Run ./docker_setup.sh first. ***"
  exit 1
fi

docker_network_args=()
if [ -n "$DOCKER_NETWORK_MODE" ]; then
  docker_network_args+=(--network "$DOCKER_NETWORK_MODE")
fi

# Run an interactive shell inside the container:
docker run --rm -it \
  --name "$CONTAINER_NAME" \
  --init \
  "${docker_network_args[@]}" \
  -w /workspace \
  -v "$HOST_DIR:/workspace" \
  "$IMAGE_NAME" \
  bash
