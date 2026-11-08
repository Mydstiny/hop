#!/usr/bin/env python3
"""Find a real Xcode Developer directory without changing global xcode-select.

Default: print a JSON discovery report. --print-developer-dir emits one path for
build scripts, or exit2 if absent/invalid. Discovery is not license/SDK/build proof.
"""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys


def developer_directory(path):
    path = Path(path).expanduser()
    if path.suffix == '.app':
        path = path / 'Contents/Developer'
    return path.resolve()


def is_full_xcode(path):
    # CLT has a macOS SDK but not the full IDE's Developer binary + Platforms tree.
    return (path / 'usr/bin/xcodebuild').is_file() and (path / 'Platforms/MacOSX.platform').is_dir()


def find_xcode(override=None, active=None, app_roots=None):
    """Pure filesystem discovery. Explicit overrides are honored, never ignored."""
    if override:
        selected = developer_directory(override)
        return {'developerDirectory': str(selected) if is_full_xcode(selected) else None,
                'source': 'DEVELOPER_DIR',
                'reason': 'Explicit override' if is_full_xcode(selected) else 'Explicit DEVELOPER_DIR is not a complete Xcode Developer directory'}
    if active:
        selected = developer_directory(active)
        if is_full_xcode(selected):
            return {'developerDirectory': str(selected), 'source': 'xcode-select', 'reason': 'Existing active full Xcode'}
    roots = list(app_roots) if app_roots is not None else [Path('/Applications'), Path.home() / 'Applications']
    candidates = []
    for priority, root in enumerate(roots):
        for binary in Path(root).glob('*.app/Contents/Developer/usr/bin/xcodebuild'):
            selected = binary.parents[2]
            if is_full_xcode(selected):
                app = selected.parent.parent
                candidates.append((('beta' in app.name.lower(), app.name != 'Xcode.app', priority, str(app)), selected))
    if candidates:
        _, selected = min(candidates, key=lambda entry: entry[0])
        return {'developerDirectory': str(selected.resolve()), 'source': 'application discovery',
                'reason': 'Found installed full Xcode; only the caller process needs DEVELOPER_DIR'}
    return {'developerDirectory': None, 'source': 'not found',
            'reason': 'Install full Xcode from Mac App Store, finish its first launch, or explicitly set DEVELOPER_DIR'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--print-developer-dir', action='store_true')
    args = parser.parse_args(argv)
    try:
        selected = subprocess.run(['xcode-select', '-p'], text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, timeout=8)
        active = selected.stdout.strip() if selected.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        active = None
    result = find_xcode(os.environ.get('DEVELOPER_DIR'), active)
    if args.print_developer_dir:
        if result['developerDirectory']:
            print(result['developerDirectory'])
        else:
            print(result['reason'], file=sys.stderr)
    else:
        print(json.dumps({'activeDeveloperDirectory': active, **result,
                          'globalSelectionChanged': False, 'projectBuildVerified': False}, indent=2))
    return 0 if result['developerDirectory'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
