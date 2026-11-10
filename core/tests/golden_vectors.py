#!/usr/bin/env python3
"""Cross-check independently encoded v2 fixtures against the C++ codec."""
import json
import pathlib
import subprocess
import sys

vectors = json.loads((pathlib.Path(__file__).resolve().parents[2] / 'protocol/v2-golden-vectors.json').read_text())
for v in vectors:
    result = subprocess.run([sys.argv[1], str(v['kind']), str(v['sequence']), str(v['epoch']), v['payload_hex']],
                            text=True, capture_output=True, check=True).stdout.strip()
    expected = ('valid ' if v['valid'] else 'invalid ') + v['frame_hex']
    if result != expected:
        raise AssertionError(v['name'])
print(f"PASS {len(vectors)} shared v2 payload/frame golden vectors (no network or application)")
