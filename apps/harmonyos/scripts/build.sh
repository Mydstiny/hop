#!/usr/bin/env bash
# Run from apps/harmonyos. Uses the installed tool's configured cache as-is.
# Uses only this project's user-configured signing material; never copies/creates credentials.
# No dependency installation, cache relocation or device action.
set -uo pipefail
if [[ ! -f build-profile.json5 || ! -f scripts/write_build_report.py ]]; then
  printf 'Run this script from apps/harmonyos.\n' >&2
  exit 2
fi
mkdir -p tests/.build
started=$(python3 -c 'import time; print(time.time())')
export DEVECO=/Applications/DevEco-Studio.app/Contents
export NODE_HOME="$DEVECO/tools/node"
export PATH="$NODE_HOME/bin:$DEVECO/tools/ohpm/bin:$PATH"
export DEVECO_SDK_HOME="$DEVECO/sdk"
export JAVA_HOME="$DEVECO/jbr/Contents/Home"
"$NODE_HOME/bin/node" "$DEVECO/tools/hvigor/bin/hvigorw.js" --mode module -p product=default -p module=entry@default -p buildMode=debug assembleHap --no-daemon --no-parallel --stacktrace 2>&1 | tee tests/.build/hap-build.log
rc=${PIPESTATUS[0]}
printf '\n[exit code: %s]\n' "$rc" | tee -a tests/.build/hap-build.log
python3 scripts/write_build_report.py "$rc" "$started"
report_rc=$?
if [[ "$rc" -eq 0 && "$report_rc" -ne 0 ]]; then rc=$report_rc; fi
exit "$rc"
