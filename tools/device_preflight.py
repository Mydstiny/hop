#!/usr/bin/env python3
"""Read-only tool/device prerequisite audit; NEVER installs, launches, signs or injects.

Default exit 0 means report collected, NOT ready. --require-prerequisites exits 2
when automatic prerequisites are missing. An actual SDK API redline exits 3 and
stops the audit before any other tool/device calls. Device/profile consent and
physical acceptance remain manual even if automatic prerequisites pass.
"""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import re
from resolve_xcode import find_xcode
import subprocess
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DEVECO = Path('/Applications/DevEco-Studio.app/Contents')
PARAMS = {'model': 'const.product.model', 'type': 'const.product.devicetype',
          'api': 'const.ohos.apiversion', 'os': 'const.ohos.fullname'}


def run(command, *, cwd=None, timeout=12):
    """Only fixed, argument-vector, non-shell read/verify commands call this helper."""
    try:
        result = subprocess.run([str(x) for x in command], cwd=cwd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=timeout)
        return {'exitCode': result.returncode, 'output': result.stdout}
    except subprocess.TimeoutExpired:
        return {'exitCode': 124, 'output': 'Read-only command timed out'}
    except OSError:
        return {'exitCode': 127, 'output': 'Read-only tool unavailable'}


def audit_sdk(deveco):
    header = deveco / 'sdk/default/openharmony/native/sysroot/usr/include/multimodalinput/oh_input_manager.h'
    result = {'header': str(header), 'headerReadable': False, 'apiDeclarationsPass': False, 'api20AvailabilityVerified': True, 'missingOrMismatched': [],
              'arm64InputLibraryPresent': (deveco / 'sdk/default/openharmony/native/sysroot/usr/lib/aarch64-linux-ohos/libohinput.so').is_file(),
              'desktopKitPresent': (deveco / 'sdk/default/hms/ets/kits/@kit.DeskTopExtensionKit.d.ts').is_file()}
    try:
        raw = header.read_bytes()
        source = re.sub(r'/\*.*?\*/|//[^\n]*', '', raw.decode('utf-8'), flags=re.S)
    except (OSError, UnicodeError):
        result['api20AvailabilityVerified'] = False
        result['missingOrMismatched'] = ['real SDK input header unavailable']
        return result
    result['headerReadable'] = True
    result['headerSha256'] = hashlib.sha256(raw).hexdigest()
    specs = {
        'OH_Input_RequestInjection': r'Input_Result\s+OH_Input_RequestInjection\s*\(\s*Input_InjectAuthorizeCallback\s+\w+\s*\)',
        'OH_Input_InjectMouseEventGlobal': r'int32_t\s+OH_Input_InjectMouseEventGlobal\s*\(\s*const\s+struct\s+Input_MouseEvent\s*\*\s*\w+\s*\)',
        'OH_Input_InjectKeyEvent': r'int32_t\s+OH_Input_InjectKeyEvent\s*\(\s*const\s+struct\s+Input_KeyEvent\s*\*\s*\w+\s*\)',
    }
    for name, pattern in specs.items():
        declaration = re.search(r'\b' + pattern + r'\s*([^;]*);', source)
        availability = re.search(r'introduced\s*=\s*(\d+)\.', declaration[1]) if declaration else None
        if not declaration:
            result['missingOrMismatched'].append(name)
        if not availability or int(availability[1]) > 20:
            result['api20AvailabilityVerified'] = False
    callback = r'\btypedef\s+void\s*\(\s*\*\s*Input_InjectAuthorizeCallback\s*\)\s*\(\s*Input_InjectionStatus\s+\w+\s*\)\s*;'
    if not re.search(callback, source):
        result['missingOrMismatched'].append('Input_InjectAuthorizeCallback')
    for name, value in (('UNAUTHORIZED', 0), ('AUTHORIZING', 1), ('AUTHORIZED', 2)):
        if not re.search(r'\b' + name + r'\s*=\s*' + str(value) + r'\b', source):
            result['missingOrMismatched'].append(name)
    result['apiDeclarationsPass'] = not result['missingOrMismatched']
    return result


