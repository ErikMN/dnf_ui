#!/usr/bin/env bash
set -e

IMAGE_NAME="dnfui-dev"

echo "*** Building (or updating) Docker image..."
docker build -t "$IMAGE_NAME" .
echo "*** Docker image is up to date."
