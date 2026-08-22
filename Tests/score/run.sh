#!/usr/bin/env bash
# Runs one of the score-side suites and stops as soon as it has finished.
#
#   Tests/score/run.sh <build-dir> <suite> [timeout-seconds]
#   Tests/score/run.sh ~/ossia/score/build-release reconnect
#
# score does not exit after evaluating a --script, so a plain `timeout N` burns
# the whole N on every run -- forty minutes each under a sanitizer, for suites
# whose work is over in two. The report file's last line is the completion
# signal, so watch for it and stop there.
set -u

BUILD=${1:?usage: run.sh <build-dir> <suite> [timeout]}
SUITE=${2:?usage: run.sh <build-dir> <suite> [timeout]}
LIMIT=${3:-1800}

HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$HERE/${SUITE}_e2e.js"
[[ -f "$SCRIPT" ]] || { echo "no such suite: $SCRIPT"; exit 2; }

# The suites write to /tmp/<suite>_report.txt; keep that in one place.
REPORT="/tmp/${SUITE}_report.txt"
rm -f "$REPORT"

export QT_ASSUME_STDERR_HAS_CONSOLE=1
LOG="${LOG:-/tmp/${SUITE}_score.log}"

"$BUILD/ossia-score" --no-gui \
    --script="$(sed 's|/tmp/claude-1000/[^"]*scratchpad|/tmp|g' "$SCRIPT")" \
    > "$LOG" 2>&1 &
PID=$!

deadline=$((SECONDS + LIMIT))
while kill -0 "$PID" 2>/dev/null; do
  if grep -q "CHECKS PASSED\|CHECKS FAILED\|EXCEPTION" "$REPORT" 2>/dev/null; then
    sleep 1   # let the last write land
    kill "$PID" 2>/dev/null
    wait "$PID" 2>/dev/null
    break
  fi
  (( SECONDS > deadline )) && { echo "TIMEOUT after ${LIMIT}s"; kill -9 "$PID" 2>/dev/null; break; }
  sleep 2
done

echo "--- $SUITE"
tail -1 "$REPORT" 2>/dev/null || echo "(no report)"

# Sanitizer findings, if the build has them, split by whose code they are in.
if grep -q "AddressSanitizer\|runtime error:" "$LOG" 2>/dev/null; then
  ours='score-addon-orbbec/(DepthCamera|Backends)/[^ ]*:[0-9]+:[0-9]+: runtime error'
  echo "    hard ASan errors  : $(grep -c 'ERROR: AddressSanitizer' "$LOG")"
  echo "    UBSan, our code   : $(grep -cE "^/[^ ]*$ours" "$LOG")"
  echo "    UBSan, everywhere : $(grep -c 'runtime error:' "$LOG")"
  grep -hE "^/[^ ]*$ours" "$LOG" | sort -u | head
fi
