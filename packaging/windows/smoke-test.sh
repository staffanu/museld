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
    while read -r imp arrow where _; do
        if [ -f "/ucrt64/bin/$imp" ] && [ ! -f "./$imp" ]; then
            echo "FAIL: $f imports $imp, which MSYS2 provides but the package does not carry"
            status=1
        elif [ "$where" = "not" ] || [ "$arrow" = "not" ]; then
            # Neither the package, MSYS2 nor Windows has it; the staging loop
            # skips such a name silently since /ucrt64/bin does not provide it.
            echo "FAIL: $f imports $imp, which nothing on this machine provides"
            status=1
        fi
    done < <(ldd "$f")
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
    if ! grep -q "^usage: museld" help.txt; then
        echo "FAIL: museld.exe --help printed no usage; its output ($(wc -c < help.txt) bytes):"
        cat help.txt
        echo "museld.exe resolves its imports to:"
        ldd museld.exe
        rm help.txt
        exit 1
    fi
    rm help.txt
fi

echo "$pkg: binaries start with only Windows system DLLs available"
