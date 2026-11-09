#!/bin/bash
# Sign with one local certificate whose designated requirement has no cdhash.
# Ad-hoc (--sign -) changes the cdhash on every rebuild and macOS then forgets
# Accessibility and Input Monitoring. This identity stays valid across rebuilds.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${1:?usage: sign.sh Hop.app}"
SIGNING="$ROOT/signing"
mkdir -p "$SIGNING"
KEY="$SIGNING/hop.key"
CRT="$SIGNING/hop.crt"
P12="$SIGNING/hop.p12"
KC="$SIGNING/hop.keychain-db"
PASS="hop"
CN="Hop Local Codesign"
if [[ ! -f "$KEY" || ! -f "$CRT" ]]; then
  openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
    -keyout "$KEY" -out "$CRT" -config "$ROOT/scripts/codesign.cnf"
fi
if [[ ! -f "$P12" ]]; then
  # OpenSSL 3+ MAC is rejected by the Security framework. SHA1/3DES is the format it imports.
  openssl pkcs12 -export -legacy -macalg SHA1 -inkey "$KEY" -in "$CRT" -out "$P12" -passout "pass:$PASS"
fi
if [[ ! -f "$KC" ]]; then
  saved="$(mktemp)"
  security list-keychains -d user | sed -e 's/^[[:space:]]*//' -e 's/"//g' > "$saved"
  security create-keychain -p "$PASS" "$KC"
  # create-keychain inserts itself into the user search list. Restore the previous list exactly.
  security list-keychains -d user -s $(cat "$saved")
  rm -f "$saved"
  security set-keychain-settings -lut 21600 "$KC"
  security unlock-keychain -p "$PASS" "$KC"
  security import "$P12" -k "$KC" -P "$PASS" -T /usr/bin/codesign -T /usr/bin/security
  security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$PASS" "$KC" >/dev/null
fi
security unlock-keychain -p "$PASS" "$KC"
codesign --force --sign "$CN" --keychain "$KC" \
  --identifier "local.hop.macos" \
  -r="designated => identifier \"local.hop.macos\" and certificate leaf[subject.CN] = \"$CN\"" \
  "$APP"
