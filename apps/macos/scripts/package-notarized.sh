#!/bin/bash
# Explicit distribution build. The notarization step submits the signed app to Apple.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${HOP_BUILD_DIR:-$ROOT/build/distribution}"
IDENTITY="${HOP_MAC_DEVELOPER_ID:?Set HOP_MAC_DEVELOPER_ID to your Developer ID Application identity}"
PROFILE="${HOP_MAC_NOTARY_PROFILE:?Set HOP_MAC_NOTARY_PROFILE to a notarytool Keychain profile}"
case "$IDENTITY" in
  'Developer ID Application: '*) ;;
  *) printf '%s\n' 'A Developer ID Application identity is required for distribution.' >&2; exit 2 ;;
esac

HOP_BUILD_DIR="$OUT" HOP_ARCHS='arm64 x86_64' HOP_MAC_ADHOC=1 bash "$ROOT/scripts/build.sh"
APP="$OUT/Hop.app"
codesign --force --sign "$IDENTITY" --timestamp --options runtime "$APP"
codesign --verify --strict --all-architectures "$APP"
for arch in arm64 x86_64; do lipo -verify_arch "$arch" "$APP/Contents/MacOS/Hop"; done

SUBMISSION="$OUT/Hop-notary-submission.zip"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$SUBMISSION"
xcrun notarytool submit "$SUBMISSION" --keychain-profile "$PROFILE" --wait --output-format json > "$OUT/notary-result.json"
python3 - "$OUT/notary-result.json" <<'PY'
import json, pathlib, sys
with pathlib.Path(sys.argv[1]).open() as f:
    accepted = json.load(f).get("status") == "Accepted"
if not accepted:
    sys.exit("Notarization was not accepted; inspect the local notary result.")
PY
xcrun stapler staple "$APP"
xcrun stapler validate "$APP"
spctl --assess --type execute "$APP"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$OUT/Hop-macos-universal-notarized.zip"
printf 'Notarized archive: %s\n' "$OUT/Hop-macos-universal-notarized.zip"
