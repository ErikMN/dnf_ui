#!/usr/bin/env bash
set -e

SERVICE_NAME="com.fedora.Dnfui.Transaction1"
MANAGER_PATH="/com/fedora/Dnfui/Transaction1"
MANAGER_METHOD="com.fedora.Dnfui.Transaction1.StartTransaction"

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SERVICE_BIN="$PROJECT_ROOT/dnf_ui_transaction_service"

if [ ! -x "$SERVICE_BIN" ]; then
  echo "*** Missing service binary: $SERVICE_BIN ***" >&2
  echo "*** Build it first with: make dnf_ui_transaction_service ***" >&2
  exit 1
fi

LOG_FILE="$(mktemp)"
cleanup_log() {
  rm -f "$LOG_FILE"
}
trap cleanup_log EXIT

echo "*** Running transaction service proof of concept test ***"

SERVICE_BIN="$SERVICE_BIN" \
SERVICE_NAME="$SERVICE_NAME" \
MANAGER_PATH="$MANAGER_PATH" \
MANAGER_METHOD="$MANAGER_METHOD" \
LOG_FILE="$LOG_FILE" \
dbus-run-session -- bash -lc '
  set -e

  cleanup() {
    if [ -n "${service_pid:-}" ]; then
      kill "$service_pid" >/dev/null 2>&1 || true
      wait "$service_pid" >/dev/null 2>&1 || true
    fi
  }
  trap cleanup EXIT

  "$SERVICE_BIN" --session >"$LOG_FILE" 2>&1 &
  service_pid=$!

  gdbus wait --session "$SERVICE_NAME" >/dev/null

  reply="$(gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$MANAGER_PATH" \
    --method "$MANAGER_METHOD" \
    "[\"bash\"]" \
    "[]" \
    "[]")"

  echo "$reply"

  case "$reply" in
    *"/com/fedora/Dnfui/Transaction1/requests/"* )
      ;;
    * )
      echo "*** Service did not return a transaction object path ***" >&2
      exit 1
      ;;
  esac

  echo "*** Service log ***"
  cat "$LOG_FILE"
'
