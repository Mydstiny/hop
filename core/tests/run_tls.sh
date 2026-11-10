#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/hop-tls-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
cmake_cmd=${CMAKE:-cmake}
source_arg=''
if [ -n "${HOP_MBEDTLS_SOURCE_DIR:-}" ]; then source_arg="-DHOP_MBEDTLS_SOURCE_DIR=$HOP_MBEDTLS_SOURCE_DIR"; fi
"$cmake_cmd" -S "$root/core" -B "$build_dir/build" -DHOP_ENABLE_EXPERIMENTAL_TLS=ON -DHOP_ENABLE_HARMONY_FAKE_TESTS=ON -DBUILD_TESTING=ON ${source_arg:+"$source_arg"}
"$cmake_cmd" --build "$build_dir/build" --target hop_tls_tests hop_posix_tls_tests hop_posix_session_pump_tests hop_harmony_fake_tls_tests hop_harmony_receiver_network_tests hop_pair_offer_vector_tool hop_identity_fixture_tool --parallel 2
for name in a b bad; do
  openssl req -x509 -newkey rsa:2048 -nodes -keyout "$build_dir/$name.key" -out "$build_dir/$name.crt" -days 2 -subj "/CN=hop-test-$name" >/dev/null 2>&1
done
"$build_dir/build/hop_tls_tests" "$build_dir/a.crt" "$build_dir/a.key" "$build_dir/b.crt" "$build_dir/b.key" "$build_dir/bad.crt" "$build_dir/bad.key"
"$build_dir/build/hop_posix_tls_tests"
"$build_dir/build/hop_posix_session_pump_tests"
"$build_dir/build/hop_harmony_fake_tls_tests"
"$build_dir/build/hop_harmony_receiver_network_tests"
python3 "$root/core/tests/pair_offer_vectors.py" "$build_dir/build/hop_pair_offer_vector_tool"

if [ "$(uname -s)" = Darwin ]; then
  umask 077
  "$build_dir/build/hop_identity_fixture_tool" "$build_dir/generated.crt" "$build_dir/generated.key"
  xcrun swiftc -swift-version 5 -module-cache-path "$build_dir/swift-cache" -framework Security \
    "$root/apps/macos/Sources/Protocol.swift" "$root/apps/macos/Sources/V2TrustStore.swift" \
    "$root/apps/macos/Tests/V2GeneratedIdentityTool.swift" -o "$build_dir/mac-identity-check"
  "$build_dir/mac-identity-check" "$build_dir/generated.crt" "$build_dir/generated.key"
fi
