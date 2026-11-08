#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${HOP_BUILD_DIR:-$ROOT/build}"
# Scope tool selection to this child process; never change global xcode-select.
if ! DEVELOPER_DIR="$(python3 "$ROOT/../../tools/resolve_xcode.py" --print-developer-dir)"; then
  echo "Full Xcode was not found. Install it from Mac App Store and complete its first launch." >&2
  echo "Then rerun this script; it detects installed Xcode even if global xcode-select stays on CLT." >&2
  echo "CLT production builds remain available: bash apps/macos/scripts/build.sh" >&2
  exit 2
fi
export DEVELOPER_DIR
if ! xcodebuild -version; then
  echo "Xcode was located at $DEVELOPER_DIR but is not usable. Open it to complete license/component setup." >&2
  exit 2
fi
if ! xcodebuild -checkFirstLaunchStatus; then
  echo "Xcode first-launch/license/component setup is pending. Complete it in Xcode, then rerun this script." >&2
  exit 2
fi
xcodebuild -project "$ROOT/Hop.xcodeproj" -scheme Hop -configuration Release \
  -derivedDataPath "$OUT/DerivedData" -destination 'generic/platform=macOS' \
  ARCHS='arm64 x86_64' ONLY_ACTIVE_ARCH=NO CODE_SIGNING_ALLOWED=NO build
