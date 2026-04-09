#!/usr/bin/env bash
set -e

SERVICE_NAME="com.fedora.Dnfui.Transaction1"
MANAGER_PATH="/com/fedora/Dnfui/Transaction1"
MANAGER_METHOD="com.fedora.Dnfui.Transaction1.StartTransaction"
RESULT_METHOD="com.fedora.Dnfui.TransactionRequest1.GetResult"
PREVIEW_METHOD="com.fedora.Dnfui.TransactionRequest1.GetPreview"
APPLY_METHOD="com.fedora.Dnfui.TransactionRequest1.Apply"
RELEASE_METHOD="com.fedora.Dnfui.TransactionRequest1.Release"
INSTALL_SPEC="${SERVICE_TEST_INSTALL_SPEC:-}"
REINSTALL_SPEC="${SERVICE_TEST_REINSTALL_NEVRA:-}"

# Make this script work from any directory:
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SERVICE_BIN="$PROJECT_ROOT/dnf_ui_transaction_service"

if [ ! -x "$SERVICE_BIN" ]; then
  echo "*** Missing service binary: $SERVICE_BIN ***" >&2
  echo "*** Build it first with: make dnf_ui_transaction_service ***" >&2
  exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "*** serviceapplytest must run as root until Polkit is added ***" >&2
  exit 1
fi

if [ -z "$INSTALL_SPEC" ] && [ -z "$REINSTALL_SPEC" ]; then
  echo "*** Set SERVICE_TEST_INSTALL_SPEC or SERVICE_TEST_REINSTALL_NEVRA before running serviceapplytest ***" >&2
  exit 1
fi

if [ -n "$INSTALL_SPEC" ] && [ -n "$REINSTALL_SPEC" ]; then
  echo "*** Set only one of SERVICE_TEST_INSTALL_SPEC or SERVICE_TEST_REINSTALL_NEVRA ***" >&2
  exit 1
fi

LOG_FILE="$(mktemp)"
cleanup_log() {
  rm -f "$LOG_FILE"
}
trap cleanup_log EXIT

echo "*** Running transaction service apply test ***"
echo "*** WARNING: This test applies a real package transaction on the current system. ***"
if [ -n "$INSTALL_SPEC" ]; then
  echo "*** Package spec: $INSTALL_SPEC ***"
else
  echo "*** Package spec: $REINSTALL_SPEC ***"
fi
echo "*** Use only a harmless package for install tests. ***"
echo "*** Use only a non critical installed package for reinstall tests. ***"

SERVICE_BIN="$SERVICE_BIN" \
SERVICE_NAME="$SERVICE_NAME" \
MANAGER_PATH="$MANAGER_PATH" \
MANAGER_METHOD="$MANAGER_METHOD" \
RESULT_METHOD="$RESULT_METHOD" \
PREVIEW_METHOD="$PREVIEW_METHOD" \
APPLY_METHOD="$APPLY_METHOD" \
RELEASE_METHOD="$RELEASE_METHOD" \
INSTALL_SPEC="$INSTALL_SPEC" \
REINSTALL_SPEC="$REINSTALL_SPEC" \
LOG_FILE="$LOG_FILE" \
dbus-run-session -- bash <<'EOF'
  set -e

  wait_for_result() {
    local transaction_path="$1"
    local expected_stage="$2"
    local expected_success="$3"
    local deadline="$((SECONDS + 60))"
    local result=""

    while :; do
      result="$(gdbus call \
        --session \
        --dest "$SERVICE_NAME" \
        --object-path "$transaction_path" \
        --method "$RESULT_METHOD")"

      if printf "%s\n" "$result" | grep -Fq "'$expected_stage', true, $expected_success,"; then
        printf "%s\n" "$result"
        return 0
      fi

      if printf "%s\n" "$result" | grep -Fq "'preview-running'"; then
        :
      elif printf "%s\n" "$result" | grep -Fq "'apply-running'"; then
        :
      else
        printf "%s\n" "$result"
        echo "*** Transaction did not reach the expected result state ***" >&2
        return 1
      fi

      if [ "$SECONDS" -ge "$deadline" ]; then
        echo "*** Timed out waiting for transaction service result ***" >&2
        echo "*** Service log ***" >&2
        cat "$LOG_FILE" >&2 || true
        return 1
      fi

      sleep 1
    done
  }

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

  start_install="[]"
  start_reinstall="[]"
  expected_preview_spec="$REINSTALL_SPEC"
  if [ -n "$INSTALL_SPEC" ]; then
    start_install="[\"$INSTALL_SPEC\"]"
    expected_preview_spec="$INSTALL_SPEC"
  else
    start_reinstall="[\"$REINSTALL_SPEC\"]"
  fi

  reply="$(gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$MANAGER_PATH" \
    --method "$MANAGER_METHOD" \
    "$start_install" \
    "[]" \
    "$start_reinstall")"

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

  preview_result="$(wait_for_result "$transaction_path" "preview-ready" "true")"
  echo "$preview_result"

  preview_data="$(gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$PREVIEW_METHOD")"
  echo "$preview_data"

  case "$preview_data" in
    *"$expected_preview_spec"* )
      ;;
    * )
      echo "*** Structured preview did not contain the expected package spec ***" >&2
      exit 1
      ;;
  esac

  echo "*** WARNING: Applying a real package transaction now. ***"

  gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$APPLY_METHOD" >/dev/null

  apply_result="$(wait_for_result "$transaction_path" "apply-succeeded" "true")"
  echo "$apply_result"

  case "$apply_result" in
    *"Transaction applied successfully."* )
      ;;
    * )
      echo "*** Transaction apply result did not contain the expected success text ***" >&2
      exit 1
      ;;
  esac

  gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$RELEASE_METHOD" >/dev/null

  set +e
  released_result="$(gdbus call \
    --session \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$RESULT_METHOD" 2>&1)"
  released_status=$?
  set -e

  if [ "$released_status" -eq 0 ]; then
    echo "*** Released transaction request is still reachable ***" >&2
    exit 1
  fi

  echo "*** Service log ***"
  cat "$LOG_FILE"
EOF
