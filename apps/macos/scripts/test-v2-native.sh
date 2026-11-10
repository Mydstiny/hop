#!/bin/bash
# Isolated TLS bridge verification with ephemeral identities and localhost only.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
OUT="${HOP_V2_BUILD_DIR:-$ROOT/build/v2-native}"
mkdir -p "$OUT"
HOP_V2_BUILD_DIR="$OUT" bash "$ROOT/scripts/build-v2-core.sh"
tmp="$(mktemp -d "$OUT/test.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
for i in 1 2 3; do
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout "$tmp/key$i.pem" -out "$tmp/cert$i.pem" -days 1 -subj "/CN=Hop temporary test $i" >/dev/null 2>&1
done
mkdir -p "$OUT/module-cache"
xcrun swiftc -swift-version 5 -module-cache-path "$OUT/module-cache" -framework Security \
  "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/V2TrustStore.swift" "$ROOT/native/V2IdentityFactory.swift" "$ROOT/native/V2TLSSocket.swift" "$ROOT/native/V2SessionBridge.swift" \
  "$ROOT/Tests/V2TLSSocketTests.swift" "$OUT/libhop-v2-universal.a" -lc++ -o "$OUT/v2-tls-bridge-test"
"$OUT/v2-tls-bridge-test" "$tmp/cert1.pem" "$tmp/key1.pem" "$tmp/cert2.pem" "$tmp/key2.pem" "$tmp/cert3.pem" "$tmp/key3.pem"
xcrun swiftc -swift-version 5 -module-cache-path "$OUT/module-cache" -framework Security \
  "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/Clipboard.swift" "$ROOT/Sources/V2TrustStore.swift" \
  "$ROOT/Sources/V2PairOffer.swift" "$ROOT/Sources/V2IncomingListener.swift" "$ROOT/Sources/V2OfferExchange.swift" \
  "$ROOT/Sources/V2SelectorReader.swift" "$ROOT/native/V2IdentityFactory.swift" "$ROOT/native/V2TLSSocket.swift" \
  "$ROOT/native/V2SessionBridge.swift" "$ROOT/native/V2ClipboardActor.swift" "$ROOT/native/V2HostCoordinator.swift" \
  "$ROOT/Tests/V2HostCoordinatorTests.swift" "$OUT/libhop-v2-universal.a" -lc++ -o "$OUT/v2-host-coordinator-test"
"$OUT/v2-host-coordinator-test"
xcrun swiftc -swift-version 5 -module-cache-path "$OUT/module-cache" -framework Security \
  "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/Clipboard.swift" "$ROOT/Sources/V2TrustStore.swift" \
  "$ROOT/Sources/V2PairOffer.swift" "$ROOT/Sources/V2IncomingListener.swift" "$ROOT/Sources/V2OfferExchange.swift" \
  "$ROOT/Sources/V2SelectorReader.swift" "$ROOT/native/V2IdentityFactory.swift" "$ROOT/native/V2TLSSocket.swift" \
  "$ROOT/native/V2SessionBridge.swift" "$ROOT/native/V2ClipboardActor.swift" "$ROOT/native/V2HostCoordinator.swift" \
  "$ROOT/native/V2ReceiverCoordinator.swift" "$ROOT/Tests/V2ReceiverCoordinatorTests.swift" "$OUT/libhop-v2-universal.a" -lc++ -o "$OUT/v2-receiver-coordinator-test"
"$OUT/v2-receiver-coordinator-test"
xcrun swiftc -swift-version 5 -module-cache-path "$OUT/module-cache" -framework Security \
  "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/Clipboard.swift" "$ROOT/Sources/V2TrustStore.swift" \
  "$ROOT/Sources/V2PairOffer.swift" "$ROOT/Sources/V2IncomingListener.swift" "$ROOT/Sources/V2OfferExchange.swift" \
  "$ROOT/Sources/V2SelectorReader.swift" "$ROOT/native/V2IdentityFactory.swift" "$ROOT/native/V2TLSSocket.swift" \
  "$ROOT/native/V2SessionBridge.swift" "$ROOT/native/V2ClipboardActor.swift" "$ROOT/native/V2HostCoordinator.swift" \
  "$ROOT/native/V2ReceiverCoordinator.swift" "$ROOT/Tests/V2ReceiverClipboardTests.swift" "$OUT/libhop-v2-universal.a" -lc++ -o "$OUT/v2-receiver-clipboard-test"
"$OUT/v2-receiver-clipboard-test"
xcrun clang++ -std=c++17 -pthread -I "$REPO/core/include" -I "$REPO/apps/harmonyos/tests/fake" \
  -I "$REPO/apps/harmonyos/entry/src/main/cpp" "$REPO/apps/harmonyos/tests/v2_receiver_interop_tool.cpp" \
  "$REPO/apps/harmonyos/entry/src/main/cpp/v2_receiver.cpp" "$OUT/libhop-v2-universal.a" -o "$OUT/harmony-fake-receiver"
xcrun swiftc -swift-version 5 -module-cache-path "$OUT/module-cache" -framework Security \
  "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/SecurityStore.swift" "$ROOT/Sources/HIDKeymap.swift" "$ROOT/Sources/V2Wire.swift" "$ROOT/Sources/V2CapturedInput.swift" "$ROOT/Sources/V2InputRouter.swift" \
  "$ROOT/Sources/V2TrustStore.swift" "$ROOT/Sources/V2PairOffer.swift" "$ROOT/Sources/V2DesktopLayout.swift" \
  "$ROOT/Sources/V2IncomingListener.swift" "$ROOT/Sources/V2OfferExchange.swift" "$ROOT/Sources/V2SelectorReader.swift" "$ROOT/native/V2TLSSocket.swift" \
  "$ROOT/native/V2SessionBridge.swift" "$ROOT/Tests/V2HarmonyInteropTests.swift" "$OUT/libhop-v2-universal.a" -lc++ -o "$OUT/v2-harmony-interop-test"
"$OUT/v2-harmony-interop-test" "$OUT/harmony-fake-receiver" "$tmp/cert1.pem" "$tmp/key1.pem" "$tmp/cert2.pem" "$tmp/key2.pem" "$REPO/protocol/keymap.json"
xcrun swiftc -swift-version 5 -module-cache-path "$OUT/module-cache" \
  "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/SecurityStore.swift" "$ROOT/Sources/HIDKeymap.swift" "$ROOT/Sources/V2Wire.swift" \
  "$ROOT/native/V2MacReceiverInjector.swift" "$ROOT/Tests/V2MacReceiverInjectorTests.swift" -o "$OUT/v2-mac-receiver-injector-test"
"$OUT/v2-mac-receiver-injector-test" "$REPO/protocol/keymap.json"
