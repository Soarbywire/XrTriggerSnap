#!/usr/bin/env bash
# Cross-compiles the Windows binaries with MinGW-w64 and assembles a release folder.
#   usage: scripts/build-windows.sh [out-dir]      (default: build/package)
# Needs: g++-mingw-w64-x86-64-posix (Debian/Ubuntu). Output has no runtime dependencies
# beyond Windows itself.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/package}"
CXX=x86_64-w64-mingw32-g++-posix
RC=x86_64-w64-mingw32-windres
mkdir -p "$OUT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

$RC -I"$ROOT" "$ROOT/tray/tray.rc" -O coff -o "$TMP/tray_res.o"
$CXX -std=c++17 -O2 -Wall -municode -mwindows \
    "$ROOT/tray/tray.cpp" "$ROOT/tray/updater.cpp" "$TMP/tray_res.o" \
    -o "$OUT/XrTriggerSnap.exe" -static -static-libgcc -static-libstdc++ \
    -lcomctl32 -lshell32 -ladvapi32 -luser32 -lgdi32 -lwinhttp -lbcrypt -s
$CXX -std=c++17 -O2 -Wall -shared -I"$ROOT/external" "$ROOT/src/layer.cpp" \
    -o "$OUT/XR_APILAYER_trigger_snap.dll" -static -static-libgcc -static-libstdc++ -s

cp "$ROOT/dist/XR_APILAYER_trigger_snap.json" "$ROOT/dist/trigger_snap.ini" "$ROOT/README.md" "$OUT/"
echo "Built into $OUT"
