#!/usr/bin/env python3
"""Deterministic malformed-message differential checks; synthetic data only."""
import json, pathlib, random, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[2]
vectors = json.loads((root/'protocol/v2-golden-vectors.json').read_text())
rng = random.Random(20261110)
cases = []
for v in vectors:
    base = bytearray.fromhex(v['payload_hex'])
    if not base: continue
    cases.append((v['kind'],v['sequence'],v['epoch'],base))
    for _ in range(100):
        body = base.copy()
        for _ in range(rng.randint(1,3)):
            at = rng.randrange(len(body)); body[at] = rng.randrange(256)
        if rng.randrange(5) == 0 and len(body) > 1: del body[rng.randrange(len(body)):]
        cases.append((v['kind'],v['sequence'],v['epoch'],body))
lines=''.join(f'{kind} {seq} {epoch} {body.hex()}\n' for kind,seq,epoch,body in cases)
outputs=[]
for tool in sys.argv[1:3]:
    outputs.append(subprocess.run([tool],input=lines,text=True,capture_output=True,check=True).stdout.splitlines())
assert len(outputs[0]) == len(outputs[1]) == len(cases)
for index,(a,b) in enumerate(zip(*outputs)):
    if a != b: raise AssertionError(f'cross-language validation mismatch at synthetic case {index}: {a} vs {b}')
print(f'PASS {len(cases)} synthetic C++/Swift v2 payload parity checks')
