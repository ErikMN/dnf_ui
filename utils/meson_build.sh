#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ "${FINAL:-}" = "y" ]; then
  BUILD_NAME="final"
  BUILD_TYPE="release"
  FINAL_BUILD="true"
  WARNING_LEVEL="3"
else
  BUILD_NAME="debug"
  BUILD_TYPE="debug"
  FINAL_BUILD="false"
  WARNING_LEVEL="0"
fi

if [ "${ASAN:-}" = "y" ]; then
  SANITIZE="address"
else
  SANITIZE="none"
fi

if [ "${DEBUG_TRACE:-}" = "y" ]; then
  DEBUG_TRACE_BUILD="true"
else
  DEBUG_TRACE_BUILD="false"
fi

BUILD_DIR="$PROJECT_ROOT/build/$BUILD_NAME"
APP_BIN="$PROJECT_ROOT/dnf_ui"
SERVICE_BIN="$PROJECT_ROOT/dnf_ui_transaction_service"
TEST_BIN="$PROJECT_ROOT/dnf_ui_tests"

build_tests="false"
target_names=()
link_app="no"
link_service="no"
link_tests="no"

for arg in "$@"; do
  case "$arg" in
  build-dir)
    echo "$BUILD_DIR"
    exit 0
    ;;
  app)
    target_names+=("dnf_ui")
    link_app="yes"
    ;;
  service)
    target_names+=("dnf_ui_transaction_service")
    link_service="yes"
    ;;
  tests)
    build_tests="true"
    target_names+=("dnf_ui_tests")
    link_tests="yes"
    ;;
  all)
    target_names+=("dnf_ui" "dnf_ui_transaction_service")
    link_app="yes"
    link_service="yes"
    ;;
  *)
    echo "*** Unknown meson build target: $arg ***" >&2
    exit 1
    ;;
  esac
done

if [ "${#target_names[@]}" -eq 0 ]; then
  echo "*** Set at least one meson build target. Use app, service, tests, all, or build-dir. ***" >&2
  exit 1
fi

if [ -d "$BUILD_DIR" ]; then
  meson setup "$BUILD_DIR" \
    --reconfigure \
    --prefix /usr \
    --libexecdir libexec \
    --buildtype "$BUILD_TYPE" \
    -Dwarning_level="$WARNING_LEVEL" \
    -Dbuild_tests="$build_tests" \
    -Ddebug_trace="$DEBUG_TRACE_BUILD" \
    -Dfinal_build="$FINAL_BUILD" \
    -Db_sanitize="$SANITIZE"
else
  meson setup "$BUILD_DIR" \
    --prefix /usr \
    --libexecdir libexec \
    --buildtype "$BUILD_TYPE" \
    -Dwarning_level="$WARNING_LEVEL" \
    -Dbuild_tests="$build_tests" \
    -Ddebug_trace="$DEBUG_TRACE_BUILD" \
    -Dfinal_build="$FINAL_BUILD" \
    -Db_sanitize="$SANITIZE"
fi

meson compile -C "$BUILD_DIR" "${target_names[@]}"

if [ "$link_app" = "yes" ]; then
  ln -sfn "$BUILD_DIR/src/dnf_ui" "$APP_BIN"
fi

if [ "$link_service" = "yes" ]; then
  ln -sfn "$BUILD_DIR/src/service/dnf_ui_transaction_service" "$SERVICE_BIN"
fi

if [ "$link_tests" = "yes" ]; then
  ln -sfn "$BUILD_DIR/test/dnf_ui_tests" "$TEST_BIN"
fi
