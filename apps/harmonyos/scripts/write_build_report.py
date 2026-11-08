#!/usr/bin/env python3
"""Record actual build outcome and unsigned artifact hash; never infer a pass."""
from datetime import datetime
import hashlib
import json
from pathlib import Path
import sys
import time
import zipfile

root = Path(__file__).resolve().parents[1]
out = root / 'tests/.build'
out.mkdir(exist_ok=True)
rc = int(sys.argv[1])
report = {
    'build': 'passing' if rc == 0 else 'failing',
    'build.passing': rc == 0,
    'exitCode': rc,
    'timestamp': datetime.now().astimezone().isoformat(),
    'durationMs': round((time.time() - float(sys.argv[2])) * 1000),
    'command': 'node $DEVECO/tools/hvigor/bin/hvigorw.js --mode module -p product=default -p module=entry@default -p buildMode=debug assembleHap --no-daemon --no-parallel --stacktrace',
    'toolchain': '/Applications/DevEco-Studio.app/Contents',
    'minimumApi': 20,
    'compileApi': 26,
    'signing': 'unsigned; no signingConfig',
    'deviceInstalled': False,
    'deviceInputTested': False,
    'artifact': None,
    'log': 'tests/.build/hap-build.log',
}
if rc == 0:
    hap = root / 'entry/build/default/outputs/default/entry-default-unsigned.hap'
    if not hap.is_file() or not zipfile.is_zipfile(hap):
        report['build'] = 'failing'
        report['build.passing'] = False
        report['error'] = 'Tool exited zero but expected unsigned HAP was missing/invalid.'
    else:
        report['artifact'] = {
            'path': str(hap.relative_to(root)),
            'bytes': hap.stat().st_size,
            'sha256': hashlib.sha256(hap.read_bytes()).hexdigest(),
        }
(out / 'hap-build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
if not report['build.passing'] and rc == 0:
    sys.exit(1)
