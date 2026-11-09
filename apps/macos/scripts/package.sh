#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${HOP_BUILD_DIR:-$ROOT/build}"
HOP_ARCHS='arm64 x86_64' bash "$ROOT/scripts/build.sh"
# The day-to-day build may use a machine-local certificate. The zip people download
# is ad-hoc, so it does not depend on that private key.
codesign --force --sign - --identifier "local.hop.macos" "$OUT/Hop.app"
codesign --verify --strict "$OUT/Hop.app"
ditto -c -k --sequesterRsrc --keepParent "$OUT/Hop.app" "$OUT/Hop-macos-universal-adhoc.zip"
echo "Ad-hoc only, NOT Developer ID signed or notarized: $OUT/Hop-macos-universal-adhoc.zip"
