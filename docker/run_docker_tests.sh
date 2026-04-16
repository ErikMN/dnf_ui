#!/usr/bin/env bash
set -e

# Run the full Docker-backed test matrix from one command. This covers the
# backend test suite plus all session and system bus service smoke tests.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MAKE_BIN="${MAKE:-make}"

echo "*** Running Docker Catch2 test suite ***"
"$MAKE_BIN" -C "$PROJECT_ROOT" dockertest

echo "*** Running Docker session bus transaction service smoke tests ***"
"$MAKE_BIN" -C "$PROJECT_ROOT" dockerservicetest
"$MAKE_BIN" -C "$PROJECT_ROOT" dockerservicecanceltest
"$MAKE_BIN" -C "$PROJECT_ROOT" dockerserviceapplytest

echo "*** Running Docker system bus transaction service smoke tests ***"
"$MAKE_BIN" -C "$PROJECT_ROOT" dockerservicesystemtest
"$MAKE_BIN" -C "$PROJECT_ROOT" dockerservicesystemdisconnecttest
"$MAKE_BIN" -C "$PROJECT_ROOT" dockerservicesystemapplytest

echo "*** Docker test run completed successfully ***"
