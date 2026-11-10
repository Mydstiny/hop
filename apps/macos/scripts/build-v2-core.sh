#!/bin/bash
# Build the isolated v2 core for both Mac architectures. This does not link,
# sign, install or launch Hop, and it is not a user release package.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
OUT="${HOP_V2_BUILD_DIR:-$ROOT/build/v2-native}"
CMAKE="${CMAKE:-$(command -v cmake || true)}"
if [[ -z "$CMAKE" ]]; then
  CMAKE="/Applications/DevEco-Studio.app/Contents/sdk/default/openharmony/native/build-tools/cmake/bin/cmake"
  if [[ ! -x "$CMAKE" ]]; then
    echo "CMake is required to build Hop's encrypted transport. Install CMake or set CMAKE to its executable." >&2
    exit 2
  fi
fi
SDK="$(xcrun --sdk macosx --show-sdk-path)"
mkdir -p "$OUT"
archives=()
for arch in arm64 x86_64; do
  build="$OUT/$arch"
  args=(-S "$REPO/core" -B "$build" -DHOP_ENABLE_EXPERIMENTAL_TLS=ON -DBUILD_TESTING=OFF
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$arch" -DCMAKE_OSX_SYSROOT="$SDK" -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0)
  if [[ -n "${HOP_MBEDTLS_SOURCE_DIR:-}" ]]; then args+=(-DHOP_MBEDTLS_SOURCE_DIR="$HOP_MBEDTLS_SOURCE_DIR"); fi
  "$CMAKE" "${args[@]}"
  "$CMAKE" --build "$build" --target hop_tls --parallel 4
  archive="$OUT/libhop-v2-$arch.a"
  xcrun libtool -static -o "$archive" "$build/libhop_tls.a" "$build/libhop_core.a" \
    "$build/mbedtls/library/libmbedtls.a" "$build/mbedtls/library/libmbedx509.a" \
    "$build/mbedtls/library/libmbedcrypto.a" "$build/mbedtls/3rdparty/everest/libeverest.a" \
    "$build/mbedtls/3rdparty/p256-m/libp256m.a"
  archives+=("$archive")
done
lipo -create "${archives[@]}" -output "$OUT/libhop-v2-universal.a"
lipo -info "$OUT/libhop-v2-universal.a"
echo "Built isolated v2 native core (not linked or installed): $OUT/libhop-v2-universal.a"
