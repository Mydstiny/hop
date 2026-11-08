#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${HOP_BUILD_DIR:-$ROOT/build}"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
ARCHS="${HOP_ARCHS:-arm64 x86_64}"
python3 "$ROOT/scripts/generate-resources.py"
mkdir -p "$OUT/Hop.app/Contents/MacOS" "$OUT/Hop.app/Contents/Resources" "$OUT/module-cache"
SOURCES=("$ROOT"/Sources/*.swift)
BINARIES=()
for arch in $ARCHS; do
  case "$arch" in arm64|x86_64) ;; *) echo "Unsupported architecture: $arch" >&2; exit 2;; esac
  xcrun swiftc -swift-version 5 -O -sdk "$SDK" -target "$arch-apple-macosx14.0" \
    -module-cache-path "$OUT/module-cache" -framework AppKit -framework Network \
    -framework Security -framework ApplicationServices "${SOURCES[@]}" -o "$OUT/Hop-$arch"
  BINARIES+=("$OUT/Hop-$arch")
done
lipo -create "${BINARIES[@]}" -output "$OUT/Hop.app/Contents/MacOS/Hop"
cp "$ROOT/Resources/Info.plist" "$OUT/Hop.app/Contents/Info.plist"
for resource in "$ROOT"/Resources/*.json; do
  if [[ -f "$resource" ]]; then cp "$resource" "$OUT/Hop.app/Contents/Resources/"; fi
done
codesign --force --sign - "$OUT/Hop.app"
lipo -info "$OUT/Hop.app/Contents/MacOS/Hop"
plutil -lint "$OUT/Hop.app/Contents/Info.plist"
echo "Built (not launched): $OUT/Hop.app"
