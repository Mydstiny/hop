#!/usr/bin/env bash
# Run from apps/harmonyos. Uses the installed tool's configured cache as-is.
# Uses only this project's user-configured signing material; never copies/creates credentials.
# Resolves declared local NAPI type packages; the pinned v2 TLS source may be fetched on a clean build. No device action.
set -uo pipefail
if [[ ! -f build-profile.json5 || ! -f scripts/write_build_report.py ]]; then
  printf 'Run this script from apps/harmonyos.\n' >&2
  exit 2
fi
if ! python3 ../../tools/generate_hid_keymap.py --check; then
  printf 'v2 HID keymap is stale; regenerate it before building.\n' >&2
  exit 2
fi
license_dir='entry/src/main/resources/rawfile/hop-licenses'
if ! (mkdir -p "$license_dir/third_party" &&
      cp ../../LICENSE ../../THIRD_PARTY_NOTICES.md "$license_dir/" &&
      cp ../../third_party/mbedtls-3.6.7-LICENSE.txt "$license_dir/third_party/"); then
  printf 'Cannot stage release license notices.\n' >&2
  exit 2
fi
mkdir -p tests/.build
started=$(python3 -c 'import time; print(time.time())')
export DEVECO=/Applications/DevEco-Studio.app/Contents
export NODE_HOME="$DEVECO/tools/node"
export PATH="$NODE_HOME/bin:$DEVECO/tools/ohpm/bin:$PATH"
export DEVECO_SDK_HOME="$DEVECO/sdk"
export JAVA_HOME="$DEVECO/jbr/Contents/Home"
# Both declared dependencies are local type packages. Fail instead of building
# with unresolved NAPI types, which ArkTS may otherwise infer as `any`.
if ! (cd entry && "$DEVECO/tools/ohpm/bin/ohpm" install); then
  printf 'Cannot resolve local NAPI type packages.\n' >&2
  exit 2
fi
"$NODE_HOME/bin/node" "$DEVECO/tools/hvigor/bin/hvigorw.js" --mode module -p product=default -p module=entry@default -p buildMode=debug assembleHap --no-daemon --no-parallel --stacktrace 2>&1 | tee tests/.build/hap-build.log
rc=${PIPESTATUS[0]}
printf '\n[exit code: %s]\n' "$rc" | tee -a tests/.build/hap-build.log
python3 scripts/write_build_report.py "$rc" "$started"
report_rc=$?
if [[ "$rc" -eq 0 && "$report_rc" -ne 0 ]]; then rc=$report_rc; fi
exit "$rc"
