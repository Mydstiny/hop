#!/usr/bin/env python3
"""Build/test read-only Hop metadata probe; optionally collect scoped PC readiness.
Never builds/restarts a product, opens a product TCP connection, reads pairing data,
requests/grants permissions, clicks controls, captures or injects input.
"""
from pathlib import Path
import argparse
import datetime
import ipaddress
import json
import os
import plistlib
import re
import subprocess
import time
from device_preflight import select_target, PARAMS, DEFAULT_DEVECO

ROOT = Path(__file__).resolve().parents[1]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', help='Exact previously authorized PC hdc target; never guessed')
    args = parser.parse_args(argv)
    out = ROOT / 'artifacts/test-build/runtime-probe'
    out.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if not env.get('DEVELOPER_DIR') and Path('/Library/Developer/CommandLineTools/usr/bin/clang').is_file():
        env['DEVELOPER_DIR'] = '/Library/Developer/CommandLineTools'
    records = []

    def run(command, timeout=30):
        command = [str(p) for p in command]
        try:
            result = subprocess.run(command, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            result = subprocess.CompletedProcess(command, 124, 'Read-only diagnostic command timed out')
        except OSError as error:
            result = subprocess.CompletedProcess(command, 127, str(error))
        records.append({'command': [str(p) for p in command], 'exitCode': result.returncode, 'output': result.stdout})
        return result

    started = time.monotonic()
    report = {'timestamp': datetime.datetime.now().astimezone().isoformat(),
              'scope': 'Isolated read-only probe build/tests + own app/kernel metadata; NOT product build or physical acceptance',
              'build': 'failing', 'tests': 'failing', 'commandsRun': records, 'failures': [],
              'compilerDeveloperDirectory': env.get('DEVELOPER_DIR', 'active selection'),
              'runtimeInputVerified': False, 'consentActionAttempted': False,
              'pairingDataRead': False, 'productRestarted': False,
              'unverified': ['input permissions', 'mutual pairing confirmation', 'notification/continuous task',
                             'real popup/window appearance', 'physical mouse/keyboard input and recovery']}
    binary = out / 'mac_runtime_probe'
    compile_result = run(['xcrun', 'swiftc', '-swift-version', '5', str(ROOT / 'tools/mac_runtime_probe.swift'),
                          '-framework', 'AppKit', '-o', str(binary)], timeout=120)
    tests = []
    try:
        assert compile_result.returncode == 0, 'Probe compile failed'
        report['build'] = 'passing'
        missing_args = run([binary])
        assert missing_args.returncode == 64
        tests.append('missing argument rejected')
        for name, identifier, expected in [('Foreign.app', 'example.not-hop', 3), ('AbsentHop.app', 'local.hop.macos', 2)]:
            contents = out / name / 'Contents'
            (contents / 'MacOS').mkdir(parents=True, exist_ok=True)
            with (contents / 'Info.plist').open('wb') as stream:
                plistlib.dump({'CFBundleIdentifier': identifier, 'CFBundleName': 'ProbeFixture',
                               'CFBundleExecutable': 'Hop', 'CFBundlePackageType': 'APPL'}, stream)
            (contents / 'MacOS/Hop').touch()
            result = run([binary, contents.parent])
            assert result.returncode == expected, f'{name}: expected {expected}, got {result.returncode}'
            if expected == 2:
                fixture = json.loads(result.stdout)
                assert not fixture['running'] and fixture['processes'] == []
            tests.append('foreign bundle rejected' if expected == 3 else 'same ID at different path cannot impersonate live app')
        result = run([binary, ROOT / 'apps/macos/build/Hop.app'])
        assert result.returncode in (0, 2)
        live = json.loads(result.stdout)
        for key in ['productLaunchedByProbe', 'permissionQueryAttempted', 'permissionGrantAttempted',
                    'pairConfirmationAttempted', 'captureEnableAttempted', 'mainWindowAbsenceVerified', 'runtimeInputVerified']:
            assert live[key] is False
        report['mac'] = live
        tests.append('live expected-path metadata + explicit limited safety/proof fields')
        report['tests'] = 'passing'
    except (AssertionError, ValueError) as error:
        report['failures'].append(str(error))

    sockets = []
    for process in report.get('mac', {}).get('processes', []):
        result = run(['/usr/sbin/lsof', '-nP', '-a', '-p', str(process['pid']), '-iTCP'])
        sockets.append({'pid': process['pid'], 'exitCode': result.returncode, 'output': result.stdout,
                        'listener24890Observed': bool(re.search(r':24890\s+\(LISTEN\)', result.stdout)),
                        'established24890Count': len(re.findall(r':24890->[^\n]+\(ESTABLISHED\)', result.stdout))})
    report['ownedMacSockets'] = sockets
    if args.target:
        hdc = DEFAULT_DEVECO / 'sdk/default/openharmony/toolchains/hdc'
        listed = run([hdc, 'list', 'targets'])
        target, count = select_target(listed.stdout, args.target) if listed.returncode == 0 else (None, 0)
        report['device'] = {'queried': True, 'selectedTarget': target, 'targetCount': count, 'eligiblePc': False}
        if target:
            facts = {}
            for name, param in PARAMS.items():
                result = run([hdc, '-t', target, 'shell', 'param', 'get', param])
                facts[name] = result.stdout.strip() if result.returncode == 0 else ''
            report['device']['facts'] = facts
            report['device']['eligiblePc'] = facts.get('type') == '2in1' and facts.get('api', '').isdigit() and int(facts['api']) >= 20
        if report['device']['eligiblePc']:
            process = run([hdc, '-t', target, 'shell', 'pidof', 'dev.hop.input'])
            pids = process.stdout.strip().split() if process.returncode == 0 else []
            report['device']['receiverPids'] = [int(pid) for pid in pids] if pids and all(pid.isdigit() for pid in pids) else []
            # Public routing metadata only; do not make a probe TCP connection to the single-peer product listener.
            try:
                address = str(ipaddress.ip_address(args.target.rsplit(':', 1)[0]))
                route = run(['/sbin/route', '-n', 'get', address])
                match = re.search(r'^\s*interface:\s*(\S+)\s*$', route.stdout, re.MULTILINE)
                if route.returncode == 0 and match:
                    interface = match.group(1)
                    local = run(['/usr/sbin/ipconfig', 'getifaddr', interface])
                    report['manualIpInterface'] = interface
                    report['manualMacIPv4'] = str(ipaddress.IPv4Address(local.stdout.strip())) if local.returncode == 0 else None
            except ValueError:
                report['manualIpReason'] = 'No IPv4 route result; use the correct trusted-LAN address manually'
    else:
        report['device'] = {'queried': False, 'reason': 'No explicit target; no device was queried'}
    report['startupBasicsVerified'] = bool(report.get('mac', {}).get('accessoryPolicyVerified') and
        any(item['listener24890Observed'] for item in sockets) and report['device'].get('receiverPids'))
    report['testsRun'] = tests
    report['durationMs'] = round((time.monotonic() - started) * 1000)
    report['signals'] = {'build.passing': report['build'] == 'passing', 'tests.passing': report['tests'] == 'passing'}
    report['critique.score'] = 5 if all(report['signals'].values()) else 3 if report['signals']['build.passing'] else 1
    (ROOT / 'critique/runtime-readiness.json').write_text(json.dumps(report, indent=2) + '\n')
    (ROOT / 'critique/runtime-readiness.log').write_text('\n\n'.join(json.dumps(row, indent=2) for row in records))
    print(json.dumps(report, indent=2))
    return 0 if all(report['signals'].values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
