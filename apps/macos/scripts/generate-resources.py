#!/usr/bin/env python3
"""Generate the macOS bundle resource from the sole canonical keymap, never a second map."""
import json
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = root.parents[1] / "protocol" / "keymap.json"
subprocess.run([sys.executable, str(root.parents[1] / "tools/generate_hid_keymap.py"), "--check"], check=True)
value = json.loads(source.read_text(encoding="utf-8"))
assert value["version"] == 1
assert len({key["mac"] for key in value["keys"]}) == len(value["keys"])
assert set(value["modifier_bits"].values()) == set(range(9))
for key in value["keys"]:
    assert key["status"] == "unsupported" or isinstance(key["harmony"], int)
# This generated file is a canonical-resource copy, not a handwritten platform mapping.
output = root / "Resources" / "keymap.json"
output.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(f"Generated {output} from {source} ({len(value['keys'])} physical keys)")
