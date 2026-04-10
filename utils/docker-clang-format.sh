#!/bin/sh
#
# Run a fixed clang-format version in Docker
#
set -eu

IMAGE_NAME="project-clang-format"
CLANG_VERSION="${CLANG_VERSION:-18}"
FORMAT_BIN="/usr/lib/llvm${CLANG_VERSION}/bin/clang-format"
IMAGE_TAG="${IMAGE_NAME}:${CLANG_VERSION}"

PROJECT_ROOT=$(git rev-parse --show-toplevel 2>/dev/null || pwd)

die() {
  printf "%s\n" "$1" >&2
  exit 1
}

check_docker() {
  command -v docker >/dev/null 2>&1 ||
    die "Docker is not installed. Please install Docker first."
}

ensure_image() {
  if docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
    return
  fi

  printf "Building clang-format Docker image version %s\n" "$CLANG_VERSION"

  docker build -t "$IMAGE_TAG" - <<EOF
FROM alpine:3.20
RUN apk add --no-cache clang${CLANG_VERSION}-extra-tools
WORKDIR /workspace
EOF
}

docker_run() {
  docker run --rm \
    -u "$(id -u):$(id -g)" \
    -v "$PROJECT_ROOT:$PROJECT_ROOT" \
    -w "$PROJECT_ROOT" \
    "$IMAGE_TAG" \
    "$@"
}

docker_format() {
  docker_run \
    "$FORMAT_BIN" \
    -style=file \
    -i \
    -fallback-style=none \
    "$@"
}

format_tree() {
  docker_run \
    find src \
    \( -name "*.c" -o -name "*.cc" -o -name "*.cpp" \
    -o -name "*.h" -o -name "*.hh" -o -name "*.hpp" \) \
    -exec "$FORMAT_BIN" -style=file -i -fallback-style=none {} +
}

main() {
  check_docker
  ensure_image

  if [ "$#" -eq 0 ]; then
    format_tree
    return
  fi

  docker_format "$@"
}

main "$@"
