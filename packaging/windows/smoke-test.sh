#!/bin/bash
#
# Run the binaries in a staged package with MSYS2 taken off their PATH, so a DLL
# the package forgot to ship fails here rather than on someone's machine.
#
#     smoke-test.sh <package-name>

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <package-name>" >&2
    exit 2
fi

pkg=$1
cd "$pkg"

# Only the binaries under test get the stripped PATH; the shell keeps its own so
# grep and the rest still work.
clean_path="/c/Windows/System32:/c/Windows"

# An import that MSYS2 provides must travel with the package. Running with the
# stripped PATH does not prove that: Windows ships some of the same DLL names
# itself (onnxruntime.dll, from Windows ML, lives in System32), so a forgotten
# DLL can silently resolve to the wrong library instead of failing to load.
status=0
for f in *.exe *.dll; do
    [ -f "$f" ] || continue
    for imp in $(ldd "$f" | awk '{print $1}'); do
        if [ -f "/ucrt64/bin/$imp" ] && [ ! -f "./$imp" ]; then
            echo "FAIL: $f imports $imp, which MSYS2 provides but the package does not carry"
            status=1
        fi
    done
done
[ "$status" = 0 ] || exit 1

if [ -f ac3rf-efm-decode.exe ]; then
    PATH="$clean_path" ./ac3rf-efm-decode.exe --version
fi

if [ -f museld.exe ]; then
    # museld's --help exits non-zero, so check that it printed its usage; a
    # package missing a DLL never gets as far as printing anything. --help
    # returns before a Vulkan device is needed, so this works without a GPU.
    PATH="$clean_path" ./museld.exe --help > help.txt 2>&1 || true
    grep -q "^usage: museld" help.txt
    rm help.txt
fi

echo "$pkg: binaries start with only Windows system DLLs available"
