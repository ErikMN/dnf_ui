#!/usr/bin/env bash
set -e

IMAGE_NAME="dnfui-dev"
CONTAINER_NAME="dnfui-shell"

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
HOST_DIR="$PROJECT_ROOT"

# Ensure image exists:
if ! docker image inspect "$IMAGE_NAME" >/dev/null 2>&1; then
  echo "*** Docker image missing. Run ./docker_setup.sh first. ***"
  exit 1
fi

# Run an interactive shell inside the container:
docker run --rm -it \
  --name "$CONTAINER_NAME" \
  --init \
  -w /workspace \
  -v "$HOST_DIR:/workspace" \
  "$IMAGE_NAME" \
  bash
