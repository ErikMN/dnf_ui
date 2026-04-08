#!/usr/bin/env bash
set -e

SERVICE_NAME="com.fedora.Dnfui.Transaction1"
MANAGER_PATH="/com/fedora/Dnfui/Transaction1"
MANAGER_METHOD="com.fedora.Dnfui.Transaction1.StartTransaction"
RESULT_METHOD="com.fedora.Dnfui.TransactionRequest1.GetResult"

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

echo "*** Running transaction service preview test ***"

SERVICE_BIN="$SERVICE_BIN" \
SERVICE_NAME="$SERVICE_NAME" \
MANAGER_PATH="$MANAGER_PATH" \
MANAGER_METHOD="$MANAGER_METHOD" \
RESULT_METHOD="$RESULT_METHOD" \
LOG_FILE="$LOG_FILE" \
dbus-run-session -- bash <<'EOF'
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
    "[]" \
    "[]" \
    "[\"bash\"]")"

  echo "$reply"

  case "$reply" in
    *"/com/fedora/Dnfui/Transaction1/requests/"* )
      ;;
    * )
      echo "*** Service did not return a transaction object path ***" >&2
      exit 1
      ;;
  esac

  transaction_path="${reply#*objectpath \'}"
  transaction_path="${transaction_path%%\'*}"
  if [ -z "$transaction_path" ]; then
    echo "*** Failed to parse transaction object path ***" >&2
    exit 1
  fi

  deadline=$((SECONDS + 30))
  while :; do
    result="$(gdbus call \
      --session \
      --dest "$SERVICE_NAME" \
      --object-path "$transaction_path" \
      --method "$RESULT_METHOD")"

    if printf "%s\n" "$result" | grep -Fq "'preview-running'"; then
      if [ "$SECONDS" -ge "$deadline" ]; then
        echo "*** Timed out waiting for transaction preview result ***" >&2
        exit 1
      fi
      sleep 1
      continue
    fi

    if printf "%s\n" "$result" | grep -Fq "'preview-ready', true, true,"; then
      echo "$result"
      break
    fi

    echo "$result"
    echo "*** Transaction preview did not finish successfully ***" >&2
    exit 1
  done

  case "$result" in
    *"reinstalled"* )
      ;;
    * )
      echo "*** Transaction preview result did not contain the expected summary text ***" >&2
      exit 1
      ;;
  esac

  echo "*** Service log ***"
  cat "$LOG_FILE"
EOF
