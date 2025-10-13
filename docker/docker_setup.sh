#!/usr/bin/env bash
set -e

IMAGE_NAME="dnfui-dev"

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "*** Building (or updating) Docker image..."
docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR"
echo "*** Docker image is up to date."
