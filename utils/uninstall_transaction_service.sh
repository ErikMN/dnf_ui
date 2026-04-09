#!/usr/bin/env bash
set -e

# Native development helper that removes the locally installed transaction
# service files after Fedora testing.

SERVICE_BIN_DEST="/usr/libexec/dnf_ui_transaction_service"
POLICY_DEST="/usr/share/polkit-1/actions/com.fedora.dnfui.policy"
DBUS_SERVICE_DEST="/usr/share/dbus-1/system-services/com.fedora.Dnfui.Transaction1.service"
DBUS_POLICY_DEST="/usr/share/dbus-1/system.d/com.fedora.Dnfui.Transaction1.conf"
SYSTEMD_UNIT_DEST="/usr/lib/systemd/system/com.fedora.dnfui.transaction.service"
SYSTEMD_UNIT_NAME="com.fedora.dnfui.transaction.service"

if [ "$(id -u)" -ne 0 ]; then
  echo "*** uninstall_transaction_service.sh must run as root ***" >&2
  exit 1
fi

systemctl stop "$SYSTEMD_UNIT_NAME" >/dev/null 2>&1 || true

rm -f \
  "$SERVICE_BIN_DEST" \
  "$POLICY_DEST" \
  "$DBUS_SERVICE_DEST" \
  "$DBUS_POLICY_DEST" \
  "$SYSTEMD_UNIT_DEST"

systemctl daemon-reload
gdbus call \
  --system \
  --dest org.freedesktop.DBus \
  --object-path /org/freedesktop/DBus \
  --method org.freedesktop.DBus.ReloadConfig >/dev/null

echo "*** Removed native transaction service files. ***"
