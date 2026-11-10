#!/usr/bin/env bash
# Build a fresh HAP with no signing material in a disposable project.
set -euo pipefail
APP="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$APP/../.." && pwd)"
DEVECO=/Applications/DevEco-Studio.app/Contents
MODE="${1:-full}"
OUT="${HOP_HARMONY_OUT:-$APP/build/distribution}"
case "$MODE" in
  full) NAME=Hop-harmonyos-unsigned.hap ;;
  input-only) NAME=Hop-harmonyos-input-only-unsigned.hap ;;
  *) printf 'Usage: %s [full|input-only]\n' "$0" >&2; exit 2 ;;
esac
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
STAGE="$(mktemp -d "${TMPDIR:-/tmp}/hop-unsigned.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
PROJECT="$STAGE/apps/harmonyos"
mkdir -p "$PROJECT"
rsync -a --exclude '/build-profile.json5' --exclude '/signing.local/' \
  --exclude 'build/' --exclude '.build/' --exclude '.hvigor/' --exclude '.idea/' \
  --exclude '.cxx/' --exclude 'oh_modules/' --exclude 'oh-package-lock.json5' \
  --exclude 'local.properties' --exclude '.DS_Store' --exclude '/artifacts/' "$APP/" "$PROJECT/"
for path in core tools protocol third_party LICENSE THIRD_PARTY_NOTICES.md; do
  ln -s "$REPO/$path" "$STAGE/$path"
done
"$DEVECO/tools/node/bin/node" - "$DEVECO/tools/hvigor/hvigor-ohos-plugin/node_modules/json5" "$APP" "$PROJECT" "$MODE" <<'JS'
const fs = require('fs');
const [parser, source, project, mode] = process.argv.slice(2);
try {
  const json5 = require(parser);
  const profile = json5.parse(fs.readFileSync(source + '/build-profile.json5', 'utf8'));
  profile.app.signingConfigs = [];
  for (const product of profile.app.products) delete product.signingConfig;
  fs.writeFileSync(project + '/build-profile.json5', JSON.stringify(profile, null, 2) + '\n');
  if (mode === 'input-only') {
    const main = project + '/entry/src/main/';
    const manifest = main + 'module.json5';
    const module = json5.parse(fs.readFileSync(manifest, 'utf8'));
    const declared = module.module.requestPermissions;
    const permissions = declared.filter(p => p.name !== 'ohos.permission.READ_PASTEBOARD');
    if (permissions.length !== declared.length - 1) throw new Error('Unexpected permission list');
    module.module.requestPermissions = permissions;
    fs.writeFileSync(manifest, JSON.stringify(module, null, 2) + '\n');
    const index = main + 'ets/pages/Index.ets';
    const page = fs.readFileSync(index, 'utf8');
    const flag = 'clipboardAvailable: boolean = true';
    if (page.split(flag).length !== 2) throw new Error('Unexpected clipboard flag');
    fs.writeFileSync(index, page.replace(flag, 'clipboardAvailable: boolean = false'));
  }
} catch (_) {
  console.error('Cannot configure the disposable unsigned Hop project.');
  process.exit(1);
}
JS
(cd "$PROJECT" && bash scripts/build.sh)
HAP="$PROJECT/entry/build/default/outputs/default/entry-default-unsigned.hap"
python3 - "$STAGE" "$HAP" "$MODE" <<'PY'
import json
import sys
import zipfile
from pathlib import Path

stage, hap = (Path(x).resolve() for x in sys.argv[1:3])
sys.path.insert(0, str(stage / 'tools'))
from device_preflight import audit_hap_identity, audit_signature, DEFAULT_DEVECO

identity = audit_hap_identity(hap)
if not (identity['bundleMatches'] and identity['minimumApiMatches']):
    sys.exit('Unexpected bundle or minimum API in the unsigned HAP.')
if audit_signature(stage, DEFAULT_DEVECO, hap)['status'] != 'unsigned':
    sys.exit('The SDK did not identify the HAP as unsigned.')
with zipfile.ZipFile(hap) as package:
    manifest = json.loads(package.read('module.json'))
    clipboard = any(p['name'] == 'ohos.permission.READ_PASTEBOARD' for p in manifest['module']['requestPermissions'])
if clipboard != (sys.argv[3] == 'full'):
    sys.exit('The unsigned HAP declares the wrong permissions for this variant.')
PY
PENDING="$(mktemp "$OUT/.hop-unsigned.XXXXXX")"
trap 'rm -rf "$STAGE"; rm -f "$PENDING"' EXIT
cp "$HAP" "$PENDING"
chmod 644 "$PENDING"
mv -f "$PENDING" "$OUT/$NAME"
printf 'Verified unsigned HAP: %s\n' "$OUT/$NAME"
