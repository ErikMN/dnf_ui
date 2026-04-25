#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SUPPRESSIONS="${MEMCHECK_SUPPRESSIONS:-$PROJECT_ROOT/utils/valgrind-dnfui.supp}"
LOG_DIR="${MEMCHECK_LOG_DIR:-$PROJECT_ROOT/build/memcheck}"
ERROR_EXITCODE="${MEMCHECK_ERROR_EXITCODE:-99}"
TIMEOUT="${MEMCHECK_TIMEOUT:-}"
TRACK_FDS="${MEMCHECK_TRACK_FDS:-no}"
TIMEOUT_KILL_DELAY="${MEMCHECK_TIMEOUT_KILL_DELAY:-5}"

mode="${1:-}"
if [ -z "$mode" ]; then
  echo "*** Usage: $0 test|app|command PROGRAM [ARGS...] ***" >&2
  exit 2
fi
shift

if [ "$#" -eq 0 ]; then
  echo "*** Missing program for memcheck mode: $mode ***" >&2
  exit 2
fi

case "$mode" in
test | app | command) ;;
*)
  echo "*** Unknown memcheck mode: $mode ***" >&2
  exit 2
  ;;
esac

if ! command -v valgrind >/dev/null 2>&1; then
  echo "*** valgrind is not installed ***" >&2
  exit 127
fi

if [ ! -f "$SUPPRESSIONS" ]; then
  echo "*** Missing Valgrind suppressions: $SUPPRESSIONS ***" >&2
  exit 2
fi

case "$TRACK_FDS" in
yes | no) ;;
*)
  echo "*** Set MEMCHECK_TRACK_FDS to yes or no ***" >&2
  exit 2
  ;;
esac

mkdir -p "$LOG_DIR"
log_file="$LOG_DIR/$mode.log"
timeout_file="$LOG_DIR/$mode.timeout"
rm -f "$timeout_file"

args=(
  --tool=memcheck
  --leak-check=full
  "--show-leak-kinds=definite,indirect,possible"
  "--errors-for-leak-kinds=definite,indirect"
  --error-exitcode="$ERROR_EXITCODE"
  --track-origins=yes
  --track-fds="$TRACK_FDS"
  --num-callers=40
  --trace-children=yes
  --child-silent-after-fork=yes
  --suppressions="$SUPPRESSIONS"
  --log-file="$log_file"
)

if [ "${MEMCHECK_GEN_SUPPRESSIONS:-no}" = "yes" ]; then
  args+=(--gen-suppressions=all)
fi

echo "*** Running Valgrind Memcheck: $mode ***"
echo "*** Log: $log_file ***"
if [ -n "$TIMEOUT" ]; then
  echo "*** Timeout: $TIMEOUT ***"
fi

child_pid=""
timer_pid=""

stop_memcheck()
{
  echo "*** Stopping Memcheck ***" >&2
  if [ -n "$timer_pid" ]; then
    kill "$timer_pid" >/dev/null 2>&1 || true
  fi
  if [ -n "$child_pid" ]; then
    kill -TERM "$child_pid" >/dev/null 2>&1 || true
    wait "$child_pid" >/dev/null 2>&1 || true
  fi
  exit 130
}

trap stop_memcheck INT TERM

set +e
valgrind "${args[@]}" "$@" &
child_pid="$!"

if [ -n "$TIMEOUT" ]; then
  (
    sleep "$TIMEOUT"
    if kill -0 "$child_pid" >/dev/null 2>&1; then
      touch "$timeout_file"
      kill -TERM "$child_pid" >/dev/null 2>&1 || true
      sleep "$TIMEOUT_KILL_DELAY"
      kill -KILL "$child_pid" >/dev/null 2>&1 || true
    fi
  ) &
  timer_pid="$!"
fi

wait "$child_pid"
status="$?"
if [ -n "$timer_pid" ]; then
  kill "$timer_pid" >/dev/null 2>&1 || true
  wait "$timer_pid" >/dev/null 2>&1 || true
fi
set -e
trap - INT TERM

if [ -f "$timeout_file" ]; then
  rm -f "$timeout_file"
  echo "*** Memcheck timed out after $TIMEOUT. See $log_file ***" >&2
  exit 124
fi

if [ "$status" -ne 0 ]; then
  echo "*** Memcheck failed with status $status. See $log_file ***" >&2
  exit "$status"
fi

echo "*** Memcheck passed. See $log_file ***"
