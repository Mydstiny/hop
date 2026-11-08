#!/usr/bin/env python3
"""Read-only regression tests for Hop's bounded Xcode structural checker.

Run: python3 -B tools/test_xcode_project.py
The real snapshot is parsed with native plutil. All negative cases mutate only
in-memory copies and mock reads; no repository files, apps or Xcode builds run.
"""
import contextlib
import copy
import io
from pathlib import Path
import runpy
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / 'apps/macos'
PROJECT_PATH = APP / 'Hop.xcodeproj/project.pbxproj'
INFO_PATH = APP / 'Resources/Info.plist'
SCHEME_PATH = APP / 'Hop.xcodeproj/xcshareddata/xcschemes/Hop.xcscheme'
# run_path also avoids creating a repository __pycache__ on ordinary invocation.
CHECKER = runpy.run_path(str(ROOT / 'tools/check_xcode_project.py'))
CHECK = CHECKER['check']
CHECK_ERROR = CHECKER['CheckError']


class XcodeProjectTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base_document = CHECKER['native_plist'](PROJECT_PATH)
        cls.base_info = CHECKER['native_plist'](INFO_PATH)
        cls.base_scheme = ET.parse(SCHEME_PATH)

    def setUp(self):
        self.document = copy.deepcopy(self.base_document)
        self.info = copy.deepcopy(self.base_info)
        self.scheme = copy.deepcopy(self.base_scheme)
        self.objects = self.document['objects']
        self.project = self.objects[self.document['rootObject']]
        self.target_id = self.project['targets'][0]
        self.target = self.objects[self.target_id]
        self.text_overrides = {}

    def object_id(self, kind, **fields):
        matches = [oid for oid, obj in self.objects.items()
                   if obj.get('isa') == kind and all(obj.get(k) == v for k, v in fields.items())]
        self.assertEqual(len(matches), 1, f'fixture needs one {kind} matching {fields}')
        return matches[0]

    def obj(self, kind, **fields):
        return self.objects[self.object_id(kind, **fields)]

    def release_settings(self):
        ids = self.objects[self.target['buildConfigurationList']]['buildConfigurations']
        return next(self.objects[oid]['buildSettings'] for oid in ids
                    if self.objects[oid]['name'] == 'Release')

    def assert_rejected(self, diagnostic):
        """Run the complete checker with copied metadata and selective read mocks."""
        def native_plist(path):
            if Path(path) == PROJECT_PATH:
                return self.document
            if Path(path) == INFO_PATH:
                return self.info
            self.fail(f'unexpected plist read: {path}')

        read_text = Path.read_text
        def read(path, *args, **kwargs):
            if path in self.text_overrides:
                return self.text_overrides[path]
            return read_text(path, *args, **kwargs)

        with patch.dict(CHECK.__globals__, {'native_plist': native_plist}), \
                patch.object(ET, 'parse', return_value=self.scheme), \
                patch.object(Path, 'read_text', read), \
                contextlib.redirect_stdout(io.StringIO()), \
                self.assertRaisesRegex(CHECK_ERROR, diagnostic):
            CHECK(ROOT)

    def test_actual_project_passes_with_explicit_runtime_boundary(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            CHECK(ROOT)
        self.assertIn('PASS Compile Sources:', output.getvalue())
        self.assertIn('including InputOwnership.swift', output.getvalue())
        self.assertIn('Debug uses ONLY_ACTIVE_ARCH=YES', output.getvalue())
        self.assertIn('BOUNDARY: static structure only.', output.getvalue())

    def test_rejects_dangling_build_file_reference(self):
        ref = self.object_id('PBXFileReference', path='InputOwnership.swift')
        self.obj('PBXBuildFile', fileRef=ref)['fileRef'] = 'D' * 24
        self.assert_rejected('missing object')

    def test_rejects_wrong_reference_type(self):
        self.project['mainGroup'] = self.target_id
        self.assert_rejected('must be.*PBXGroup')

    def test_rejects_unknown_object_schema(self):
        self.objects['B' * 24] = {'isa': 'PBXFutureObject'}
        self.assert_rejected('unsupported object kind')

    def test_rejects_unmodeled_id_shaped_link(self):
        self.target['unexpectedLink'] = self.document['rootObject']
        self.assert_rejected('unmodeled ID-shaped value')

    def test_rejects_input_ownership_omitted_from_compile_sources(self):
        ref = self.object_id('PBXFileReference', path='InputOwnership.swift')
        build_file = self.object_id('PBXBuildFile', fileRef=ref)
        self.obj('PBXSourcesBuildPhase')['files'].remove(build_file)
        del self.objects[build_file]
        self.assert_rejected('Compile Sources mismatch: missing=.*InputOwnership')

    def test_rejects_nonexistent_source_path(self):
        self.obj('PBXFileReference', path='InputOwnership.swift')['path'] = '__checker_missing_ownership__.swift'
        self.assert_rejected('referenced path does not exist')

    def test_rejects_info_plist_as_copied_resource(self):
        keymap = self.object_id('PBXFileReference', path='keymap.json')
        self.obj('PBXBuildFile', fileRef=keymap)['fileRef'] = self.object_id('PBXFileReference', path='Info.plist')
        self.assert_rejected('Copy Bundle Resources differs')

    def test_rejects_stale_keymap_resource(self):
        self.text_overrides[APP / 'Resources/keymap.json'] = '{}'
        self.assert_rejected('bundled keymap is missing or differs')

    def test_rejects_disabled_ls_ui_element(self):
        self.info['LSUIElement'] = False
        self.assert_rejected('LSUIElement=true')

    def test_rejects_minimum_os_drift(self):
        self.release_settings()['MACOSX_DEPLOYMENT_TARGET'] = '13.0'
        self.assert_rejected('Release: minimum macOS differs')

    def test_rejects_release_active_architecture_only(self):
        self.release_settings()['ONLY_ACTIVE_ARCH'] = 'YES'
        self.assert_rejected('Release: unexpected ONLY_ACTIVE_ARCH policy')

    def test_rejects_release_architecture_drift(self):
        self.release_settings()['ARCHS'] = 'x86_64'
        self.assert_rejected('Release: unexpected architecture intent')

    def test_rejects_conditional_source_exclusion(self):
        self.release_settings()['EXCLUDED_SOURCE_FILE_NAMES[sdk=macosx*]'] = 'InputOwnership.swift'
        self.assert_rejected('conditional/filter/Info/executable overrides need checker review')

    def test_rejects_startup_storyboard(self):
        self.info['NSMainStoryboardFile'] = 'Main'
        self.assert_rejected('unexpected startup storyboard/nib declaration')

    def test_rejects_scheme_dangling_blueprint(self):
        self.scheme.getroot().find('.//BuildableReference').set('BlueprintIdentifier', 'D' * 24)
        self.assert_rejected('shared scheme target/product mismatch')

    def test_rejects_disabled_sources_phase_action_mask(self):
        self.obj('PBXSourcesBuildPhase')['buildActionMask'] = '0'
        self.assert_rejected('PBXSourcesBuildPhase: nonstandard build action mask')


if __name__ == '__main__':
    unittest.main(verbosity=2)
