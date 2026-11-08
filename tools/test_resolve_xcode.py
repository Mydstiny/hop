#!/usr/bin/env python3
"""Filesystem/mocked-CLI tests, never an actual Xcode build or installation."""
from pathlib import Path
import contextlib
import io
import json
import runpy
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

MODULE = runpy.run_path(str(Path(__file__).with_name('resolve_xcode.py')))
G = MODULE['main'].__globals__


class ResolveXcodeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hop-xcode-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.global_apps = self.root / 'Applications'
        self.user_apps = self.root / 'User Applications'
        self.roots = [self.global_apps, self.user_apps]

    def install(self, root=None, name='Xcode.app'):
        app = (root or self.global_apps) / name
        developer = app / 'Contents/Developer'
        (developer / 'usr/bin').mkdir(parents=True)
        (developer / 'usr/bin/xcodebuild').touch()
        (developer / 'Platforms/MacOSX.platform').mkdir(parents=True)
        return developer

    def find(self, override=None, active=None):
        return MODULE['find_xcode'](override, active, self.roots)

    def test_clt_binary_without_ide_platform_tree_is_not_full_xcode(self):
        clt = self.root / 'CommandLineTools'
        (clt / 'usr/bin').mkdir(parents=True)
        (clt / 'usr/bin/xcodebuild').touch()
        self.assertIsNone(self.find(active=str(clt))['developerDirectory'])

    def test_active_full_xcode_is_honored(self):
        active = self.install(name='Selected Xcode.app')
        self.install()
        result = self.find(active=str(active))
        self.assertEqual(result['developerDirectory'], str(active))
        self.assertEqual(result['source'], 'xcode-select')

    def test_installed_stable_found_while_active_is_clt(self):
        stable = self.install()
        self.assertEqual(self.find(active='/Library/Developer/CommandLineTools')['developerDirectory'], str(stable))

    def test_user_stable_preferred_to_system_beta(self):
        self.install(name='Xcode-beta.app')
        stable = self.install(self.user_apps)
        self.assertEqual(self.find()['developerDirectory'], str(stable))

    def test_explicit_developer_directory_preferred(self):
        active = self.install()
        explicit = self.install(self.user_apps, 'Custom Xcode.app')
        self.assertEqual(self.find(str(explicit), str(active))['developerDirectory'], str(explicit))

    def test_explicit_app_path_normalized(self):
        developer = self.install()
        app = developer.parent.parent
        self.assertEqual(self.find(str(app))['developerDirectory'], str(developer))

    def test_invalid_override_not_silently_replaced(self):
        valid = self.install()
        result = self.find(str(self.root / 'Missing.app'), str(valid))
        self.assertIsNone(result['developerDirectory'])
        self.assertEqual(result['source'], 'DEVELOPER_DIR')

    def test_renamed_installed_app_and_space_in_path(self):
        developer = self.install(self.user_apps, 'Apple IDE Custom.app')
        self.assertEqual(self.find()['developerDirectory'], str(developer))

    def test_discovery_itself_executes_no_commands_or_global_selection(self):
        developer = self.install()
        with patch.object(G['subprocess'], 'run', side_effect=AssertionError('Discovery must be read-only filesystem checks')):
            self.assertEqual(self.find()['developerDirectory'], str(developer))

    def test_cli_report_does_not_claim_build_or_global_change(self):
        selected = SimpleNamespace(returncode=0, stdout='/Library/Developer/CommandLineTools\n')
        missing = {'developerDirectory': None, 'source': 'not found', 'reason': 'fixture'}
        output = io.StringIO()
        with patch.object(G['subprocess'], 'run', return_value=selected), patch.dict(G, {'find_xcode': Mock(return_value=missing)}), contextlib.redirect_stdout(output):
            self.assertEqual(MODULE['main']([]), 2)
        report = json.loads(output.getvalue())
        self.assertFalse(report['globalSelectionChanged'])
        self.assertFalse(report['projectBuildVerified'])

    def test_cli_path_output_preserves_spaces_without_shell_evaluation(self):
        selected = SimpleNamespace(returncode=0, stdout='CLT\n')
        value = '/Some Folder/Xcode.app/Contents/Developer'
        found = {'developerDirectory': value, 'source': 'fixture', 'reason': 'fixture'}
        output = io.StringIO()
        with patch.object(G['subprocess'], 'run', return_value=selected), patch.dict(G, {'find_xcode': Mock(return_value=found)}), contextlib.redirect_stdout(output):
            self.assertEqual(MODULE['main'](['--print-developer-dir']), 0)
        self.assertEqual(output.getvalue(), value + '\n')


if __name__ == '__main__':
    unittest.main(verbosity=2)
