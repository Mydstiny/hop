#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
OUT="${HOP_BUILD_DIR:-$ROOT/build}"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
ARCH="$(uname -m)"
mkdir -p "$OUT/tests" "$OUT/module-cache"
python3 "$ROOT/scripts/generate-resources.py"
CORE=("$ROOT/Sources/Protocol.swift" "$ROOT/Sources/SecurityStore.swift" "$ROOT/Sources/PairingSession.swift" "$ROOT/Sources/MoveCoalescer.swift" "$ROOT/Sources/InputOwnership.swift" "$ROOT/Sources/HIDKeymap.swift" "$ROOT/Sources/V2Wire.swift" "$ROOT/Sources/V2CapturedInput.swift" "$ROOT/Sources/V2InputRouter.swift")
xcrun swiftc -swift-version 5 -g -sdk "$SDK" -target "$ARCH-apple-macosx14.0" \
  -module-cache-path "$OUT/module-cache" -framework Security "${CORE[@]}" \
  "$ROOT/Tests/CoreTests.swift" -o "$OUT/tests/core-tests"
"$OUT/tests/core-tests" "$REPO"
xcrun swiftc -swift-version 5 -g -sdk "$SDK" -target "$ARCH-apple-macosx14.0" \
  -module-cache-path "$OUT/module-cache" -framework Security "${CORE[@]}" \
  "$ROOT/Tests/OwnershipTests.swift" -o "$OUT/tests/ownership-tests"
"$OUT/tests/ownership-tests" "$REPO"
xcrun swiftc -swift-version 5 -D HOP_TESTING -g -sdk "$SDK" -target "$ARCH-apple-macosx14.0" \
  -module-cache-path "$OUT/module-cache" -framework AppKit -framework Security -framework ApplicationServices "${CORE[@]}" \
  "$ROOT/Sources/InputCapture.swift" "$ROOT/Tests/RecoveryTests.swift" -o "$OUT/tests/recovery-tests"
"$OUT/tests/recovery-tests" "$REPO"
xcrun swiftc -swift-version 5 -g -sdk "$SDK" -target "$ARCH-apple-macosx14.0" \
  -module-cache-path "$OUT/module-cache" -framework Network -framework Security "${CORE[@]}" \
  "$ROOT/Sources/TCPServer.swift" "$ROOT/Tests/HeadlessServer.swift" -o "$OUT/tests/headless-server"
python3 "$ROOT/Tests/socket_integration.py" "$OUT/tests/headless-server"
xcrun swiftc -swift-version 5 -g -sdk "$SDK" -target "$ARCH-apple-macosx14.0" \
  -module-cache-path "$OUT/module-cache" -framework Network -framework Security "${CORE[@]}" \
  "$ROOT/Sources/TCPServer.swift" "$ROOT/Tests/TCPOptionsTests.swift" -o "$OUT/tests/tcp-options-tests"
"$OUT/tests/tcp-options-tests"
xcrun swiftc -swift-version 5 -g -sdk "$SDK" -target "$ARCH-apple-macosx14.0" \
  -module-cache-path "$OUT/module-cache" "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/V2IncomingListener.swift" \
  "$ROOT/Tests/V2IncomingListenerTests.swift" -o "$OUT/tests/v2-incoming-listener-tests"
"$OUT/tests/v2-incoming-listener-tests"
xcrun swiftc -swift-version 5 -sdk "$SDK" -target "$ARCH-apple-macosx14.0" -module-cache-path "$OUT/module-cache" \
  -framework Network "$ROOT/Sources/V2Discovery.swift" "$ROOT/Tests/V2DiscoveryTests.swift" -o "$OUT/tests/v2-discovery-tests"
"$OUT/tests/v2-discovery-tests"
xcrun swiftc -swift-version 5 -g -sdk "$SDK" -target "$ARCH-apple-macosx14.0" \
  -module-cache-path "$OUT/module-cache" -framework AppKit -framework ImageIO \
  "$ROOT/Sources/Clipboard.swift" "$ROOT/Tests/ClipboardTests.swift" -o "$OUT/tests/clipboard-tests"
