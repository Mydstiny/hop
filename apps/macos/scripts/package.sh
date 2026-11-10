#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${HOP_BUILD_DIR:-$ROOT/build}"
HOP_ARCHS='arm64 x86_64' HOP_MAC_ADHOC=1 bash "$ROOT/scripts/build.sh"
codesign --verify --strict "$OUT/Hop.app"
ditto -c -k --sequesterRsrc --keepParent "$OUT/Hop.app" "$OUT/Hop-macos-universal-adhoc.zip"
echo "Ad-hoc only, NOT Developer ID signed or notarized: $OUT/Hop-macos-universal-adhoc.zip"
