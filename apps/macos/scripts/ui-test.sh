#!/bin/bash
# Explicit native OFFSCREEN UI harness; no product app launch or global input.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${HOP_BUILD_DIR:-$ROOT/build}"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
ARCH="$(uname -m)"
mkdir -p "$OUT/tests" "$OUT/module-cache" "$OUT/ui-test"
SOURCES=()
for source in "$ROOT"/Sources/*.swift; do
  if [[ "$source" != "$ROOT/Sources/main.swift" ]]; then SOURCES+=("$source"); fi
done
xcrun swiftc -swift-version 5 -g -sdk "$SDK" -target "$ARCH-apple-macosx14.0" \
  -module-cache-path "$OUT/module-cache" -framework AppKit -framework Security \
  -framework Network -framework ApplicationServices "${SOURCES[@]}" \
  "$ROOT/Tests/PopoverTests.swift" -o "$OUT/tests/popover-tests"
"$OUT/tests/popover-tests" "$OUT/ui-test"
