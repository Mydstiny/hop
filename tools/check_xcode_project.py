#!/usr/bin/env python3
"""Read-only, stdlib structural checks for Hop's current explicit Xcode schema.

Apple plutil parses OpenStep syntax; this is NOT a general pbxproj parser or an
Xcode build-settings evaluator. Unknown object kinds, reference-shaped fields,
xcconfig inheritance, conditional settings and source/architecture filters fail
closed rather than being silently certified. No builds, signing or app launch.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import plistlib
import re
import shlex
import subprocess
import sys
import xml.etree.ElementTree as ET


class CheckError(Exception):
    pass


def need(condition, message):
    if not condition:
        raise CheckError(message)


def native_plist(path):
    """Convert to stdout only: never rewrite the source project or plist."""
    result = subprocess.run(['/usr/bin/plutil', '-convert', 'xml1', '-o', '-', str(path)],
                            capture_output=True, check=False)
    need(result.returncode == 0,
         f'plutil rejected {path}: {result.stderr.decode(errors="replace").strip()}')
    return plistlib.loads(result.stdout)


# field: (expected destination ISA(s), array-valued). These are object links,
# unlike build settings, filesystem paths, shell inputPaths or scheme XML IDs.
PHASES = ('PBXSourcesBuildPhase', 'PBXResourcesBuildPhase',
          'PBXFrameworksBuildPhase', 'PBXShellScriptBuildPhase')
SCHEMA = {
    'PBXProject': {'buildConfigurationList': ('XCConfigurationList', False),
                   'mainGroup': ('PBXGroup', False), 'productRefGroup': ('PBXGroup', False),
                   'targets': ('PBXNativeTarget', True)},
    'PBXNativeTarget': {'buildConfigurationList': ('XCConfigurationList', False),
                        'buildPhases': (PHASES, True), 'buildRules': ('PBXBuildRule', True),
                        'dependencies': ('PBXTargetDependency', True),
                        'productReference': ('PBXFileReference', False)},
    'PBXGroup': {'children': (('PBXGroup', 'PBXFileReference'), True)},
    'PBXFileReference': {},
    'PBXBuildFile': {'fileRef': ('PBXFileReference', False)},
    'XCConfigurationList': {'buildConfigurations': ('XCBuildConfiguration', True)},
    'XCBuildConfiguration': {},
    **{kind: {'files': ('PBXBuildFile', True)} for kind in PHASES},
}


def check_graph(document):
    need(isinstance(document, dict), 'project root must be a dictionary')
    objects = document.get('objects')
    need(isinstance(objects, dict) and objects, 'missing project objects dictionary')
    edges, modeled = {}, {('rootObject',)}
    for oid, obj in objects.items():
        need(re.fullmatch(r'[0-9A-Fa-f]{24}', oid), f'unsupported object ID: {oid}')
        need(isinstance(obj, dict), f'{oid}: object must be a dictionary')
        kind = obj.get('isa')
        need(kind in SCHEMA, f'{oid}: unsupported object kind {kind!r}; extend checker schema')
        edges[oid] = []
        for field, (expected, many) in SCHEMA[kind].items():
            value = obj.get(field)
            refs = value if many else [value]
            need(isinstance(refs, list), f'{oid}.{field}: expected an object-ID array')
            for index, ref in enumerate(refs):
                need(isinstance(ref, str) and ref in objects, f'{oid}.{field}: missing object {ref!r}')
                types = (expected,) if isinstance(expected, str) else expected
                need(objects[ref].get('isa') in types,
                     f'{oid}.{field}: {ref} must be {types}, not {objects[ref].get("isa")}')
                edges[oid].append(ref)
                modeled.add(('objects', oid, field, index) if many else ('objects', oid, field))
        need('baseConfigurationReference' not in obj, f'{oid}: xcconfig evaluation is unsupported')
        need(not any(k in obj for k in ('platformFilter', 'platformFilters')),
             f'{oid}: platform-filtered membership is unsupported')

    # Catch unmodeled links without assuming every arbitrary string is an ID.
    # ID-shaped values in unknown fields require explicit schema review.
    def scan(value, trail=()):
        if isinstance(value, dict):
            for key, child in value.items():
                if re.fullmatch(r'[0-9A-Fa-f]{24}', key) and trail != ('objects',):
                    raise CheckError(f'unmodeled object-ID key at {trail + (key,)}')
                scan(child, trail + (key,))
        elif isinstance(value, list):
            for index, child in enumerate(value):
                scan(child, trail + (index,))
        elif isinstance(value, str) and re.fullmatch(r'[0-9A-Fa-f]{24}', value):
            need(trail in modeled, f'unmodeled ID-shaped value at {trail}; extend checker schema')
    scan(document)
    root_id = document.get('rootObject')
    need(root_id in objects and objects[root_id].get('isa') == 'PBXProject', 'invalid rootObject')
    seen, pending = set(), [root_id]
    while pending:
        oid = pending.pop()
        if oid not in seen:
            seen.add(oid)
            pending.extend(edges[oid])
    need(seen == set(objects), f'unreachable project objects: {sorted(set(objects) - seen)}')
    return objects, objects[root_id], sum(map(len, edges.values())) + 1


def check(root):
    app = root / 'apps/macos'
    project_dir = app / 'Hop.xcodeproj'
    objects, project, links = check_graph(native_plist(project_dir / 'project.pbxproj'))
    need(len(project['targets']) == 1, 'expected exactly one production Hop target')
    target_id = project['targets'][0]
    target = objects[target_id]
    need(target.get('name') == 'Hop' and target.get('productType') == 'com.apple.product-type.application',
         'Hop must be a native application target')
    product_id = target['productReference']
    need(objects[product_id].get('path') == 'Hop.app', 'unexpected target product')
    need(not project.get('projectDirPath') and not project.get('projectRoot'),
         'non-default project directory roots are unsupported')

    paths = {}
    def walk(oid, parent, ancestors=()):
        need(oid not in ancestors, f'group cycle at {oid}')
        need(oid not in paths, f'file/group has multiple parents: {oid}')
        obj = objects[oid]
        tree, path = obj.get('sourceTree'), obj.get('path', '')
        need(isinstance(path, str), f'{oid}: invalid filesystem path')
        if tree == 'BUILT_PRODUCTS_DIR':
            need(oid == product_id, 'only the target product may be a generated file reference')
            paths[oid] = None  # A build output is not expected to exist before building.
            return
        need(tree in ('<group>', 'SOURCE_ROOT'), f'{oid}: unsupported sourceTree {tree!r}')
        need('$' not in path and not Path(path).is_absolute(), f'{oid}: unsupported variable/absolute path')
        resolved = ((app if tree == 'SOURCE_ROOT' else parent) / path).resolve()
        paths[oid] = resolved
        need(resolved.exists(), f'{oid}: referenced path does not exist: {resolved}')
        if obj['isa'] == 'PBXGroup':
            need(resolved.is_dir(), f'{oid}: group is not a directory')
            for child in obj['children']:
                walk(child, resolved, ancestors + (oid,))
        else:
            need(resolved.is_file(), f'{oid}: expected a file: {resolved}')
    walk(project['mainGroup'], app)
    need(all(oid in paths for oid, obj in objects.items() if obj['isa'] == 'PBXFileReference'),
         'file reference outside the main group tree')

    phases = [objects[oid] for oid in target['buildPhases']]
    def phase(kind):
        matches = [p for p in phases if p['isa'] == kind]
        need(len(matches) == 1, f'expected exactly one {kind}')
        value = matches[0]
        need(str(value.get('runOnlyForDeploymentPostprocessing')) == '0', f'{kind}: deployment-only phase')
        need(str(value.get('buildActionMask')) == '2147483647', f'{kind}: nonstandard build action mask')
        return value
    def members(kind):
        refs = [objects[bid]['fileRef'] for bid in phase(kind)['files']]
        need(len(refs) == len(set(refs)), f'{kind}: duplicate file membership')
        need(all(paths.get(ref) is not None for ref in refs), f'{kind}: unresolved/generated input')
        return {paths[ref] for ref in refs}
    source_paths = members('PBXSourcesBuildPhase')
    actual_sources = {p.resolve() for p in (app / 'Sources').rglob('*.swift')}
    need(actual_sources and source_paths == actual_sources,
         f'Compile Sources mismatch: missing={sorted(map(str, actual_sources - source_paths))}; '
         f'extra={sorted(map(str, source_paths - actual_sources))}')
    need(app / 'Sources/InputOwnership.swift' in source_paths, 'InputOwnership.swift is not compiled')
    resource_paths = members('PBXResourcesBuildPhase')
    info_path = app / 'Resources/Info.plist'
    actual_resources = {p.resolve() for p in (app / 'Resources').rglob('*') if p.is_file()} - {info_path}
    need(resource_paths == actual_resources, 'Copy Bundle Resources differs from Resources minus Info.plist')
    need(not phase('PBXFrameworksBuildPhase')['files'], 'explicit framework links need checker review')
    for path in paths.values():
        need(path is None or path.suffix.lower() not in ('.storyboard', '.xib', '.nib'),
             f'unexpected window/storyboard/nib resource: {path}')

    generator = phase('PBXShellScriptBuildPhase')
    need(generator.get('shellPath') == '/bin/bash', 'unexpected resource-generation shell')
    need(generator.get('shellScript') == 'set -euo pipefail\n/usr/bin/python3 "$SRCROOT/scripts/generate-resources.py"\n',
         'resource-generation script shape changed; review instead of evaluating arbitrary shell')
    need((app / 'scripts/generate-resources.py').is_file(), 'missing resource generator script')
    need(generator.get('inputPaths') == ['$(SRCROOT)/../../protocol/keymap.json'] and
         generator.get('outputPaths') == ['$(SRCROOT)/Resources/keymap.json'],
         'canonical keymap generator input/output contract changed')
    need(phases.index(generator) < phases.index(phase('PBXResourcesBuildPhase')),
         'keymap generation must precede resource copying')
    canonical = json.loads((root / 'protocol/keymap.json').read_text())
    need(app / 'Resources/keymap.json' in resource_paths and
         json.loads((app / 'Resources/keymap.json').read_text()) == canonical,
         'bundled keymap is missing or differs from canonical protocol keymap')

    def configurations(owner):
        config_list = objects[owner['buildConfigurationList']]
        configs = [objects[oid] for oid in config_list['buildConfigurations']]
        need(Counter(c.get('name') for c in configs) == Counter(('Debug', 'Release')),
             'expected unique Debug and Release configurations')
        need(config_list.get('defaultConfigurationName') in ('Debug', 'Release'), 'invalid default configuration')
        for config in configs:
            settings = config.get('buildSettings')
            need(isinstance(settings, dict), 'buildSettings must be a dictionary')
            need(not any('[' in key or key.startswith(('EXCLUDED_', 'INCLUDED_', 'INFOPLIST_KEY_')) or
                         key in ('VALID_ARCHS', 'EXECUTABLE_NAME', 'EXECUTABLE_PREFIX', 'EXECUTABLE_SUFFIX')
                         for key in settings), 'conditional/filter/Info/executable overrides need checker review')
        return {c['name']: c['buildSettings'] for c in configs}
    project_configs, target_configs = configurations(project), configurations(target)
    info = native_plist(info_path)
    need(info.get('LSUIElement') is True, 'Info.plist must declare LSUIElement=true')
    need(info.get('LSMinimumSystemVersion') == '14.0', 'Info.plist minimum macOS must be 14.0')
    need(info.get('CFBundlePackageType') == 'APPL', 'unexpected bundle package type')
    need(not any(key.startswith(('NSMainStoryboardFile', 'NSMainNibFile', 'UIMainStoryboardFile'))
                 for key in info), 'unexpected startup storyboard/nib declaration')
    for name in ('Debug', 'Release'):
        settings = {**project_configs[name], **target_configs[name]}
        for layer in (project_configs[name], target_configs[name]):
            need(layer.get('MACOSX_DEPLOYMENT_TARGET') == '14.0', f'{name}: minimum macOS differs from 14.0')
        need(settings.get('SDKROOT') == 'macosx' and settings.get('SWIFT_VERSION') == '5.0',
             f'{name}: unexpected SDK or Swift language version')
        need(settings.get('GENERATE_INFOPLIST_FILE') == 'NO' and
             settings.get('INFOPLIST_FILE') == 'Resources/Info.plist', f'{name}: wrong Info.plist wiring')
        need(settings.get('PRODUCT_BUNDLE_IDENTIFIER') == info.get('CFBundleIdentifier'), f'{name}: bundle ID mismatch')
        product_name = settings.get('PRODUCT_NAME', '').replace('$(TARGET_NAME)', target['name'])
        need(product_name == info.get('CFBundleExecutable') == info.get('CFBundleName') == 'Hop',
             f'{name}: executable/product/bundle name mismatch')
        archs = settings.get('ARCHS')
        need(archs in ('$(ARCHS_STANDARD)', '${ARCHS_STANDARD}', 'arm64 x86_64', 'x86_64 arm64') or
             archs == ['arm64', 'x86_64'], f'{name}: unexpected architecture intent {archs!r}')
        need(settings.get('ONLY_ACTIVE_ARCH') == ('YES' if name == 'Debug' else 'NO'),
             f'{name}: unexpected ONLY_ACTIVE_ARCH policy')

    scheme = ET.parse(project_dir / 'xcshareddata/xcschemes/Hop.xcscheme').getroot()
    need(scheme.tag == 'Scheme', 'invalid shared scheme root')
    references = scheme.findall('.//BuildableReference')
    need(references, 'shared scheme has no buildable references')
    for ref in references:
        need(ref.get('BlueprintIdentifier') == target_id and ref.get('BlueprintName') == 'Hop' and
             ref.get('BuildableName') == 'Hop.app' and
             ref.get('ReferencedContainer') == 'container:Hop.xcodeproj', 'shared scheme target/product mismatch')
    entries = scheme.findall('./BuildAction/BuildActionEntries/BuildActionEntry')
    need(len(entries) == 1 and entries[0].find('BuildableReference') is not None and
         all(entries[0].get(key) == 'YES' for key in ('buildForRunning', 'buildForProfiling', 'buildForArchiving')),
         'scheme does not build Hop for run/profile/archive')
    for action, expected in (('LaunchAction', 'Debug'), ('ProfileAction', 'Release'),
                             ('AnalyzeAction', 'Debug'), ('ArchiveAction', 'Release')):
        node = scheme.find(action)
        need(node is not None and node.get('buildConfiguration') == expected, f'scheme {action}: invalid configuration')
        if action in ('LaunchAction', 'ProfileAction'):
            need(node.find('./BuildableProductRunnable/BuildableReference') is not None,
                 f'{action}: expected Hop product runnable')

    # Deliberately limited source-contract checks, NOT a shell interpreter.
    native = (app / 'scripts/build.sh').read_text()
    arch_default = re.search(r'^ARCHS="\$\{HOP_ARCHS:-([^}]+)\}"$', native, re.MULTILINE)
    need(arch_default and shlex.split(arch_default[1]) == ['arm64', 'x86_64'],
         'native build script universal default changed')
    for token in ('-target "$arch-apple-macosx14.0"', '-swift-version 5',
                  'SOURCES=("$ROOT"/Sources/*.swift)', 'cp "$ROOT/Resources/Info.plist"'):
        need(token in native, f'native build source contract changed: {token}')
    need(all(p.parent == app / 'Sources' for p in actual_sources),
         'native Sources/*.swift glob does not cover nested production sources')
    xcode_route = (app / 'scripts/xcode-build.sh').read_text().replace('\\\n', ' ')
    commands = [shlex.split(line) for line in xcode_route.splitlines() if line.startswith('xcodebuild -project ')]
    need(len(commands) == 1 and all(token in commands[0] for token in
                                  ('Release', 'ARCHS=arm64 x86_64', 'ONLY_ACTIVE_ARCH=NO', 'build')),
         'Xcode release helper no longer requests explicit universal architectures')

    print(f'PASS structural graph: {len(objects)} objects, {links} typed object links; all reachable/resolved')
    print(f'PASS Compile Sources: all {len(actual_sources)} production Swift files, including InputOwnership.swift')
    print(f'PASS resources: {len(resource_paths)} copied resource(s), canonical keymap generator wiring; Info.plist processed separately')
    print('PASS metadata: LSUIElement=true, macOS 14.0, bundle/product identity, no startup storyboard/nib')
    print(f'PASS shared scheme: {len(references)} valid Hop references; Debug run/analyze, Release profile/archive')
    print('PASS architecture intent: Release standard architectures + ONLY_ACTIVE_ARCH=NO; native/helper explicitly arm64 + x86_64')
    print('NOTE Debug uses ONLY_ACTIVE_ARCH=YES; it is not claimed to emit a universal binary.')
    print('NOTE ARCHS_STANDARD is not expanded here; native script checks are source contracts, not shell execution.')
    print('BOUNDARY: static structure only. No xcodebuild/build/signing/app launch/input or Keychain checks; no runtime or binary proof.')
    print('NOTE Shared scheme has no TestAction.' if scheme.find('TestAction') is None else 'NOTE Scheme TestAction is outside this checker scope.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1],
                        help='repository root (default: parent of tools)')
    args = parser.parse_args()
    try:
        check(args.root.resolve())
    except (CheckError, OSError, ValueError, TypeError, KeyError, ET.ParseError, plistlib.InvalidFileException) as exc:
        print(f'FAIL Xcode structural check: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
