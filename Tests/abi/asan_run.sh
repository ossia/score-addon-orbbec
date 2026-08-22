#!/usr/bin/env bash
# ASan/UBSan pass over every camera, at the ABI layer.
#
# The harnesses have to be sanitized too: ASan's runtime must be the first
# thing in the initial library list, and a plain executable that dlopens an
# instrumented backend aborts with "ASan runtime does not come first".
set -u
SP="$(cd "$(dirname "$0")" && pwd)"
INC=/home/jcelerier/ossia/score/src/addons/score-addon-orbbec/DepthCamera/Backend
BE=/home/jcelerier/ossia/score/build-asan-ubsan/depth-camera
OUT="$SP/asan-results"
rm -rf "$OUT"; mkdir -p "$OUT"

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -g"

echo "=== building sanitized harnesses ==="
for t in nettest imutest ctltest ctlwrite reopen streamsq gaptest; do
  clang++ -std=c++20 -O1 $SAN -o "$SP/san_$t" "$SP/$t.cpp" -I"$INC" -ldl 2>&1 | head -3
done

export ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1:detect_odr_violation=0:abort_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"
export LSAN_OPTIONS="report_objects=0"

run() { # name, harness, backend, extra args...
  local name=$1 harness=$2 backend=$3; shift 3
  echo "--- $name"
  timeout 180 "$SP/san_$harness" "$BE/score_depthcam_$backend.so" "$BE" "$@" \
      > "$OUT/$name.log" 2>&1
  local rc=$?
  local errs=$(grep -cE "ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:" "$OUT/$name.log")
  echo "    exit=$rc  sanitizer reports=$errs"
}

for b in orbbec realsense k4a freenect freenect2; do
  run "stream-$b"  nettest  "$b" "$b:"
  run "control-$b" ctltest  "$b"
  run "write-$b"   ctlwrite "$b"
  run "reopen-$b"  reopen   "$b"
done
for b in orbbec realsense k4a; do
  run "imu-$b" imutest "$b"
done

echo
echo "=== reports mentioning our own code ==="
grep -l . "$OUT"/*.log >/dev/null 2>&1
grep -nE "ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:" "$OUT"/*.log \
  | sed 's|.*/asan-results/||' | sort | uniq -c | sort -rn | head -40
echo
echo "=== our files in any report ==="
grep -hE "score-addon-orbbec/(DepthCamera|Backends)" "$OUT"/*.log \
  | grep -v "3rdparty" | sed 's/^ *//' | sort -u | head -40
echo "ASAN_RUN_COMPLETE"
