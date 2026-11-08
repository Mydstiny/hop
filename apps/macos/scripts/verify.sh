#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
python3 - "$ROOT" <<'PY'
import json
from pathlib import Path
import subprocess
import sys
import time

root = Path(sys.argv[1])
report_dir = root / "build" / "critique"
report_dir.mkdir(parents=True, exist_ok=True)
started = time.monotonic()
report = {"build": "failing", "tests": "skipped", "reason": "Build has not completed", "commandsRun": [], "failures": []}
logs = []
for name, command in [("build", ["bash", str(root / "scripts/build.sh")]),
                      ("tests", ["bash", str(root / "scripts/test.sh")])]:
    begin = time.monotonic()
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end="", flush=True)
    logs.append("$ " + " ".join(command) + "\n" + result.stdout + f"\n[exit code: {result.returncode}]\n")
    report["commandsRun"].append({"command": command, "exitCode": result.returncode,
                                  "durationMs": round((time.monotonic() - begin) * 1000)})
    report[name] = "passing" if result.returncode == 0 else "failing"
    if result.returncode:
        report["failures"].append({"phase": name, "exitCode": result.returncode, "output": result.stdout[-12000:]})
        if name == "build":
            report["reason"] = "Tests skipped because the application build failed"
        break
    if name == "build":
        report.pop("reason", None)
report["durationMs"] = round((time.monotonic() - started) * 1000)
report["signals"] = {"build.passing": report["build"] == "passing", "tests.passing": report["tests"] == "passing"}
report["critique.score"] = 5 if all(report["signals"].values()) else (3 if report["signals"]["build.passing"] else 1)
report["runtimeInputVerified"] = False
(report_dir / "build-test.json").write_text(json.dumps(report, indent=2) + "\n")
(report_dir / "build-test.log").write_text("\n".join(logs))
print(json.dumps(report["signals"]))
sys.exit(0 if all(report["signals"].values()) else 1)
PY
