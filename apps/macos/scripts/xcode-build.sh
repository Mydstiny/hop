#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${HOP_BUILD_DIR:-$ROOT/build}"
if ! xcodebuild -version; then
  echo "Full Xcode is required for this route. Use bash apps/macos/scripts/build.sh for CLT Swift validation." >&2
  exit 2
fi
xcodebuild -project "$ROOT/Hop.xcodeproj" -scheme Hop -configuration Release \
  -derivedDataPath "$OUT/DerivedData" -destination 'generic/platform=macOS' \
  ARCHS='arm64 x86_64' ONLY_ACTIVE_ARCH=NO CODE_SIGNING_ALLOWED=NO build
