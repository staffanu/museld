#!/bin/bash
#
# Stage a Windows package and zip it up, for use from an MSYS2 UCRT64 shell.
#
#     stage-package.sh <build-dir> <package-name> <binary>...
#
# The binaries are taken from <build-dir>/src, together with everything they
# need to run on a machine that has no MSYS2: the DLLs they resolve to in
# /ucrt64/bin, and — for museld — the SPIR-V shaders and the subtitle font it
# loads from its own directory at startup.

set -euo pipefail

if [ $# -lt 3 ]; then
    echo "usage: $0 <build-dir> <package-name> <binary>..." >&2
    exit 2
fi

build_dir=$1; shift
pkg=$1; shift
binaries=("$@")

repo_root=$(cd "$(dirname "$0")/../.." && pwd)

rm -rf "$pkg"
mkdir -p "$pkg"

for binary in "${binaries[@]}"; do
    cp "$build_dir/src/$binary" "$pkg/"
done

cp "$repo_root/gpl-3.0.txt" "$pkg/"

if [ -f "$pkg/museld.exe" ]; then
    mkdir -p "$pkg/shaders" "$pkg/fonts"
    cp "$build_dir/src/shaders"/*.spv "$pkg/shaders/"
    cp "$build_dir/src/fonts/NotoSansJP-Regular.ttf" "$pkg/fonts/"
    cp "$repo_root/packaging/windows/README-museld.txt" "$pkg/README.txt"
else
    cp "$repo_root/packaging/windows/README-ac3rf-efm-decode.txt" "$pkg/README.txt"
fi

# Copy the UCRT64 DLLs the binaries need, transitively. The test is whether
# /ucrt64/bin provides the imported name, not where ldd resolved it: Windows
# ships some of the same names itself (onnxruntime.dll, from Windows ML, lives
# in System32), and System32 comes before PATH in the loader's search order, so
# such an import resolves to the system copy while MSYS2's is the one the
# binary was linked against and must travel with the package. Names /ucrt64/bin
# does not provide are Windows system DLLs and are left to the target machine.
changed=1
while [ "$changed" = 1 ]; do
    changed=0
    for f in "$pkg"/*.exe "$pkg"/*.dll; do
        [ -f "$f" ] || continue
        for dll in $(ldd "$f" | awk '{print $1}'); do
            if [ -f "/ucrt64/bin/$dll" ] && [ ! -f "$pkg/$dll" ]; then
                cp "/ucrt64/bin/$dll" "$pkg/"
                changed=1
            fi
        done
    done
done

# onnxruntime loads its provider bridge with LoadLibrary at runtime, which ldd
# cannot see.
if [ -f "$pkg/onnxruntime.dll" ] && [ -f /ucrt64/bin/onnxruntime_providers_shared.dll ]; then
    cp /ucrt64/bin/onnxruntime_providers_shared.dll "$pkg/"
fi

# The Vulkan loader normally arrives with the graphics driver, so it is not among
# the DLLs resolved above. Ship it too: it finds the driver through the registry
# either way, and museld then starts far enough to report a missing Vulkan device
# itself instead of failing to load.
if [ -f "$pkg/museld.exe" ]; then
    cp /ucrt64/bin/vulkan-1.dll "$pkg/"
fi

zip -qr "$pkg.zip" "$pkg"

echo "$pkg: $(ls "$pkg"/*.dll | wc -l) DLLs, $(du -sh "$pkg" | cut -f1) unpacked, $(du -h "$pkg.zip" | cut -f1) zipped"
