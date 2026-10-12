#!/usr/bin/env bash
# Runs the host-side tests (Linux/macOS): the layer against a fake OpenXR runtime, and the
# updater's release/version/checksum parsing.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
g++ -std=c++17 -O2 -Wall -fPIC -shared -fvisibility=hidden -I"$ROOT/external" "$ROOT/src/layer.cpp" -o "$TMP/libtrigger_snap.so"
g++ -std=c++17 -Wall -I"$ROOT/external" "$ROOT/test/test_layer.cpp" -ldl -o "$TMP/test_layer"
g++ -std=c++17 -Wall "$ROOT/test/test_update_logic.cpp" -o "$TMP/test_update_logic"
"$TMP/test_layer" "$TMP/libtrigger_snap.so"
"$TMP/test_update_logic"
