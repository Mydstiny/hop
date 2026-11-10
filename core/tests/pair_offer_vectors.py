#!/usr/bin/env python3
import json, pathlib, subprocess, sys
root=pathlib.Path(__file__).resolve().parents[2]
vectors=json.loads((root/'protocol/v2-pairing-vectors.json').read_text())
inputs=''.join(f"{v['type']} {v['hex']}\n" for v in vectors)
results=subprocess.run([sys.argv[1]],input=inputs,text=True,capture_output=True,check=True).stdout.splitlines()
assert len(results)==len(vectors)
for v,result in zip(vectors,results):
    if (result=='valid')!=v['valid']: raise AssertionError(v['name'])
print(f'PASS {len(vectors)} public pairing-offer and selector vectors')