def audit_configuration(root, deveco):
    # Use the installed vendor parser for real JSON5, not an invented subset.
    # Only reduced fields leave Node; passwords, certificate/profile contents and
    # signing-material paths are never printed, extracted, copied or reported.
    script = """
try {
 const fs=require('fs'); const JSON5=require(process.argv[1]);
 const app=JSON5.parse(fs.readFileSync(process.argv[2],'utf8')).app || {};
 const list=Array.isArray(app.signingConfigs) ? app.signingConfigs : [];
 const product=(app.products || []).find(x=>x.name==='default') || {};
 const configured=typeof product.signingConfig==='string' && list.some(x=>x.name===product.signingConfig);
 console.log(JSON.stringify({signingConfigCount:list.length,selectedConfigPresent:configured,
   compatibleSdkVersion:product.compatibleSdkVersion,targetSdkVersion:product.targetSdkVersion}));
} catch (_) { console.error('Project JSON5 could not be parsed'); process.exit(1); }
"""
    result = run([deveco / 'tools/node/bin/node', '-e', script,
                  deveco / 'tools/hvigor/hvigor-ohos-plugin/node_modules/json5',
                  root / 'apps/harmonyos/build-profile.json5'])
    try:
        data = json.loads(result['output']) if result['exitCode'] == 0 else {}
        return {'parsed': bool(data), 'signingConfigCount': data.get('signingConfigCount', 0),
                'selectedConfigPresent': data.get('selectedConfigPresent') is True,
                'compatibleSdkVersion': data.get('compatibleSdkVersion'),
                'targetSdkVersion': data.get('targetSdkVersion')}
    except (ValueError, TypeError, AttributeError):
        return {'parsed': False, 'selectedConfigPresent': False}


def audit_hap_identity(hap):
    result = {'exists': hap.is_file(), 'bundleMatches': False, 'minimumApiMatches': False}
    try:
        data = hap.read_bytes()
        result.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        with zipfile.ZipFile(hap) as archive:
            info = archive.getinfo('module.json')
            if info.file_size > 1024 * 1024:
                return result
            app = json.loads(archive.read(info))['app']
        result['bundleMatches'] = app.get('bundleName') == 'dev.hop.input'
        result['minimumApiMatches'] = app.get('minAPIVersion') in (20, 60000020)
        # Do not dump arbitrary app metadata or names from a user-supplied HAP.
    except (OSError, KeyError, ValueError, TypeError, AttributeError, zipfile.BadZipFile):
        pass
    return result


def classify_verification(exit_code, output):
    if exit_code == 0:
        return 'verified_by_sdk_tool'
    if 'Verify Hap failed, signature not found.' in output or 'No Hap Signing Block before ZIP Central Directory' in output:
        return 'unsigned'
    return 'unverified'  # a verifier/tool error is NOT proof that a signature is absent


def audit_signature(root, deveco, hap):
    # The official verifier requires certificate/profile OUTPUT paths. They stay
    # under ignored private artifacts; no profile/UDID contents are read or printed.
    work = (root / 'artifacts/private/preflight' / uuid.uuid4().hex).resolve()
    work.mkdir(parents=True, exist_ok=False)
    command = [deveco / 'jbr/Contents/Home/bin/java', '-jar',
               deveco / 'sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar',
               'verify-app', '-inFile', hap, '-outCertChain', work / 'verified-chain.cer',
               '-outProfile', work / 'verified-profile.p7b']
    result = run(command, cwd=work, timeout=30)
    (work / 'verification.log').write_text(result['output'])
    status = classify_verification(result['exitCode'], result['output'])
    return {'status': status, 'exitCode': result['exitCode'],
            'diagnostic': 'No Hap Signing Block before ZIP Central Directory' if status == 'unsigned' else
                          'SDK verifier succeeded; device/profile eligibility still manual' if status == 'verified_by_sdk_tool' else
                          'Verification did not establish a valid signature; see local private log',
            'privateLog': str((work / 'verification.log').relative_to(root))}


def select_target(output, requested=None):
    targets = list(dict.fromkeys(line.strip() for line in output.splitlines()
                                if line.strip() and not line.strip().startswith('[')))
    if requested:
        return (requested if requested in targets else None), len(targets)
    return (targets[0] if len(targets) == 1 else None), len(targets)


def audit_device(deveco, requested=None, skip=False):
    if skip:
        return {'queried': False, 'eligiblePc': False, 'reason': 'Explicit --skip-device; no readiness claim'}
    hdc = deveco / 'sdk/default/openharmony/toolchains/hdc'
    listed = run([hdc, 'list', 'targets'])
    target, count = select_target(listed['output'], requested) if listed['exitCode'] == 0 else (None, 0)
    result = {'queried': True, 'targetCount': count, 'eligiblePc': False, 'selectedTarget': target}
    if not target:
        result['reason'] = 'No unique authorized target; specify --target when multiple devices are connected'
        return result
    facts = {}
    for name, param in PARAMS.items():
        response = run([hdc, '-t', target, 'shell', 'param', 'get', param])
        facts[name] = response['output'].strip() if response['exitCode'] == 0 else ''
    result['facts'] = facts
    try:
        api = int(facts['api'])
    except ValueError:
        api = 0
    result['eligiblePc'] = facts['type'] == '2in1' and api >= 20
    return result


