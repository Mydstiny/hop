#!/usr/bin/env python3
"""Record actual artifact metadata; verify configured signed output using the SDK."""
from datetime import datetime
import hashlib
import json
from pathlib import Path
import sys
import time
import zipfile

root = Path(__file__).resolve().parents[1]
project = root.parents[1]
sys.path.insert(0, str(project / 'tools'))
from device_preflight import audit_configuration, audit_signature, DEFAULT_DEVECO
configuration = audit_configuration(project, DEFAULT_DEVECO)
configured = configuration['selectedConfigPresent']
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
    'minimumApi': None,
    'compileApi': 26,
    'signing': 'configured; actual output verification pending' if configured else 'unsigned; no selected signingConfig',
    'deviceInstalled': False,
    'deviceInputTested': False,
    'artifact': None,
    'log': 'tests/.build/hap-build.log',
}
if rc == 0:
    name = 'entry-default-signed.hap' if configured else 'entry-default-unsigned.hap'
    hap = root / 'entry/build/default/outputs/default' / name
    if not hap.is_file() or not zipfile.is_zipfile(hap):
        report['build'] = 'failing'
        report['build.passing'] = False
        report['error'] = f'Tool exited zero but expected {name} was missing/invalid.'
    else:
        with zipfile.ZipFile(hap) as archive:
            app = json.loads(archive.read('module.json'))['app']
        encoded_minimum = app.get('minAPIVersion')
        report['minimumApiEncoded'] = encoded_minimum
        report['minimumApi'] = encoded_minimum % 1000 if isinstance(encoded_minimum, int) else None
        report['bundleMatches'] = app.get('bundleName') == 'dev.hop.input'
        if report['minimumApi'] != 20 or not report['bundleMatches']:
            report['build'] = 'failing'
            report['build.passing'] = False
            report['error'] = 'Artifact must retain dev.hop.input and minimum API20; tool success alone is insufficient.'
        if configured:
            report['signature'] = audit_signature(project, DEFAULT_DEVECO, hap)
            if report['signature']['status'] == 'verified_by_sdk_tool':
                report['signing'] = 'signed; SDK verification passed; device eligibility untested'
            else:
                report['build'] = 'failing'
                report['build.passing'] = False
                report['signing'] = 'configured output failed SDK signature verification'
        report['artifact'] = {
            'path': str(hap.relative_to(root)),
            'bytes': hap.stat().st_size,
            'sha256': hashlib.sha256(hap.read_bytes()).hexdigest(),
        }
(out / 'hap-build-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
if not report['build.passing'] and rc == 0:
    sys.exit(1)
