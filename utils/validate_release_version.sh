#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MESON_FILE="${MESON_FILE:-$PROJECT_ROOT/meson.build}"
SPEC_FILE="${SPEC_FILE:-$PROJECT_ROOT/dnf-ui.spec}"
EXPECTED_VERSION="${1:-}"

fail() {
  echo "*** $* ***" >&2
  exit 1
}

meson_version="$(
  sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" "$MESON_FILE" |
    head -n1
)"
spec_version="$(
  awk '/^Version:[[:space:]]*/ { print $2; exit }' "$SPEC_FILE"
)"

if [ -z "$meson_version" ]; then
  fail "Could not read Meson project version from $MESON_FILE"
fi

if [ -z "$spec_version" ]; then
  fail "Could not read RPM spec version from $SPEC_FILE"
fi

if [ "$meson_version" != "$spec_version" ]; then
  echo "Meson version: $meson_version" >&2
  echo "Spec version:  $spec_version" >&2
  fail "Meson project version does not match RPM spec version"
fi

if [ -n "$EXPECTED_VERSION" ]; then
  if [[ ! "$EXPECTED_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    fail "Invalid release version: $EXPECTED_VERSION"
  fi

  if [ "$EXPECTED_VERSION" != "$spec_version" ]; then
    echo "Release version: $EXPECTED_VERSION" >&2
    echo "Spec version:    $spec_version" >&2
    fail "Release version does not match project files"
  fi
fi

echo "Release version check passed: $spec_version"
