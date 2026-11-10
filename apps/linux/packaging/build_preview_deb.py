#!/usr/bin/env python3
"""Build an isolated, explicitly experimental amd64 Debian package.

Requires caller-supplied verified Linux shared libraries and their exact hashes.
Never modifies the system or overwrites an existing output. This is not a release.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import secrets
import shutil
import struct
import subprocess
import tempfile


def library(path,expected):
    path=Path(path)
    if not path.is_absolute() or not path.is_file() or not re.fullmatch('[0-9a-f]{64}',expected):raise ValueError('absolute library and SHA-256 required')
    raw=path.read_bytes()
    if hashlib.sha256(raw).hexdigest()!=expected:raise ValueError('library hash mismatch')
    if len(raw)<20 or raw[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',raw,16)[0]!=3 or struct.unpack_from('<H',raw,18)[0]!=62:
        raise ValueError('library must be an amd64 ELF shared object')
    return path


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('tls','shortcut','clipboard'):
        p.add_argument('--'+name,required=True);p.add_argument('--'+name+'-sha256',required=True)
    p.add_argument('--output-dir',required=True);args=p.parse_args()
    libs={name:library(getattr(args,name),getattr(args,name+'_sha256')) for name in ('tls','shortcut','clipboard')}
    output=Path(args.output_dir)
    if not output.is_absolute() or not output.is_dir():raise ValueError('existing absolute output directory required')
    source=Path(__file__).resolve().parents[1]/'hop_linux'
    if not source.is_dir():raise RuntimeError('Linux source missing')
    if not shutil.which('dpkg-deb'):raise RuntimeError('dpkg-deb is required')
    with tempfile.TemporaryDirectory(prefix='hop-preview-package-') as temporary:
        stage=Path(temporary)/'package';meta=stage/'DEBIAN';app=stage/'usr/lib/hop-v2-preview';binary=stage/'usr/bin';desktop=stage/'usr/share/applications'
        for d in (meta,app,binary,desktop):d.mkdir(parents=True,exist_ok=True)
        shutil.copytree(source,app/'hop_linux',ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
        for name,path in libs.items():shutil.copyfile(path,app/('libhop_'+name+'_runtime.so'))
        control='''Package: hop-v2-preview
Version: 0.0.0~dev1
Section: utils
Priority: optional
Architecture: amd64
Maintainer: Hop development preview <noreply@example.invalid>
Depends: python3 (>= 3.10), python3-gi, python3-dbus, gir1.2-secret-1, libgtk-3-0, libx11-6, libxtst6, libxi6, libxrandr2, libxfixes3, libgdk-pixbuf-2.0-0, libc6 (>= 2.35)
Recommends: avahi-daemon, gnome-keyring
Description: Hop experimental X11 development preview
 A development-only preview; cross-device acceptance, Wayland support,
 automatic screen-edge routing and clipboard UI are not complete.
'''
        (meta/'control').write_text(control,encoding='utf8')
        launcher=binary/'hop-v2-preview'
        launcher.write_text('''#!/bin/sh
PYTHONPATH=/usr/lib/hop-v2-preview exec /usr/bin/python3 -B -m hop_linux --app --tls-library /usr/lib/hop-v2-preview/libhop_tls_runtime.so --shortcut-library /usr/lib/hop-v2-preview/libhop_shortcut_runtime.so --clipboard-library /usr/lib/hop-v2-preview/libhop_clipboard_runtime.so "$@"
''',encoding='utf8');launcher.chmod(0o755)
        (desktop/'hop-v2-preview.desktop').write_text('''[Desktop Entry]
Type=Application
Name=Hop Development Preview
Name[zh_CN]=Hop 开发预览
Comment=Experimental LAN keyboard and mouse sharing
Exec=hop-v2-preview
Icon=input-mouse
Terminal=false
Categories=Utility;
''',encoding='utf8')
        built=Path(temporary)/'preview.deb'
        subprocess.run(['dpkg-deb','--build','--root-owner-group',str(stage),str(built)],check=True)
        destination=output/('hop-v2-preview-'+secrets.token_hex(6)+'_amd64.deb')
        with built.open('rb') as inp,destination.open('xb') as out:
            shutil.copyfileobj(inp,out);out.flush();os.fsync(out.fileno())
        print('PREVIEW_PACKAGE',destination)
        print('SHA256',hashlib.sha256(destination.read_bytes()).hexdigest())

if __name__=='__main__':main()
