#!/usr/bin/env bash
set -e

# Inner system bus smoke test used by the Docker service wrappers. It stages the
# policy files, starts a private bus and polkitd, then exercises preview and
# apply through the installed service contract.

SERVICE_NAME="com.fedora.Dnfui.Transaction1"
MANAGER_PATH="/com/fedora/Dnfui/Transaction1"
MANAGER_METHOD="com.fedora.Dnfui.Transaction1.StartTransaction"
RESULT_METHOD="com.fedora.Dnfui.TransactionRequest1.GetResult"
PREVIEW_METHOD="com.fedora.Dnfui.TransactionRequest1.GetPreview"
APPLY_METHOD="com.fedora.Dnfui.TransactionRequest1.Apply"
RELEASE_METHOD="com.fedora.Dnfui.TransactionRequest1.Release"
TEST_USER="dnfuitest"
REINSTALL_SPEC="${SERVICE_TEST_REINSTALL_NEVRA:-bash}"
INSTALL_SPEC="${SERVICE_TEST_INSTALL_SPEC:-}"
ALLOW_APPLY="${SERVICE_SYSTEM_BUS_ALLOW_APPLY:-}"
SERVICE_BIN="/workspace/dnf_ui_transaction_service"
POLICY_FILE="/workspace/packaging/com.fedora.dnfui.policy"
BUS_POLICY_FILE="/workspace/packaging/com.fedora.Dnfui.Transaction1.conf"
RULES_FILE="/etc/polkit-1/rules.d/49-com.fedora.dnfui-test.rules"

if [ "$(id -u)" -ne 0 ]; then
  echo "*** docker_service_system_bus_inner.sh must run as root ***" >&2
  exit 1
fi

if [ ! -x "$SERVICE_BIN" ]; then
  echo "*** Missing service binary: $SERVICE_BIN ***" >&2
  exit 1
fi

if [ ! -f "$POLICY_FILE" ] || [ ! -f "$BUS_POLICY_FILE" ]; then
  echo "*** Missing service packaging files in /workspace/packaging ***" >&2
  exit 1
fi

SERVICE_LOG="$(mktemp)"
POLKIT_LOG="$(mktemp)"

cleanup() {
  if [ -n "${service_pid:-}" ]; then
    kill "$service_pid" >/dev/null 2>&1 || true
    wait "$service_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "${polkit_pid:-}" ]; then
    kill "$polkit_pid" >/dev/null 2>&1 || true
    wait "$polkit_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "${dbus_pid:-}" ]; then
    kill "$dbus_pid" >/dev/null 2>&1 || true
    wait "$dbus_pid" >/dev/null 2>&1 || true
  fi
  rm -f "$SERVICE_LOG" "$POLKIT_LOG"
}
trap cleanup EXIT

print_logs() {
  echo "*** Service log ***"
  cat "$SERVICE_LOG" || true

  echo "*** Polkit log ***"
  cat "$POLKIT_LOG" || true
}

wait_for_result() {
  local transaction_path="$1"
  local expected_stage="$2"
  local expected_success="$3"
  local deadline="$((SECONDS + 60))"
  local result=""

  while :; do
    result="$(runuser -u "$TEST_USER" -- \
      env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
      gdbus call \
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
      print_logs >&2
      return 1
    fi

    if [ "$SECONDS" -ge "$deadline" ]; then
      echo "*** Timed out waiting for transaction service result ***" >&2
      print_logs >&2
      return 1
    fi

    sleep 1
  done
}

if ! id "$TEST_USER" >/dev/null 2>&1; then
  useradd -m "$TEST_USER"
fi

install -D -m 0755 "$SERVICE_BIN" /usr/libexec/dnf_ui_transaction_service
install -D -m 0644 "$POLICY_FILE" /usr/share/polkit-1/actions/com.fedora.dnfui.policy
install -D -m 0644 "$BUS_POLICY_FILE" /usr/share/dbus-1/system.d/com.fedora.Dnfui.Transaction1.conf

if [ -n "$ALLOW_APPLY" ]; then
  install -d /etc/polkit-1/rules.d
  cat >"$RULES_FILE" <<'EOF'
