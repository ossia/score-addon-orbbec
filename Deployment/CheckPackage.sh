#!/usr/bin/env bash
# Checks a staged depth-camera package before it is published.
#
#   Deployment/CheckPackage.sh <package-dir> "<expected backends>"
#   Deployment/CheckPackage.sh build/depth-camera "orbbec freenect k4a"
#
# A package that is merely smaller than it should be is the failure mode to
# worry about here: every backend is optional at configure time, so a missing
# system dependency silently drops one and the build still succeeds. Naming the
# expected set turns that into an error.
set -uo pipefail

DIR=${1:?usage: CheckPackage.sh <package-dir> "<expected backends>"}
EXPECT=${2:?usage: CheckPackage.sh <package-dir> "<expected backends>"}

case "$(uname -s)" in
  Darwin)             EXT=dylib ;;
  MINGW*|MSYS*|CYGWIN*) EXT=dll  ;;
  *)                  EXT=so    ;;
esac

fail=0
note() { echo "::error::$*"; fail=1; }

[[ -d "$DIR" ]] || { note "no package directory at $DIR"; exit 1; }

echo "--- $DIR"
ls -la "$DIR"

### 1. Every backend the platform is meant to ship is there, and nothing else.
for b in $EXPECT; do
  f="$DIR/score_depthcam_$b.$EXT"
  if [[ -f "$f" ]]; then
    echo "  ok    score_depthcam_$b.$EXT ($(du -h "$f" | cut -f1))"
  else
    note "missing backend: score_depthcam_$b.$EXT"
  fi
done

for f in "$DIR"/score_depthcam_*."$EXT"; do
  [[ -e "$f" ]] || continue
  name=$(basename "$f" ".$EXT"); name=${name#score_depthcam_}
  grep -qw -- "$name" <<< "$EXPECT" || note "unexpected backend in package: $name"
done

### 2. One exported symbol each.
###
### The whole design rests on this: score dlopens everything in its package and
### support directories, on Linux with RTLD_GLOBAL, so a backend that exports
### anything beyond its entry point can interpose a sibling's libusb or libjpeg.
### Five SDKs, five private copies.
for f in "$DIR"/score_depthcam_*."$EXT"; do
  [[ -e "$f" ]] || continue
  case "$EXT" in
    so)    syms=$(nm -D --defined-only "$f" | wc -l) ;;
    dylib) syms=$(nm -gU "$f" 2>/dev/null | grep -c ' T \| S \| D ') ;;
    *)     continue ;;   # Windows exports only what is __declspec(dllexport)
  esac
  if [[ "$syms" -ne 1 ]]; then
    note "$(basename "$f") exports $syms symbols, expected exactly 1"
    [[ "$EXT" == so ]] && nm -D --defined-only "$f" | head -20
    [[ "$EXT" == dylib ]] && nm -gU "$f" | head -20
  else
    echo "  ok    $(basename "$f") exports 1 symbol"
  fi
done

### 3. The manifest score reads after installing.
if [[ -f "$DIR/package.json" ]]; then
  python3 -c "import json,sys; d=json.load(open(sys.argv[1])); [sys.exit('package.json missing '+k) for k in ('key','raw_name','name','version','kind') if k not in d]; print('  ok    package.json:', d['raw_name'], d['architecture'] if 'architecture' in d else '')" "$DIR/package.json" || note "package.json is not usable"
else
  note "no package.json in the package"
fi

exit $fail
