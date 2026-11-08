#!/usr/bin/env bash
# Offline C++ (fake input SDK + loopback TCP), then actual HAP and configured SDK verification.
# Run from apps/harmonyos; no dependency install or device operation.
set -uo pipefail
if [[ ! -f tests/run_native_tests.py || ! -f scripts/build.sh ]]; then
  printf 'Run this script from apps/harmonyos.\n' >&2
  exit 2
fi
mkdir -p tests/.build
python3 tests/run_native_tests.py 2>&1 | tee tests/.build/native-tests.log
test_rc=${PIPESTATUS[0]}
bash scripts/build.sh
build_rc=$?
python3 - "$test_rc" "$build_rc" <<'PY'
import json
from pathlib import Path
import sys
out=Path('tests/.build')
test_rc,build_rc=map(int,sys.argv[1:])
report={
 'build':'passing' if build_rc==0 else 'failing',
 'tests':'passing' if test_rc==0 else 'failing',
 'build.passing':build_rc==0,
 'tests.passing':test_rc==0,
 'nativeExitCode':test_rc,
 'hapExitCode':build_rc,
 'commandsRun':['python3 tests/run_native_tests.py','bash scripts/build.sh'],
 'deviceInputTested':False,
 'reason':'Native tests use recording fakes, never the system input service. Actual HAP/signature status is in hap-build-report.json; signing is not device acceptance.',
 'logs':['tests/.build/native-tests.log','tests/.build/hap-build.log'],
}
(out/'build-test.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
PY
if [[ "$test_rc" -ne 0 ]]; then exit "$test_rc"; fi
exit "$build_rc"
