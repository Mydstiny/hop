#!/usr/bin/env python3
"""Portable unit tests; no actual SDK command, signing, device or input operations."""
from pathlib import Path
import contextlib
import io
import json
import runpy
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

PROBE = runpy.run_path(str(Path(__file__).with_name('device_preflight.py')))
G = PROBE['audit'].__globals__
HEADER = '''
typedef enum { UNAUTHORIZED = 0, AUTHORIZING = 1, AUTHORIZED = 2 } Input_InjectionStatus;
typedef void (*Input_InjectAuthorizeCallback)(Input_InjectionStatus authorizedStatus);
Input_Result OH_Input_RequestInjection(Input_InjectAuthorizeCallback callback)
__attribute__((__availability__(ohos, introduced=20.0.0)));
int32_t OH_Input_InjectMouseEventGlobal(const struct Input_MouseEvent* mouseEvent)
__attribute__((__availability__(ohos, introduced=20.0.0)));
int32_t OH_Input_InjectKeyEvent(const struct Input_KeyEvent* keyEvent)
__attribute__((__availability__(ohos, introduced=12.0.0)));
'''


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='hop-preflight-test-')
        self.root = Path(self.temporary.name).resolve()
        self.addCleanup(self.temporary.cleanup)
        self.deveco = self.root / 'deveco'
        self.header = self.deveco / 'sdk/default/openharmony/native/sysroot/usr/include/multimodalinput/oh_input_manager.h'
        self.header.parent.mkdir(parents=True)
        self.header.write_text(HEADER)
        self.hap = self.root / 'test.hap'

    def sdk(self, text=None):
        if text is not None:
            self.header.write_text(text)
        return PROBE['audit_sdk'](self.deveco)

    def hap_metadata(self, bundle='dev.hop.input', minimum=60000020):
        with zipfile.ZipFile(self.hap, 'w') as archive:
            archive.writestr('module.json', json.dumps({'app': {'bundleName': bundle, 'minAPIVersion': minimum}}))
        return PROBE['audit_hap_identity'](self.hap)

    def assembled(self, xcode_exit=0, configured=True, signature='verified_by_sdk_tool', eligible=True):
        sdk = self.sdk()
        sdk.update(desktopKitPresent=True, arm64InputLibraryPresent=True)
        replacements = {
            'audit_sdk': Mock(return_value=sdk),
            'find_xcode': Mock(return_value={'developerDirectory': '/fixture/Xcode.app/Contents/Developer', 'source': 'memory fixture'}),
            'run': Mock(return_value={'exitCode': xcode_exit, 'output': 'tool metadata'}),
            'audit_configuration': Mock(return_value={'selectedConfigPresent': configured}),
            'audit_hap_identity': Mock(return_value={'exists': True, 'bundleMatches': True, 'minimumApiMatches': True}),
            'audit_signature': Mock(return_value={'status': signature}),
            'audit_device': Mock(return_value={'eligiblePc': eligible}),
        }
        with patch.dict(G, replacements):
            return PROBE['audit'](self.root, self.deveco, self.hap)

    def test_exact_callback_and_sdk_annotated_signatures(self):
        result = self.sdk()
        self.assertTrue(result['apiDeclarationsPass'])
        self.assertTrue(result['api20AvailabilityVerified'])
        self.assertEqual(len(result['headerSha256']), 64)
        self.assertFalse(result['arm64InputLibraryPresent']) # no false ABI/link proof

    def test_comment_only_prototypes_do_not_pass(self):
        self.assertFalse(self.sdk('/*' + HEADER + '*/')['apiDeclarationsPass'])

    def test_wrong_request_return_type_is_redline(self):
        result = self.sdk(HEADER.replace('Input_Result OH_Input_RequestInjection', 'bool OH_Input_RequestInjection'))
        self.assertIn('OH_Input_RequestInjection', result['missingOrMismatched'])

    def test_wrong_request_arguments_are_redline(self):
        self.assertFalse(self.sdk(HEADER.replace('Input_InjectAuthorizeCallback callback', 'void'))['apiDeclarationsPass'])

    def test_wrong_callback_return_type_is_redline(self):
        self.assertFalse(self.sdk(HEADER.replace('typedef void (*', 'typedef int (*'))['apiDeclarationsPass'])

    def test_wrong_authorized_enum_is_redline(self):
        self.assertFalse(self.sdk(HEADER.replace('AUTHORIZED = 2', 'AUTHORIZED = 7'))['apiDeclarationsPass'])

    def test_unavailable_header_is_unknown_not_api_absence_proof(self):
        result = PROBE['audit_sdk'](self.root / 'absent-sdk')
        self.assertFalse(result['headerReadable'])
        self.assertFalse(result['apiDeclarationsPass'])
        self.assertFalse(result['api20AvailabilityVerified'])
        tool = Mock(side_effect=AssertionError('No external tool call for wrong SDK path'))
        with patch.dict(G, {'run': tool}):
            report = PROBE['audit'](self.root, self.root / 'absent-sdk', self.hap)
        self.assertFalse(report['redlineHit'])
        self.assertEqual(report['blockingConditions'], ['harmony_sdk_header_unavailable_not_an_api_absence_proof'])
        tool.assert_not_called()

    def test_newer_availability_is_not_signature_absence(self):
        result = self.sdk(HEADER.replace('introduced=20.0.0', 'introduced=21.0.0'))
        self.assertTrue(result['apiDeclarationsPass'])
        self.assertFalse(result['api20AvailabilityVerified'])

    def test_missing_annotation_cannot_prove_api20_but_is_not_redline(self):
        result = self.sdk(HEADER.replace('__attribute__((__availability__(ohos, introduced=20.0.0)))', ''))
        self.assertTrue(result['apiDeclarationsPass'])
        self.assertFalse(result['api20AvailabilityVerified'])

    def test_redline_stops_before_tools_or_device(self):
        sdk = self.sdk(HEADER.replace('OH_Input_InjectKeyEvent', 'OH_Input_Unrelated'))
        tool = Mock(side_effect=AssertionError('No tool allowed after redline'))
        with patch.dict(G, {'audit_sdk': Mock(return_value=sdk), 'run': tool}):
            result = PROBE['audit'](self.root, self.deveco, self.hap)
        self.assertTrue(result['redlineHit'])
        self.assertEqual(result['blockingConditions'], ['sdk_input_api_redline'])
        tool.assert_not_called()

    def test_configuration_report_does_not_emit_material_or_password(self):
        values = {'signingConfigCount': 1, 'selectedConfigPresent': True, 'compatibleSdkVersion': '6.0.0(20)',
                  'targetSdkVersion': '26.0.0', 'password': 'TEST-SECRET', 'profile': 'PRIVATE-PROFILE'}
        runner = Mock(return_value={'exitCode': 0, 'output': json.dumps(values)})
        with patch.dict(G, {'run': runner}):
            result = PROBE['audit_configuration'](self.root, self.deveco)
        self.assertTrue(result['selectedConfigPresent'])
        self.assertNotIn('TEST-SECRET', json.dumps(result))
        self.assertNotIn('PRIVATE-PROFILE', json.dumps(result))
        self.assertIn('node_modules/json5', str(runner.call_args.args[0][-2]))

    def test_configuration_tool_failure_is_not_configured(self):
        with patch.dict(G, {'run': Mock(return_value={'exitCode': 127, 'output': 'unavailable'})}):
            result = PROBE['audit_configuration'](self.root, self.deveco)
        self.assertFalse(result['parsed'])
        self.assertFalse(result['selectedConfigPresent'])

    def test_hap_identity_and_encoded_minimum(self):
        result = self.hap_metadata()
        self.assertTrue(result['bundleMatches'])
        self.assertTrue(result['minimumApiMatches'])
        self.assertEqual(result['bytes'], self.hap.stat().st_size)
        self.assertEqual(len(result['sha256']), 64)

    def test_other_application_hap_rejected(self):
        self.assertFalse(self.hap_metadata(bundle='another.application')['bundleMatches'])

    def test_raised_minimum_rejected(self):
        self.assertFalse(self.hap_metadata(minimum=60000026)['minimumApiMatches'])

    def test_invalid_hap_does_not_certify_identity(self):
        self.hap.write_bytes(b'not a HAP')
        result = PROBE['audit_hap_identity'](self.hap)
        self.assertFalse(result['bundleMatches'])
        self.assertFalse(result['minimumApiMatches'])

    def test_unsigned_requires_specific_official_diagnostic(self):
        classify = PROBE['classify_verification']
        self.assertEqual(classify(1, 'No Hap Signing Block before ZIP Central Directory'), 'unsigned')
        self.assertEqual(classify(1, 'Verify Hap failed, signature not found.'), 'unsigned')
        self.assertEqual(classify(1, 'certificate signature not found for unrelated reason'), 'unverified')
        self.assertEqual(classify(127, 'Java not found'), 'unverified')
        self.assertEqual(classify(0, 'verify-app success'), 'verified_by_sdk_tool')

    def test_signature_outputs_are_private_and_not_treated_as_input_credentials(self):
        commands = []
        def verify(command, **kwargs):
            commands.append(command)
            self.assertIn('verify-app', command)
            self.assertNotIn('-outproof', command) # not an advertised SDK flag
            cert = Path(command[command.index('-outCertChain') + 1])
            profile = Path(command[command.index('-outProfile') + 1])
            self.assertTrue(cert.is_relative_to(self.root / 'artifacts/private/preflight'))
            self.assertEqual(cert.parent, profile.parent)
            self.assertEqual(Path(kwargs['cwd']), cert.parent)
            profile.write_text('TEST-PRIVATE-PROFILE-NOT-FOR-REPORT')
            return {'exitCode': 0, 'output': 'verify-app success'}
        with patch.dict(G, {'run': verify}):
            report = PROBE['audit_signature'](self.root, self.deveco, self.hap)
        self.assertEqual(report['status'], 'verified_by_sdk_tool')
        self.assertNotIn('TEST-PRIVATE-PROFILE-NOT-FOR-REPORT', json.dumps(report))
        self.assertEqual(len(commands), 1)

    def test_signed_filename_cannot_override_missing_signature(self):
        with patch.dict(G, {'run': Mock(return_value={'exitCode': 1, 'output': 'No Hap Signing Block before ZIP Central Directory'})}):
            report = PROBE['audit_signature'](self.root, self.deveco, self.root / 'renamed-signed.hap')
        self.assertEqual(report['status'], 'unsigned')
        self.assertEqual(report['exitCode'], 1)

    def test_unique_target_and_bracketed_diagnostics(self):
        select = PROBE['select_target']
        self.assertEqual(select('[Info] server ready\nTARGET\n'), ('TARGET', 1))
        self.assertEqual(select('[Empty]\n'), (None, 0))
        self.assertEqual(select('A\nB\n'), (None, 2))
        self.assertEqual(select('A\nB\n', 'B'), ('B', 2))
        self.assertEqual(select('A\nB\n', 'OTHER'), (None, 2))

    def fake_device(self, kind, api):
        values = {'const.product.model': 'test-model', 'const.product.devicetype': kind,
                  'const.ohos.apiversion': api, 'const.ohos.fullname': 'test-os'}
        def run(command, **kwargs):
            if command[-2:] == ['list', 'targets']:
                return {'exitCode': 0, 'output': 'TARGET\n'}
            self.assertEqual(command[1:5], ['-t', 'TARGET', 'shell', 'param'])
            self.assertEqual(command[-2], 'get')
            return {'exitCode': 0, 'output': values[command[-1]]}
        with patch.dict(G, {'run': run}):
            return PROBE['audit_device'](self.deveco)

    def test_api20_pc_eligible_but_phone_not(self):
        self.assertTrue(self.fake_device('2in1', '20')['eligiblePc'])
        self.assertFalse(self.fake_device('phone', '26')['eligiblePc'])
        self.assertFalse(self.fake_device('tablet', '26')['eligiblePc'])
        self.assertFalse(self.fake_device('2in1', '19')['eligiblePc'])
        self.assertFalse(self.fake_device('2in1', 'unknown')['eligiblePc'])

    def test_skip_device_issues_no_command(self):
        tool = Mock(side_effect=AssertionError('No device call'))
        with patch.dict(G, {'run': tool}):
            self.assertFalse(PROBE['audit_device'](self.deveco, skip=True)['eligiblePc'])
        tool.assert_not_called()

    def test_all_automatic_gates_do_not_claim_runtime_or_install(self):
        result = self.assembled()
        self.assertTrue(result['automatedPrerequisitesSatisfied'])
        self.assertFalse(result['runtimeInputVerified'])
        self.assertFalse(result['deviceInstalled'])
        self.assertTrue(result['manualStillRequired'])
        self.assertFalse(result['mac']['projectBuildExecuted'])

    def test_discovered_xcode_is_scoped_without_changing_global_selection(self):
        result = self.assembled()
        self.assertEqual(result['mac']['resolvedDeveloperDirectory'], '/fixture/Xcode.app/Contents/Developer')
        self.assertFalse(result['mac']['globalSelectionChanged'])
        self.assertTrue(result['mac']['completeXcodeAvailable'])
        self.assertFalse(result['mac']['projectBuildExecuted'])

    def test_each_major_prerequisite_fails_closed(self):
        for arguments, expected in (
            ({'xcode_exit': 1}, 'complete_xcode_unavailable'),
            ({'configured': False}, 'harmony_signing_configuration_absent_or_unparsed'),
            ({'signature': 'unsigned'}, 'hap_signature_not_verified'),
            ({'signature': 'unverified'}, 'hap_signature_not_verified'),
            ({'eligible': False}, 'authorized_api20_pc_not_verified'),
        ):
            with self.subTest(arguments=arguments):
                result = self.assembled(**arguments)
                self.assertFalse(result['automatedPrerequisitesSatisfied'])
                self.assertIn(expected, result['blockingConditions'])

    def test_cli_exit_status_separates_report_from_readiness(self):
        for strict, redline, expected in ((False, False, 0), (True, False, 2), (False, True, 3)):
            result = {'redlineHit': redline, 'automatedPrerequisitesSatisfied': False}
            arguments = ['--root', str(self.root)] + (['--require-prerequisites'] if strict else [])
            with patch.dict(G, {'audit': Mock(return_value=result)}), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(PROBE['main'](arguments), expected)
            self.assertEqual(json.loads((self.root / 'critique/device-preflight.json').read_text()), result)


if __name__ == '__main__':
    unittest.main(verbosity=2)
