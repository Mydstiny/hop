#!/usr/bin/env bash
# Build an unsigned HAP for sideload. Callers sign it with their own debug
# certificate. This script never prints signing material. If build-profile.json5
# contains local paths or passwords, they are blanked only for this build and
# restored afterwards.
set -euo pipefail
cd "$(dirname "$0")/.."
PROFILE="build-profile.json5"
BACKUP="$(mktemp)"
cp "$PROFILE" "$BACKUP"
restore() { cp "$BACKUP" "$PROFILE"; rm -f "$BACKUP"; }
trap restore EXIT
python3 - "$PROFILE" << 'PY'
import pathlib, re, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
for key in ("storeFile", "storePassword", "keyAlias", "keyPassword", "profile", "certpath"):
    text = re.sub(rf'("{key}"\s*:\s*")[^"]*(")', r'\1\2', text)
path.write_text(text)
PY
# SignHap fails when material is blank. PackageHap has already written the unsigned file.
set +e
bash scripts/build.sh
set -e
OUT="entry/build/default/outputs/default"
HAP="$(find "$OUT" -maxdepth 1 -name '*unsigned*.hap' -print -quit)"
if [[ -z "$HAP" ]]; then
  echo "Unsigned HAP was not produced. Refusing to publish a signed package." >&2
  find "$OUT" -maxdepth 1 -name '*.hap' -print >&2
  exit 1
fi
echo "Unsigned HAP: $HAP"
