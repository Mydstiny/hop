#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
OUT="${HOP_BUILD_DIR:-$ROOT/build}"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
ARCHS="${HOP_ARCHS:-arm64 x86_64}"
python3 "$ROOT/scripts/generate-resources.py"
mkdir -p "$OUT/Hop.app/Contents/MacOS" "$OUT/Hop.app/Contents/Resources" "$OUT/module-cache"
SOURCES=("$ROOT"/Sources/*.swift)
NATIVE=("$ROOT"/native/*.swift)
CORE="$OUT/v2-native"
HOP_V2_BUILD_DIR="$CORE" bash "$ROOT/scripts/build-v2-core.sh"
BINARIES=()
for arch in $ARCHS; do
  case "$arch" in arm64|x86_64) ;; *) echo "Unsupported architecture: $arch" >&2; exit 2;; esac
  xcrun swiftc -swift-version 5 -O -sdk "$SDK" -target "$arch-apple-macosx14.0" \
    -module-cache-path "$OUT/module-cache" -framework AppKit -framework Network \
    -framework Security -framework ApplicationServices "${SOURCES[@]}" "${NATIVE[@]}" \
    "$CORE/libhop-v2-universal.a" -lc++ -o "$OUT/Hop-$arch"
  BINARIES+=("$OUT/Hop-$arch")
done
lipo -create "${BINARIES[@]}" -output "$OUT/Hop.app/Contents/MacOS/Hop"
cp "$ROOT/Resources/Info.plist" "$OUT/Hop.app/Contents/Info.plist"
for resource in "$ROOT"/Resources/*.json "$ROOT"/Resources/*.icns; do
  if [[ -f "$resource" ]]; then cp "$resource" "$OUT/Hop.app/Contents/Resources/"; fi
done
LICENSES="$OUT/Hop.app/Contents/Resources/Licenses"
mkdir -p "$LICENSES/third_party"
cp "$REPO/LICENSE" "$REPO/THIRD_PARTY_NOTICES.md" "$LICENSES/"
cp "$REPO/third_party/mbedtls-3.6.7-LICENSE.txt" "$LICENSES/third_party/"
if [[ "${HOP_MAC_ADHOC:-0}" == 1 ]]; then
  codesign --force --sign - --identifier local.hop.macos "$OUT/Hop.app"
else
  bash "$ROOT/scripts/sign.sh" "$OUT/Hop.app"
fi
lipo -info "$OUT/Hop.app/Contents/MacOS/Hop"
plutil -lint "$OUT/Hop.app/Contents/Info.plist"
echo "Built (not launched): $OUT/Hop.app"
