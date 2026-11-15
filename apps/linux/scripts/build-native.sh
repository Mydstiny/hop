#!/usr/bin/env bash
# Build the three Linux runtime libraries into a fresh absolute directory.
#
#   bash apps/linux/scripts/build-native.sh /abs/out
#
# Mbed TLS 3.6.7 comes from HOP_MBEDTLS_SOURCE_DIR when set, otherwise from the
# pinned release tarball, which is checked against its SHA-256 first.
set -euo pipefail

out=${1:?usage: build-native.sh /absolute/output-directory}
case $out in /*) ;; *) echo "output directory must be absolute" >&2; exit 2 ;; esac
[ ! -e "$out" ] || { echo "choose a fresh output directory" >&2; exit 2; }

root=$(cd "$(dirname "$0")/../../.." && pwd)
cmake=${CMAKE:-cmake}
url=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2
sha256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6

mkdir -p "$out"
work=$(mktemp -d "$out/.work-XXXXXX")
trap 'rm -rf "$work"' EXIT

source_dir=${HOP_MBEDTLS_SOURCE_DIR:-}
if [ -z "$source_dir" ]; then
    curl --fail --location --silent --show-error --proto '=https' --tlsv1.2 -o "$work/mbedtls.tar.bz2" "$url"
    echo "$sha256  $work/mbedtls.tar.bz2" | sha256sum --check --quiet
    mkdir "$work/src"
    tar -xjf "$work/mbedtls.tar.bz2" -C "$work/src" --no-same-owner
    source_dir=$work/src/mbedtls-3.6.7
fi
cp "$source_dir/LICENSE" "$out/mbedtls-LICENSE"

"$cmake" -S "$root/apps/linux/native" -B "$work/build" -DCMAKE_BUILD_TYPE=Release \
    -DHOP_MBEDTLS_SOURCE_DIR="$source_dir"
"$cmake" --build "$work/build" --parallel 4 --target hop_tls_runtime hop_shortcut_runtime hop_clipboard_runtime
for name in tls shortcut clipboard; do
    lib=$(find "$work/build" -maxdepth 1 \( -name "libhop_${name}_runtime.so" -o -name "libhop_${name}_runtime.dylib" \) | head -n 1)
    [ -n "$lib" ] || { echo "missing $name runtime" >&2; exit 1; }
    cp "$lib" "$out/"
done
ls -l "$out"
