#!/usr/bin/env bash
set -e

SERVICE_NAME="com.fedora.Dnfui.Transaction1"
SERVICE_BIN_DEST="/usr/libexec/dnf_ui_transaction_service"
POLICY_DEST="/usr/share/polkit-1/actions/com.fedora.dnfui.policy"
DBUS_SERVICE_DEST="/usr/share/dbus-1/system-services/com.fedora.Dnfui.Transaction1.service"
DBUS_POLICY_DEST="/usr/share/dbus-1/system.d/com.fedora.Dnfui.Transaction1.conf"
SYSTEMD_UNIT_DEST="/usr/lib/systemd/system/com.fedora.dnfui.transaction.service"

if [ "$(id -u)" -ne 0 ]; then
  echo "*** install_transaction_service.sh must run as root ***" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SERVICE_BIN_SRC="$PROJECT_ROOT/dnf_ui_transaction_service"
POLICY_SRC="$PROJECT_ROOT/packaging/com.fedora.dnfui.policy"
DBUS_SERVICE_SRC="$PROJECT_ROOT/packaging/com.fedora.Dnfui.Transaction1.service"
DBUS_POLICY_SRC="$PROJECT_ROOT/packaging/com.fedora.Dnfui.Transaction1.conf"
SYSTEMD_UNIT_SRC="$PROJECT_ROOT/packaging/com.fedora.dnfui.transaction.service"

if [ ! -x "$SERVICE_BIN_SRC" ]; then
  echo "*** Missing service binary: $SERVICE_BIN_SRC ***" >&2
  echo "*** Build it first with: make dnf_ui_transaction_service ***" >&2
  exit 1
fi

for path in "$POLICY_SRC" "$DBUS_SERVICE_SRC" "$DBUS_POLICY_SRC" "$SYSTEMD_UNIT_SRC"; do
  if [ ! -f "$path" ]; then
    echo "*** Missing packaging file: $path ***" >&2
    exit 1
  fi
done

install -D -m 0755 "$SERVICE_BIN_SRC" "$SERVICE_BIN_DEST"
install -D -m 0644 "$POLICY_SRC" "$POLICY_DEST"
install -D -m 0644 "$DBUS_SERVICE_SRC" "$DBUS_SERVICE_DEST"
install -D -m 0644 "$DBUS_POLICY_SRC" "$DBUS_POLICY_DEST"
install -D -m 0644 "$SYSTEMD_UNIT_SRC" "$SYSTEMD_UNIT_DEST"

systemctl daemon-reload
gdbus call \
  --system \
  --dest org.freedesktop.DBus \
  --object-path /org/freedesktop/DBus \
  --method org.freedesktop.DBus.ReloadConfig >/dev/null

echo "*** Installed $SERVICE_NAME service files for native testing. ***"
echo "*** Run dnf_ui as a regular desktop user and apply a transaction to trigger the Polkit prompt. ***"
