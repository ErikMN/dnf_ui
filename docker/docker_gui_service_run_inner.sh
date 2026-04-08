#!/usr/bin/env bash
set -e

SERVICE_NAME="com.fedora.Dnfui.Transaction1"
SERVICE_BIN="/workspace/dnf_ui_transaction_service"
APP_BIN="/workspace/dnf_ui"

echo "*** Building app and transaction service ***"
make clean
make -j"$(nproc)"

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
  export DNF_UI_TRANSACTION_BUS=session
  "'"$SERVICE_BIN"'" --session >/tmp/dnf_ui_transaction_service.log 2>&1 &
  service_pid=$!
  trap "kill $service_pid >/dev/null 2>&1 || true; wait $service_pid >/dev/null 2>&1 || true" EXIT
  gdbus wait --session "'"$SERVICE_NAME"'" >/dev/null
  "'"$APP_BIN"'"
'
