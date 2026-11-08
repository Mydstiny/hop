#!/usr/bin/env python3
"""Synthetic archive-metadata tests only; not signature or device acceptance."""
from pathlib import Path
import hashlib
import json
import os
import tempfile
import unittest
from unittest.mock import patch
import verify


class PackageMetadataTests(unittest.TestCase):
    def setUp(self):
        parent = verify.ROOT / 'artifacts/test-build/verifier-fixtures'
        parent.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix='owned-', dir=parent)
        self.root = Path(temporary.name).resolve()
        # Check the exact managed cleanup target before registering its deletion.
        self.assertEqual(self.root.parent, parent.resolve())
        self.addCleanup(temporary.cleanup)
        root_patch = patch.object(verify, 'ROOT', self.root)
        root_patch.start()
        self.addCleanup(root_patch.stop)
        env_patch = patch.dict(os.environ)
        env_patch.start()
        self.addCleanup(env_patch.stop)
        os.environ.pop('HOP_BUILD_DIR', None)
        self.write('apps/macos/build/Hop-macos-universal-adhoc.zip', b'stale-default')
        self.write('apps/harmonyos/entry/build/default/outputs/default/entry-default-unsigned.hap', b'unsigned-fixture')

    def write(self, name, payload):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        return path

    def isolated(self, absolute):
        output = self.root / 'artifacts/isolated-mac'
        self.write('artifacts/isolated-mac/Hop-macos-universal-adhoc.zip', b'fresh-isolated')
        os.environ['HOP_BUILD_DIR'] = str(output) if absolute else 'artifacts/isolated-mac'
        records = verify.package_records()
        self.assertEqual(records[0]['path'], 'artifacts/isolated-mac/Hop-macos-universal-adhoc.zip')
        self.assertEqual(records[0]['sha256'], hashlib.sha256(b'fresh-isolated').hexdigest())
        self.assertNotEqual(records[0]['sha256'], hashlib.sha256(b'stale-default').hexdigest())
        self.assertIn('artifacts/isolated-mac/', (self.root / 'artifacts/SHA256SUMS').read_text())

    def signed_report_fixture(self, digest):
        artifact = 'entry/build/default/outputs/default/entry-default-signed.hap'
        self.write('apps/harmonyos/' + artifact, b'signed-fixture-not-a-real-HAP')
        self.write('apps/harmonyos/tests/.build/hap-build-report.json', json.dumps({
            'build.passing': True, 'signature': {'status': 'verified_by_sdk_tool'},
            'artifact': {'path': artifact, 'sha256': digest}
        }).encode())

    def test_unset_output_uses_default(self):
        self.assertEqual(verify.package_records()[0]['path'], 'apps/macos/build/Hop-macos-universal-adhoc.zip')

    def test_empty_output_matches_bash_default(self):
        os.environ['HOP_BUILD_DIR'] = ''
        self.assertEqual(verify.package_records()[0]['path'], 'apps/macos/build/Hop-macos-universal-adhoc.zip')

    def test_relative_output_does_not_hash_stale_default(self):
        self.isolated(False)

    def test_absolute_output_does_not_hash_stale_default(self):
        self.isolated(True)

    def test_mutated_signed_bytes_cannot_inherit_verified_label(self):
        self.signed_report_fixture('0' * 64)
        with self.assertRaisesRegex(ValueError, 'changed since SDK verification'):
            verify.package_records()

    def test_matching_synthetic_report_digest_is_recorded(self):
        digest = hashlib.sha256(b'signed-fixture-not-a-real-HAP').hexdigest()
        self.signed_report_fixture(digest)
        self.assertEqual(verify.package_records()[-1]['sha256'], digest)


if __name__ == '__main__':
    unittest.main()