polkit.addRule(function(action, subject) {
  if (action.id == "com.fedora.dnfui.apply-transactions" &&
      subject.user == "dnfuitest") {
    return polkit.Result.YES;
  }

  return polkit.Result.NOT_HANDLED;
});
EOF
fi

mkdir -p /run/dbus
dbus-uuidgen --ensure=/etc/machine-id

dbus_info="$(dbus-daemon --system --fork --nopidfile --print-address=1 --print-pid=1)"
DBUS_SYSTEM_BUS_ADDRESS="$(printf "%s\n" "$dbus_info" | sed -n '1p')"
dbus_pid="$(printf "%s\n" "$dbus_info" | sed -n '2p')"
export DBUS_SYSTEM_BUS_ADDRESS

/usr/lib/polkit-1/polkitd --no-debug >"$POLKIT_LOG" 2>&1 &
polkit_pid=$!

"$SERVICE_BIN" --system >"$SERVICE_LOG" 2>&1 &
service_pid=$!

if [ -n "$ALLOW_APPLY" ]; then
  echo "*** Running transaction service system bus apply test ***"
elif [ -n "$INSTALL_SPEC" ]; then
  echo "*** Running transaction service system bus install preview test ***"
else
  echo "*** Running transaction service system bus test ***"
fi

start_install="[]"
start_remove="[]"
start_reinstall="[\"$REINSTALL_SPEC\"]"
if [ -n "$INSTALL_SPEC" ]; then
  start_install="[\"$INSTALL_SPEC\"]"
  start_reinstall="[]"
fi

gdbus wait --system "$SERVICE_NAME" >/dev/null

reply="$(runuser -u "$TEST_USER" -- \
  env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
  gdbus call \
  --system \
  --dest "$SERVICE_NAME" \
  --object-path "$MANAGER_PATH" \
  --method "$MANAGER_METHOD" \
  "$start_install" \
  "$start_remove" \
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

preview_data="$(runuser -u "$TEST_USER" -- \
  env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
  gdbus call \
  --system \
  --dest "$SERVICE_NAME" \
  --object-path "$transaction_path" \
  --method "$PREVIEW_METHOD")"
echo "$preview_data"

expected_preview_spec="$REINSTALL_SPEC"
if [ -n "$INSTALL_SPEC" ]; then
  expected_preview_spec="$INSTALL_SPEC"
fi

case "$preview_data" in
  *"$expected_preview_spec"* )
    ;;
  * )
    echo "*** Structured preview did not contain the expected package spec ***" >&2
    print_logs >&2
    exit 1
    ;;
esac

if [ -n "$ALLOW_APPLY" ]; then
  runuser -u "$TEST_USER" -- \
    env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
    gdbus call \
    --system \
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
      print_logs >&2
      exit 1
      ;;
  esac

  runuser -u "$TEST_USER" -- \
    env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
    gdbus call \
    --system \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$RELEASE_METHOD" >/dev/null

  set +e
  released_result="$(runuser -u "$TEST_USER" -- \
    env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
    gdbus call \
    --system \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$RESULT_METHOD" 2>&1)"
  released_status=$?
  set -e

  if [ "$released_status" -eq 0 ]; then
    echo "*** Released transaction request is still reachable ***" >&2
    print_logs >&2
    exit 1
  fi
else
  set +e
  apply_output="$(runuser -u "$TEST_USER" -- \
    env DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SYSTEM_BUS_ADDRESS" \
    gdbus call \
    --system \
    --dest "$SERVICE_NAME" \
    --object-path "$transaction_path" \
    --method "$APPLY_METHOD" 2>&1)"
  apply_status=$?
  set -e

  echo "$apply_output"

  if [ "$apply_status" -eq 0 ]; then
    echo "*** Unprivileged apply unexpectedly succeeded on the system bus ***" >&2
    exit 1
  fi

  case "$apply_output" in
    *"AccessDenied"* )
      ;;
    *"Not authorized to apply package transactions."* )
      ;;
    * )
      echo "*** Apply did not fail with the expected authorization error ***" >&2
      print_logs >&2
      exit 1
      ;;
  esac
fi

print_logs