"$OUT/tests/clipboard-tests"
# Disposable test certificate and memory-backed trust store; no Keychain writes.
TRUST_TEMP="$(mktemp -d "$OUT/tests/trust.XXXXXX")"
trap 'rm -rf "$TRUST_TEMP"' EXIT
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TRUST_TEMP/key.pem" -out "$TRUST_TEMP/cert.pem" -days 1 -subj "/CN=Hop test only" >/dev/null 2>&1
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TRUST_TEMP/key2.pem" -out "$TRUST_TEMP/cert2.pem" -days 1 -subj "/CN=Hop peer test only" >/dev/null 2>&1
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout "$TRUST_TEMP/ec-key.pem" -out "$TRUST_TEMP/ec-cert.pem" -days 1 -subj "/CN=Hop EC test only" >/dev/null 2>&1
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout "$TRUST_TEMP/ec-key2.pem" -out "$TRUST_TEMP/ec-cert2.pem" -days 1 -subj "/CN=Hop EC peer only" >/dev/null 2>&1
xcrun swiftc -swift-version 5 -sdk "$SDK" -target "$ARCH-apple-macosx14.0" -module-cache-path "$OUT/module-cache" \
  -framework Security "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/V2TrustStore.swift" "$ROOT/Sources/V2PairOffer.swift" \
  "$ROOT/Sources/V2IncomingListener.swift" "$ROOT/Sources/V2OfferExchange.swift" "$ROOT/Tests/V2OfferExchangeTests.swift" -o "$OUT/tests/v2-offer-exchange-tests"
"$OUT/tests/v2-offer-exchange-tests" "$TRUST_TEMP/ec-cert.pem" "$TRUST_TEMP/ec-cert2.pem"
xcrun swiftc -swift-version 5 -sdk "$SDK" -target "$ARCH-apple-macosx14.0" -module-cache-path "$OUT/module-cache" \
  -framework Security "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/V2TrustStore.swift" "$ROOT/Sources/V2PairOffer.swift" \
  "$ROOT/Sources/V2SelectorReader.swift" "$ROOT/Tests/V2SelectorReaderTests.swift" -o "$OUT/tests/v2-selector-reader-tests"
"$OUT/tests/v2-selector-reader-tests"
TRUST_FP="$(openssl x509 -in "$TRUST_TEMP/cert.pem" -noout -fingerprint -sha256 | cut -d= -f2 | tr -d ':')"
xcrun swiftc -swift-version 5 -sdk "$SDK" -target "$ARCH-apple-macosx14.0" -module-cache-path "$OUT/module-cache" \
  -framework Security "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/V2TrustStore.swift" "$ROOT/Sources/V2PairOffer.swift" "$ROOT/Tests/V2TrustTests.swift" -o "$OUT/tests/v2-trust-tests"
"$OUT/tests/v2-trust-tests" "$TRUST_TEMP/cert.pem" "$TRUST_FP" "$TRUST_TEMP/cert2.pem" "$REPO/protocol/v2-pairing-vectors.json" "$TRUST_TEMP/ec-cert.pem" "$TRUST_TEMP/ec-key.pem" "$TRUST_TEMP/ec-key2.pem"
xcrun swiftc -swift-version 5 -sdk "$SDK" -target "$ARCH-apple-macosx14.0" -module-cache-path "$OUT/module-cache" \
  -framework Security "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/V2TrustStore.swift" "$ROOT/Sources/V2PairOffer.swift" \
  "$ROOT/Sources/V2DesktopLayout.swift" "$ROOT/Tests/V2DesktopLayoutTests.swift" -o "$OUT/tests/v2-desktop-layout-tests"
"$OUT/tests/v2-desktop-layout-tests"
# Synthetic v2 differential tests compile both actual validators; no network/input.
xcrun swiftc -swift-version 5 -sdk "$SDK" -target "$ARCH-apple-macosx14.0" -module-cache-path "$OUT/module-cache" \
  "$ROOT/Sources/Protocol.swift" "$ROOT/Sources/V2Wire.swift" "$ROOT/Tests/V2WireValidationTool.swift" -o "$OUT/tests/v2-wire-swift"
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -I "$REPO/core/include" \
  "$REPO/core/src/wire_validation.cpp" "$REPO/core/tests/wire_parity_tool.cpp" -o "$OUT/tests/v2-wire-cpp"
python3 "$REPO/core/tests/wire_parity.py" "$OUT/tests/v2-wire-cpp" "$OUT/tests/v2-wire-swift"
plutil -lint "$ROOT/Hop.xcodeproj/project.pbxproj" "$ROOT/Resources/Info.plist"