def audit(root, deveco, hap, target=None, skip_device=False):
    report = {'timestamp': datetime.datetime.now().astimezone().isoformat(),
              'scope': 'Read-only source/tool/device prerequisite audit; report/private verifier outputs only',
              'deviceInstalled': False, 'runtimeInputVerified': False, 'redlineHit': False,
              'automatedPrerequisitesSatisfied': False, 'blockingConditions': [],
              'manualStillRequired': ['valid app/device debug-profile eligibility and legal system grants',
                                     'real Xcode project build', 'all unchecked two-device physical/GUI/background/fault acceptance']}
    report['sdk'] = audit_sdk(deveco)
    if not report['sdk']['headerReadable']:
        report['blockingConditions'] = ['harmony_sdk_header_unavailable_not_an_api_absence_proof']
        return report  # an unavailable/wrong toolchain path is NOT an API-absence conclusion
    if not report['sdk']['apiDeclarationsPass']:
        report['redlineHit'] = True
        report['blockingConditions'] = ['sdk_input_api_redline']
        return report  # no signing tool, Xcode, hdc, app launch, or device call after a real redline
    selected = run(['xcode-select', '-p'])
    active = selected['output'].strip() if selected['exitCode'] == 0 else None
    discovery = find_xcode(os.environ.get('DEVELOPER_DIR'), active)
    developer = discovery['developerDirectory']
    version_command = ['env', f'DEVELOPER_DIR={developer}', 'xcodebuild', '-version'] if developer else ['xcodebuild', '-version']
    xcode = run(version_command)
    setup = run(['env', f'DEVELOPER_DIR={developer}', 'xcodebuild', '-checkFirstLaunchStatus']) if developer and xcode['exitCode'] == 0 else None
    report['mac'] = {'activeDeveloperDirectory': active, 'resolvedDeveloperDirectory': developer,
                     'discovery': discovery, 'globalSelectionChanged': False,
                     'xcodebuildVersionExitCode': xcode['exitCode'],
                     'xcodeFirstLaunchExitCode': setup['exitCode'] if setup else None,
                     'completeXcodeAvailable': developer is not None and xcode['exitCode'] == 0 and setup is not None and setup['exitCode'] == 0,
                     'projectBuildExecuted': False}
    report['configuration'] = audit_configuration(root, deveco)
    report['hap'] = audit_hap_identity(hap)
    report['hap']['signature'] = audit_signature(root, deveco, hap) if report['hap']['exists'] else {'status': 'missing'}
    report['device'] = audit_device(deveco, target, skip_device)
    conditions = {
        'complete_xcode_unavailable': report['mac']['completeXcodeAvailable'],
        'harmony_signing_configuration_absent_or_unparsed': report['configuration']['selectedConfigPresent'],
        'harmony_sdk_api20_kit_or_input_library_not_verified': report['sdk']['api20AvailabilityVerified'] and report['sdk']['desktopKitPresent'] and report['sdk']['arm64InputLibraryPresent'],
        'hap_identity_or_minimum_api_not_verified': report['hap']['bundleMatches'] and report['hap']['minimumApiMatches'],
        'hap_signature_not_verified': report['hap']['signature']['status'] == 'verified_by_sdk_tool',
        'authorized_api20_pc_not_verified': report['device']['eligiblePc'],
    }
    report['blockingConditions'] = [name for name, passes in conditions.items() if not passes]
    report['automatedPrerequisitesSatisfied'] = not report['blockingConditions']
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--deveco', type=Path, default=DEFAULT_DEVECO)
    parser.add_argument('--hap', type=Path, help='HAP to verify; never renamed or installed')
    parser.add_argument('--target', help='Exact already-authorized hdc target; no automatic pairing')
    parser.add_argument('--skip-device', action='store_true')
    parser.add_argument('--require-prerequisites', action='store_true')
    args = parser.parse_args(argv)
    root, deveco = args.root.resolve(), args.deveco.resolve()
    hap = (args.hap or root / 'apps/harmonyos/entry/build/default/outputs/default/entry-default-unsigned.hap').resolve()
    report = audit(root, deveco, hap, args.target, args.skip_device)
    output = root / 'critique/device-preflight.json'
    output.parent.mkdir(exist_ok=True)
    output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps(report, indent=2, ensure_ascii=False))
    if report['redlineHit']:
        return 3
    return 2 if args.require_prerequisites and not report['automatedPrerequisitesSatisfied'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
