#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SPEC_FILE="${SPEC_FILE:-$PROJECT_ROOT/dnf-ui.spec}"
RPM_TOPDIR="${RPM_TOPDIR:-$PROJECT_ROOT/rpmbuild}"
RPM_TMPDIR="${RPM_TMPDIR:-$RPM_TOPDIR/TMP}"

"$SCRIPT_DIR/build_srpm.sh"

rpmbuild -ba \
  --define "_topdir $RPM_TOPDIR" \
  --define "_tmppath $RPM_TMPDIR" \
  "$RPM_TOPDIR/SPECS/$(basename "$SPEC_FILE")"
