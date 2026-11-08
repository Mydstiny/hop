#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
OUT="${HOP_BUILD_DIR:-$ROOT/build}"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
ARCH="$(uname -m)"
mkdir -p "$OUT/tests" "$OUT/module-cache"
python3 "$ROOT/scripts/generate-resources.py"
CORE=("$ROOT/Sources/Protocol.swift" "$ROOT/Sources/SecurityStore.swift" "$ROOT/Sources/PairingSession.swift" "$ROOT/Sources/MoveCoalescer.swift" "$ROOT/Sources/InputOwnership.swift")
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
plutil -lint "$ROOT/Hop.xcodeproj/project.pbxproj" "$ROOT/Resources/Info.plist"
