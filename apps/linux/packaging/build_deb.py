#!/usr/bin/env python3
"""Build the Hop X11 amd64 candidate with independently verified runtime libraries."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from build_preview_deb import library

PACKAGE = 'hop-input'
VERSION = '2.0.0~rc1'
ARCHITECTURE = 'amd64'
RUNTIMES = ('tls', 'shortcut', 'clipboard')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in RUNTIMES:
        parser.add_argument('--' + name, required=True)
        parser.add_argument('--' + name + '-sha256', required=True)
    parser.add_argument('--mbedtls-license', required=True, type=Path, help='Apache 2.0 license from the verified mbedTLS source')
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    libraries = {
        name: library(getattr(args, name), getattr(args, name + '_sha256'))
        for name in RUNTIMES
    }
    apache = args.mbedtls_license.read_text(encoding='utf-8')
    if 'Apache License' not in apache or 'Version 2.0' not in apache:
        parser.error('mbedTLS Apache 2.0 license is required')
    if not args.output_dir.is_absolute() or not args.output_dir.is_dir():
        parser.error('existing absolute output directory required')

    linux = Path(__file__).resolve().parents[1]
    repository = linux.parents[1]
    source = linux / 'hop_linux'
    project_license = repository / 'LICENSE'
    license_text = project_license.read_text(encoding='utf-8')
    notices = repository / 'THIRD_PARTY_NOTICES.md'
    mbedtls_license = Path('third_party/mbedtls-3.6.7-LICENSE.txt')
    canonical_license = repository / mbedtls_license
    if canonical_license.read_bytes() != args.mbedtls_license.read_bytes():
        parser.error('canonical mbedTLS license does not match the verified source')
    destination = args.output_dir / f'{PACKAGE}_{VERSION}_{ARCHITECTURE}.deb'
    if destination.exists():
        parser.error('choose a fresh output directory')

    with tempfile.TemporaryDirectory(prefix='hop-linux-package-', dir=args.output_dir) as directory:
        work = Path(directory)
        stage = work / 'package'
        meta = stage / 'DEBIAN'
        app = stage / 'usr/lib' / PACKAGE
        binary = stage / 'usr/bin'
        desktop = stage / 'usr/share/applications'
        documentation = stage / 'usr/share/doc' / PACKAGE
        for path in (meta, app, binary, desktop, documentation):
            path.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, app / 'hop_linux', ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        manifest = []
        for name, path in libraries.items():
            filename = f'libhop_{name}_runtime.so'
            target = app / filename
            shutil.copyfile(path, target)
            actual_hash = hashlib.sha256(target.read_bytes()).hexdigest()
            if actual_hash != getattr(args, name + '_sha256'):
                raise ValueError(f'{name} changed while staging')
            manifest.append(f'{actual_hash}  {filename}')
        (documentation / 'runtime.sha256').write_text('\n'.join(manifest) + '\n', encoding='ascii')
        (documentation / 'copyright').write_text(
            'Hop source and application code\n\n' + license_text +
            '\n\nThe TLS runtime includes mbedTLS 3.6.7 under Apache-2.0.\n'
            'Source: https://github.com/Mbed-TLS/mbedtls/tree/mbedtls-3.6.7\n\n' + apache,
            encoding='utf-8')
        (documentation / mbedtls_license.parent).mkdir()
        shutil.copyfile(project_license, documentation / project_license.name)
        shutil.copyfile(canonical_license, documentation / mbedtls_license)
        shutil.copyfile(notices, documentation / notices.name)
        shutil.copyfile(linux / 'README.md', documentation / 'README.md')
        (meta / 'control').write_text(f'''Package: {PACKAGE}
Version: {VERSION}
Section: utils
Priority: optional
Architecture: {ARCHITECTURE}
Maintainer: Hop maintainers <Mydstiny@users.noreply.github.com>
Homepage: https://github.com/Mydstiny/hop
Depends: python3 (>= 3.10), python3-gi, python3-dbus, gir1.2-secret-1, libgtk-3-0, libx11-6, libxtst6, libxi6, libxrandr2, libxfixes3, libgdk-pixbuf-2.0-0, libc6 (>= 2.35), libgcc-s1
Recommends: avahi-daemon, gnome-keyring, fonts-noto-cjk
Description: Share keyboard, pointer and clipboard across paired devices on X11
 Hop connects paired devices over a local network using mutually authenticated
 TLS. This release candidate targets Ubuntu 22.04 amd64 X11 desktop sessions.
''', encoding='utf-8')
        launcher = binary / PACKAGE
        launcher.write_text('''#!/bin/sh
set -eu
launcher=$(readlink -f -- "$0")
cd -- "${launcher%/*}/../lib/hop-input"
export PYTHONPATH="$PWD" PYTHONNOUSERSITE=1
case "$#:${1-}" in
    1:--check) exec /usr/bin/python3 -B -m hop_linux --check ;;
    0:) exec /usr/bin/python3 -B -m hop_linux --app \\
        --tls-library "$PWD/libhop_tls_runtime.so" \\
        --shortcut-library "$PWD/libhop_shortcut_runtime.so" \\
        --clipboard-library "$PWD/libhop_clipboard_runtime.so" ;;
    *) printf '%s\\n' 'Usage: hop-input [--check]' >&2; exit 2 ;;
esac
''', encoding='utf-8')
        launcher.chmod(0o755)
        (desktop / f'{PACKAGE}.desktop').write_text('''[Desktop Entry]
Type=Application
Name=Hop
Comment=Share a keyboard, mouse and clipboard across paired devices
Comment[zh_CN]=在已配对的设备间共享键盘、鼠标和剪贴板
Exec=/usr/bin/hop-input
Icon=input-mouse
Terminal=false
Categories=Utility;
''', encoding='utf-8')
        if 'SOURCE_DATE_EPOCH' in os.environ and not re.fullmatch(r'[0-9]+', os.environ['SOURCE_DATE_EPOCH']):
            parser.error('SOURCE_DATE_EPOCH must be a Unix timestamp')
        staged_package = work / destination.name
        subprocess.run(['dpkg-deb', '--build', '--root-owner-group', str(stage), str(staged_package)], check=True)
        # Publish the complete local file atomically while refusing to clobber
        # an existing candidate, even if another builder won the race.
        os.link(staged_package, destination)
    print(destination)
    print('SHA256', hashlib.sha256(destination.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
