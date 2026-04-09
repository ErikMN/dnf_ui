#!/usr/bin/env bash
set -e

# Native system bus smoke test for the installed transaction service. The apply
# mode is meant to be run as a regular desktop user so Polkit can prompt.

SERVICE_NAME="com.fedora.Dnfui.Transaction1"
MANAGER_PATH="/com/fedora/Dnfui/Transaction1"
MANAGER_METHOD="com.fedora.Dnfui.Transaction1.StartTransaction"
RESULT_METHOD="com.fedora.Dnfui.TransactionRequest1.GetResult"
APPLY_METHOD="com.fedora.Dnfui.TransactionRequest1.Apply"
DEFAULT_REINSTALL_SPEC="bash"
APPLY_MODE="${SERVICE_SYSTEM_APPLY:-}"
REINSTALL_SPEC="${SERVICE_TEST_REINSTALL_NEVRA:-$DEFAULT_REINSTALL_SPEC}"

if [ "$(id -u)" -eq 0 ]; then
  echo "*** Run this test as a regular user, not as root. ***" >&2
  exit 1
fi

if [ -n "$APPLY_MODE" ] && [ -z "${SERVICE_TEST_REINSTALL_NEVRA:-}" ]; then
  echo "*** Set SERVICE_TEST_REINSTALL_NEVRA for the native apply test. ***" >&2
  exit 1
fi

wait_for_result() {
  local transaction_path="$1"
  local expected_stage="$2"
  local expected_success="$3"
  local deadline="$((SECONDS + 60))"
  local result=""

  while :; do
    result="$(gdbus call \
      --system \
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
      return 1
    fi

    sleep 1
  done
}

if [ -n "$APPLY_MODE" ]; then
  echo "*** Running native transaction service apply test ***"
else
  echo "*** Running native transaction service preview test ***"
fi

reply="$(gdbus call \
  --system \
  --dest "$SERVICE_NAME" \
  --object-path "$MANAGER_PATH" \
  --method "$MANAGER_METHOD" \
  "[]" \
  "[]" \
  "[\"$REINSTALL_SPEC\"]")"

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

if [ -n "$APPLY_MODE" ]; then
  echo "*** A Polkit authentication dialog should appear before apply is allowed. ***"

  gdbus call \
    --system \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$APPLY_METHOD" >/dev/null

  apply_result="$(wait_for_result "$transaction_path" "apply-succeeded" "true")"
  echo "$apply_result"
fi
