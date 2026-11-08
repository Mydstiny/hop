#!/usr/bin/env python3
"""Reproducible native/offline verification. Does NOT launch apps or operate devices."""
from pathlib import Path
import datetime
import hashlib
import json
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def package_records():
    records = []
    for path, signing in (
        ('apps/macos/build/Hop-macos-universal-adhoc.zip', 'ad-hoc; not notarized'),
        ('apps/harmonyos/entry/build/default/outputs/default/entry-default-unsigned.hap', 'unsigned; requires app/device-specific debug signing'),
    ):
        file = ROOT / path
        data = file.read_bytes()
        records.append({'path': path, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'signing': signing})
    checksum = ROOT / 'artifacts/SHA256SUMS'
    checksum.parent.mkdir(exist_ok=True)
    checksum.write_text(''.join(f"{record['sha256']}  {record['path']}\n" for record in records))
    return records


def main():
    output = ROOT / 'critique'
    output.mkdir(exist_ok=True)
    commands = [
        ('tests', [sys.executable, 'tools/generate_keymap.py', '--check'], ROOT),
        ('tests', [sys.executable, 'tools/check_xcode_project.py'], ROOT),
        ('tests', [sys.executable, 'tools/test_xcode_project.py'], ROOT),
        ('tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tools/mock-peer', '-v'], ROOT),
        ('build_tests', ['bash', 'apps/macos/scripts/verify.sh'], ROOT),
        ('build_tests', ['bash', 'scripts/verify.sh'], ROOT / 'apps/harmonyos'),
        ('tests', [sys.executable, 'tools/test_native_interop.py'], ROOT),
        ('package', ['bash', 'apps/macos/scripts/package.sh'], ROOT),
    ]
    report = {'build': 'passing', 'tests': 'passing', 'commandsRun': [], 'failures': [],
              'timestamp': datetime.datetime.now().astimezone().isoformat(),
              'scope': 'Native CLT universal macOS build + DevEco unsigned HarmonyOS HAP + offline/loopback tests only',
              'xcodeProjectBuild': 'not verified: complete Xcode is not installed',
              'harmonySigning': 'unsigned: no app/device-specific debug signingConfig',
              'runtimeInputVerified': False, 'deviceInstalled': False}
    started = time.monotonic()
    with (output / 'build-test.log').open('w') as log:
        for kind, command, cwd in commands:
            description = ' '.join(command)
            print('+', description, flush=True)
            log.write(f'\n$ {description}\n(cwd: {cwd})\n')
            log.flush()
            begin = time.monotonic()
            try:
                result = subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, timeout=240)
                status, text = result.returncode, result.stdout
            except subprocess.TimeoutExpired as exc:
                status, text = 124, str(exc)
            log.write(text + f'\n[exit code: {status}]\n')
            log.flush()
            print(text, end='' if text.endswith('\n') else '\n', flush=True)
            entry = {'command': command, 'cwd': str(cwd.relative_to(ROOT)), 'exitCode': status,
                     'durationMs': round((time.monotonic() - begin) * 1000), 'kind': kind}
            report['commandsRun'].append(entry)
            if status:
                report['failures'].append(entry)
                if kind in ('build_tests', 'package'):
                    report['build'] = 'failing'
                if kind in ('tests', 'build_tests'):
                    report['tests'] = 'failing'
                # Do not package stale binaries or run interop on a failed build.
                break
    if report['build'] == 'passing' and sum(e['kind'] == 'build_tests' for e in report['commandsRun']) < 2:
        report['build'] = 'failing'
        report['buildReason'] = 'Unverified: a prior validation failed before both native build commands could run.'
    if not report['failures']:
        try:
            report['artifacts'] = package_records()
        except OSError as exc:
            report['build'] = 'failing'
            report['failures'].append({'kind': 'package', 'message': f'Artifact metadata failed: {exc}'})
    report['durationMs'] = round((time.monotonic() - started) * 1000)
    report['signals'] = {'build.passing': report['build'] == 'passing', 'tests.passing': report['tests'] == 'passing'}
    report['critique.score'] = 5 if all(report['signals'].values()) else (3 if report['signals']['build.passing'] else 1)
    (output / 'build-test.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return int(bool(report['failures']))


if __name__ == '__main__':
    raise SystemExit(main())
