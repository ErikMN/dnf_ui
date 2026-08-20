#!/usr/bin/env bash
set -e

PROJECT_ROOT="/workspace"
export DNFUI_MESON_BUILD_ROOT="${DNFUI_MESON_BUILD_ROOT:-/tmp/dnfui-build}"
BUILD_DIR="$("$PROJECT_ROOT/utils/meson_build.sh" build-dir)"
CLI_BIN="$BUILD_DIR/src/dnfui-backend-cli"
STARTED_SYSTEM_BUS_PID=""

start_system_bus() {
  mkdir -p /run/dbus

  if [ -S /run/dbus/system_bus_socket ]; then
    return
  fi

  rm -f /run/dbus/pid
  STARTED_SYSTEM_BUS_PID="$(dbus-daemon --system --fork --print-pid=1)"
}

stop_system_bus() {
  if [ -n "$STARTED_SYSTEM_BUS_PID" ]; then
    kill "$STARTED_SYSTEM_BUS_PID" >/dev/null 2>&1 || true
    wait "$STARTED_SYSTEM_BUS_PID" >/dev/null 2>&1 || true
  fi
}

echo "*** Building backend CLI ***"
"$PROJECT_ROOT/utils/meson_build.sh" backend-cli

if [ ! -x "$CLI_BIN" ]; then
  echo "*** Missing runtime file: $CLI_BIN ***" >&2
  exit 1
fi

echo "*** Starting system bus for dnf5daemon CLI testing ***"
start_system_bus
trap stop_system_bus EXIT

echo "*** Checking dnf5daemon D-Bus activation ***"
gdbus introspect --system --dest org.rpm.dnf.v0 --object-path /org/rpm/dnf/v0 >/dev/null

SHELL_RC="/tmp/dnfui-backend-cli-rc"
cat >"$SHELL_RC" <<EOF
alias dnfui-backend-cli='$CLI_BIN'
export DNFUI_BACKEND_CLI='$CLI_BIN'
echo 'Backend CLI ready.'
echo 'Examples:'
echo '  dnfui-backend-cli search bash'
echo '  dnfui-backend-cli list-upgrades'
echo '  dnfui-backend-cli repos list'
echo '  dnfui-backend-cli preview install cowsay'
EOF

bash --rcfile "$SHELL_RC" -i
