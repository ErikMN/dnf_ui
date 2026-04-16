#!/usr/bin/env bash
set -e

# Run the full native test matrix from one command. This includes the Catch2
# suite, session bus smoke tests, the root-only session bus apply test, and the
# installed system bus smoke tests with cleanup on exit.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MAKE_BIN="${MAKE:-make}"

INSTALL_SPEC="${SERVICE_TEST_INSTALL_SPEC:-}"
REINSTALL_SPEC="${SERVICE_TEST_REINSTALL_NEVRA:-}"

if [ "$(id -u)" -eq 0 ]; then
  echo "*** Run native test aggregation as a regular user, not as root. ***" >&2
  exit 1
fi

if [ -z "$INSTALL_SPEC" ] && [ -z "$REINSTALL_SPEC" ]; then
  echo "*** Set SERVICE_TEST_INSTALL_SPEC or SERVICE_TEST_REINSTALL_NEVRA before running nativetests. ***" >&2
  exit 1
fi

if [ -n "$INSTALL_SPEC" ] && [ -n "$REINSTALL_SPEC" ]; then
  echo "*** Set only one of SERVICE_TEST_INSTALL_SPEC or SERVICE_TEST_REINSTALL_NEVRA. ***" >&2
  exit 1
fi

if ! command -v sudo >/dev/null 2>&1; then
  echo "*** sudo is required for the native apply and installed-service tests. ***" >&2
  exit 1
fi

service_installed="no"
cleanup() {
  if [ "$service_installed" = "yes" ]; then
    echo "*** Cleaning up native transaction service install ***"
    sudo "$MAKE_BIN" -C "$PROJECT_ROOT" serviceuninstall || true
  fi
}
trap cleanup EXIT

echo "*** Running native Catch2 test suite ***"
"$MAKE_BIN" -C "$PROJECT_ROOT" test

echo "*** Running native session bus transaction service smoke tests ***"
"$MAKE_BIN" -C "$PROJECT_ROOT" servicetest
"$MAKE_BIN" -C "$PROJECT_ROOT" servicecanceltest

echo "*** Running native session bus apply smoke test ***"
sudo --preserve-env=SERVICE_TEST_INSTALL_SPEC,SERVICE_TEST_REINSTALL_NEVRA,SERVICE_TEST_TIMEOUT_SECONDS,DEBUG_TRACE,FINAL,ASAN \
  "$MAKE_BIN" -C "$PROJECT_ROOT" serviceapplytest

echo "*** Installing native transaction service for system bus smoke tests ***"
sudo "$MAKE_BIN" -C "$PROJECT_ROOT" serviceinstall
service_installed="yes"

echo "*** Running native system bus transaction service smoke tests ***"
"$MAKE_BIN" -C "$PROJECT_ROOT" servicesystemtest
"$MAKE_BIN" -C "$PROJECT_ROOT" servicesystemdisconnecttest
"$MAKE_BIN" -C "$PROJECT_ROOT" servicesystemapplytest

echo "*** Native test run completed successfully ***"
