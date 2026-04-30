#!/usr/bin/env bash
set -e

IMAGE_NAME="dnfui-dev"

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/container_runtime.sh"

echo "*** Building (or updating) $CONTAINER_RUNTIME image..."
"$CONTAINER_RUNTIME" build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR"
echo "*** $CONTAINER_RUNTIME image is up to date."
