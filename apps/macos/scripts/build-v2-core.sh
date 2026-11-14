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
  # A local HOP_MBEDTLS_SOURCE_DIR builds under mbedtls/; the pinned download
  # (the default on a clean machine) builds under _deps/mbedtls-build/.
  mbed="$build/mbedtls"
  [[ -f "$mbed/library/libmbedtls.a" ]] || mbed="$build/_deps/mbedtls-build"
  libs=("$build/libhop_tls.a" "$build/libhop_core.a" "$mbed/library/libmbedtls.a" "$mbed/library/libmbedx509.a"
        "$mbed/library/libmbedcrypto.a" "$mbed/3rdparty/everest/libeverest.a" "$mbed/3rdparty/p256-m/libp256m.a")
  for lib in "${libs[@]}"; do
    [[ -f "$lib" ]] || { echo "Missing build output: $lib" >&2; exit 1; }
  done
  xcrun libtool -static -o "$archive" "${libs[@]}"
  archives+=("$archive")
done
lipo -create "${archives[@]}" -output "$OUT/libhop-v2-universal.a"
lipo -info "$OUT/libhop-v2-universal.a"
echo "Built isolated v2 native core (not linked or installed): $OUT/libhop-v2-universal.a"
