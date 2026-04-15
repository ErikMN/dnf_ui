#!/usr/bin/env bash
set -e

# Docker GUI run helper that starts the app and transaction service on a shared
# session bus for manual transaction testing inside the container.

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$PROJECT_ROOT/utils/transaction_service_paths.conf"

export DNFUI_MESON_BUILD_ROOT="/tmp/dnfui-build"
BUILD_DIR="$("$PROJECT_ROOT/utils/meson_build.sh" build-dir)"
SERVICE_BIN="$BUILD_DIR/src/service/$TRANSACTION_SERVICE_BIN_NAME"
APP_BIN="$BUILD_DIR/src/dnfui"

echo "*** Building app and transaction service ***"
"$PROJECT_ROOT/utils/meson_build.sh" all

for path in "$SERVICE_BIN" "$APP_BIN"; do
  if [ ! -x "$path" ]; then
    echo "*** Missing runtime file: $path ***" >&2
    exit 1
  fi
done

echo "*** Starting session bus transaction service for Docker GUI testing ***"
echo "*** System bus Polkit tests remain available through the dockerservicesystem targets ***"

dbus-run-session -- bash -lc '
  set -e
  export DNFUI_TRANSACTION_BUS=session
  "'"$SERVICE_BIN"'" --session >/tmp/dnfui-service.log 2>&1 &
  service_pid=$!
  trap "kill $service_pid >/dev/null 2>&1 || true; wait $service_pid >/dev/null 2>&1 || true" EXIT
  gdbus wait --session "'"$TRANSACTION_SERVICE_NAME"'" >/dev/null
  "'"$APP_BIN"'"
'
