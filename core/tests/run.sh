#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/hop-core-test.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic \
    -I "$root/core/include" "$root/core/src/layout.cpp" "$root/core/src/focus.cpp" "$root/core/src/input_policy.cpp" "$root/core/src/frame.cpp" "$root/core/src/outbound_queue.cpp" "$root/core/src/clipboard_transfer.cpp" "$root/core/src/clipboard_transfer_c.cpp" "$root/core/src/wire_validation.cpp" "$root/core/src/shortcut_translator.cpp" "$root/core/src/shortcut_translator_c.cpp" \
    "$root/core/tests/core_tests.cpp" -o "$build_dir/hop_core_tests"
"$build_dir/hop_core_tests"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -I "$root/core/include" \
    "$root/core/src/frame.cpp" "$root/core/src/wire_validation.cpp" "$root/core/tests/wire_vector_tool.cpp" -o "$build_dir/hop_wire_vector_tool"
python3 "$root/core/tests/golden_vectors.py" "$build_dir/hop_wire_vector_tool"

# Session state machine and its C ABI: the same sources CMake builds into hop_core.
for name in session_tests session_c_tests; do
    "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pedantic -I "$root/core/include" \
        "$root"/core/src/*.cpp "$root/core/tests/$name.cpp" -o "$build_dir/hop_$name"
    "$build_dir/hop_$name"
done
