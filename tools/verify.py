#!/usr/bin/env python3
"""Native/offline verification; optional offscreen AppKit harness, never product apps/devices."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def package_records():
    records = []
    # Build/package scripts already honor HOP_BUILD_DIR. Hash that exact archive,
    # not a stale default archive when the running product is being preserved.
    mac_output = Path(os.environ.get('HOP_BUILD_DIR') or str(ROOT / 'apps/macos/build'))
    if not mac_output.is_absolute():
        mac_output = ROOT / mac_output
    mac_archive = os.path.relpath(mac_output / 'Hop-macos-universal-adhoc.zip', ROOT)
    for path, signing in (
        (mac_archive, 'ad-hoc; not notarized'),
        ('apps/harmonyos/entry/build/default/outputs/default/entry-default-unsigned.hap', 'unsigned; requires app/device-specific debug signing'),
    ):
        file = ROOT / path
        data = file.read_bytes()
        records.append({'path': path, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'signing': signing})
    harmony_report = ROOT / 'apps/harmonyos/tests/.build/hap-build-report.json'
    if harmony_report.is_file():
        data = json.loads(harmony_report.read_text())
        if data.get('build.passing') and data.get('signature', {}).get('status') == 'verified_by_sdk_tool':
            signed = ROOT / 'apps/harmonyos' / data['artifact']['path']
            payload = signed.read_bytes()
            digest = hashlib.sha256(payload).hexdigest()
            if digest != data['artifact'].get('sha256'):
                raise ValueError('Signed HAP changed since SDK verification; refusing verified signing label')
            records.append({'path': str(signed.relative_to(ROOT)), 'bytes': len(payload),
                            'sha256': digest,
                            'signing': 'SDK-verified signed debug HAP; device eligibility not verified by this offline command'})
    checksum = ROOT / 'artifacts/SHA256SUMS'
    checksum.parent.mkdir(exist_ok=True)
    checksum.write_text(''.join(f"{record['sha256']}  {record['path']}\n" for record in records))
    return records


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--include-native-ui', action='store_true', help='Include offscreen production AppKit view tests; no visible window or product app launch')
    args = parser.parse_args(argv)
    output = ROOT / 'critique'
    output.mkdir(exist_ok=True)
    commands = [
        ('tests', [sys.executable, 'tools/generate_keymap.py', '--check'], ROOT),
        ('tests', [sys.executable, 'tools/check_xcode_project.py'], ROOT),
        ('tests', [sys.executable, 'tools/test_xcode_project.py'], ROOT),
        ('tests', [sys.executable, 'tools/test_resolve_xcode.py'], ROOT),
        ('tests', [sys.executable, 'tools/test_verify.py'], ROOT),
        ('tests', [sys.executable, 'tools/test_device_preflight.py'], ROOT),
        ('tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tools/mock-peer', '-v'], ROOT),
        ('build_tests', ['bash', 'apps/macos/scripts/verify.sh'], ROOT),
        ('build_tests', ['bash', 'scripts/verify.sh'], ROOT / 'apps/harmonyos'),
        ('tests', [sys.executable, 'tools/test_native_interop.py'], ROOT),
        ('package', ['bash', 'apps/macos/scripts/package.sh'], ROOT),
    ]
    if args.include_native_ui:
        commands.insert(-1, ('tests', ['bash', 'apps/macos/scripts/ui-test.sh'], ROOT))
    report = {'build': 'passing', 'tests': 'passing', 'commandsRun': [], 'failures': [],
              'timestamp': datetime.datetime.now().astimezone().isoformat(),
              'scope': 'Native CLT universal macOS build + DevEco HAP with per-artifact signing state + offline/loopback/optional offscreen tests; no device operation',
              'xcodeProjectBuild': 'not run by this offline verifier; use apps/macos/scripts/xcode-build.sh for actual project build',
              'harmonySigning': 'not inspected until the actual HarmonyOS build report is read',
              'runtimeInputVerified': False, 'deviceInstalled': False}
    native_env = os.environ.copy()
    if not native_env.get('DEVELOPER_DIR') and Path('/Library/Developer/CommandLineTools/usr/bin/clang').is_file():
        native_env['DEVELOPER_DIR'] = '/Library/Developer/CommandLineTools'
    report['nativeCompilerDeveloperDirectory'] = native_env.get('DEVELOPER_DIR', 'active system selection')
    started = time.monotonic()
    with (output / 'build-test.log').open('w') as log:
        for kind, command, cwd in commands:
            description = ' '.join(command)
            print('+', description, flush=True)
            log.write(f'\n$ {description}\n(cwd: {cwd})\n')
            log.flush()
            begin = time.monotonic()
            try:
                result = subprocess.run(command, cwd=cwd, env=native_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
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
    harmony_run = next((e for e in report['commandsRun'] if e['cwd'] == 'apps/harmonyos'), None)
    if harmony_run:
        try:
            data = json.loads((ROOT / 'apps/harmonyos/tests/.build/hap-build-report.json').read_text())
            report['harmonySigning'] = data.get('signing', 'unverified')
            report['harmonyMinimumApi'] = data.get('minimumApi')
        except (OSError, ValueError):
            report['harmonySigning'] = 'unverified: no current build report'
    ui = next((e for e in report['commandsRun'] if e['command'][-1] == 'apps/macos/scripts/ui-test.sh'), None)
    report['nativeUiTest'] = {'status': ('passing' if ui['exitCode'] == 0 else 'failing') if ui else 'skipped',
                              'scope': 'Offscreen production AppKit content with a memory model; no menu-bar interaction/input proof',
                              'reason': 'Explicit opt-in offscreen harness executed' if ui else
                                        'Earlier validation failed' if args.include_native_ui else
                                        'Not requested; add --include-native-ui on a Mac with a WindowServer session'}
    if ui and ui['exitCode'] == 0:
        try:
            ui_report = json.loads((ROOT / 'apps/macos/build/ui-test/report.json').read_text())
            if not ui_report['passing'] or ui_report['visibleWindowsCreated'] or ui_report['runtimeInputVerified']:
                raise ValueError('Offscreen UI report has inconsistent success/safety fields')
            report['nativeUiTest']['checks'] = ui_report['checks']
            report['nativeUiTest']['visibleWindowsCreated'] = ui_report['visibleWindowsCreated']
            report['nativeUiTest']['productAppLaunched'] = ui_report['productAppLaunched']
        except (OSError, ValueError, KeyError) as exc:
            report['tests'] = 'failing'
            report['nativeUiTest']['status'] = 'failing'
            report['failures'].append({'kind': 'tests', 'message': f'Native UI report missing or invalid: {exc}'})
    if report['build'] == 'passing' and sum(e['kind'] == 'build_tests' for e in report['commandsRun']) < 2:
        report['build'] = 'failing'
        report['buildReason'] = 'Unverified: a prior validation failed before both native build commands could run.'
    if not report['failures']:
        try:
            report['artifacts'] = package_records()
        except (OSError, ValueError) as exc:
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
