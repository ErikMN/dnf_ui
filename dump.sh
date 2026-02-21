#!/usr/bin/env bash
# Dump all source files into a single file
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR" && pwd)"

OUT="/tmp/a.txt"
: >"$OUT"

for f in "$PROJECT_ROOT"/src/*.[ch]pp "$PROJECT_ROOT"/test/*.[ch]pp; do
  {
    echo "----------------------------------------"
    echo "// BEGIN FILE: $f"
    echo "----------------------------------------"
    cat "$f"
    echo ""
    echo "----------------------------------------"
    echo "// END FILE: $f"
    echo "----------------------------------------"
    echo ""
  } >>"$OUT"
done

echo "Dumped all source files to $OUT"

gedit $OUT & >/dev/null &
