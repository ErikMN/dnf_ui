#!/usr/bin/env bash

CONTAINER_RUNTIME="${CONTAINER_RUNTIME:-docker}"

case "$CONTAINER_RUNTIME" in
docker | podman)
  ;;
*)
  echo "*** Invalid CONTAINER_RUNTIME value: $CONTAINER_RUNTIME. Use docker or podman. ***" >&2
  exit 1
  ;;
esac

if ! command -v "$CONTAINER_RUNTIME" >/dev/null 2>&1; then
  echo "*** $CONTAINER_RUNTIME is not installed. Install it first or set CONTAINER_RUNTIME=docker. ***" >&2
  exit 1
fi

container_image_exists() {
  "$CONTAINER_RUNTIME" image inspect "$1" >/dev/null 2>&1
}

container_missing_image_message() {
  echo "*** $CONTAINER_RUNTIME image missing. Run CONTAINER_RUNTIME=$CONTAINER_RUNTIME ./docker/docker_setup.sh first. ***"
}
