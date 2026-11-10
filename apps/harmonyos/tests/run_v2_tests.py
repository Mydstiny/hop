#!/usr/bin/env python3
"""Build isolated v2 session/injector tests against recording fake SDK only."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[3]
cpp=root/'apps/harmonyos/entry/src/main/cpp'
tests=root/'apps/harmonyos/tests'
out=tests/'.build/v2_injector_tests'
sources=['layout.cpp','focus.cpp','input_policy.cpp','frame.cpp','outbound_queue.cpp','clipboard_transfer.cpp','clipboard_transfer_c.cpp','wire_validation.cpp','shortcut_translator.cpp','shortcut_translator_c.cpp','session.cpp','session_c.cpp']
command=['xcrun','clang++','-std=c++17','-pthread','-Wall','-Wextra','-Werror=return-type','-I',str(tests/'fake'),'-I',str(cpp),'-I',str(root/'core/include'),str(tests/'v2_injector_tests.cpp')]+[str(root/'core/src'/x) for x in sources]+['-o',str(out)]
out.parent.mkdir(exist_ok=True)
subprocess.run(command,check=True)
subprocess.run([str(out)],check=True)
