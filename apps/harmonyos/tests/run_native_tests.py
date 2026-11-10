#!/usr/bin/env python3
"""Compile actual portable Hop C++ against fake SDK symbols, never real input.
Outputs stay inside apps/harmonyos/tests/.build. No dependencies/install/network
except loopback sockets between the tested native Client and in-process peer.
"""
import json
from pathlib import Path
import subprocess
import sys
import time
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CPP = HERE.parent / 'entry/src/main/cpp'
OUT = HERE / '.build'
OUT.mkdir(exist_ok=True)
subprocess.run([sys.executable, str(ROOT / 'tools/generate_hid_keymap.py'), '--check'], check=True)
vectors = json.loads((ROOT / 'protocol/golden-vectors.json').read_text())
extra = ROOT / 'protocol/handshake-vectors.json'
if extra.exists():
    additional = json.loads(extra.read_text())
    if isinstance(additional, list):
        vectors += additional
frames = [v['hex'] for v in vectors]
(OUT / 'golden.hex').write_text('\n'.join(frames) + '\n')
command = ['xcrun', 'clang++', '-std=c++17', '-pthread', '-Wall', '-Wextra',
           '-Werror=return-type', '-I', str(HERE / 'fake'), '-I', str(CPP),
           str(HERE / 'native_tests.cpp'), str(CPP / 'client.cpp'), '-o', str(OUT / 'native_tests')]
started = time.time()
print('+', ' '.join(command), flush=True)
result = subprocess.run(command)
phase = 'compile'
output = ''
if result.returncode == 0:
    phase = 'tests'
    print(f'+ native_tests ({len(frames)} canonical golden frames)', flush=True)
    result = subprocess.run([str(OUT / 'native_tests'), str(OUT / 'golden.hex')], capture_output=True, text=True)
    output = result.stdout + result.stderr
    print(output, end='', flush=True)
count = re.search(r'(\d+) checks', output)
report = {
    'tests': 'passing' if result.returncode == 0 else 'failing',
    'tests.passing': result.returncode == 0,
    'exitCode': result.returncode,
    'phase': phase,
    'durationMs': round((time.time() - started) * 1000),
    'goldenFrames': len(frames),
    'checks': int(count.group(1)) if count else None,
    'actualSources': ['protocol.h', 'pair_state.h', 'injector.h', 'client.cpp'],
    'sdk': 'test-only recording fakes; no real OH_Input library linked',
    'network': 'loopback only',
    'deviceInputTested': False,
}
(OUT / 'native-tests-report.json').write_text(json.dumps(report, indent=2) + '\n')
sys.exit(result.returncode)
