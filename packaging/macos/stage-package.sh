#!/bin/bash
#
# Stage a macOS package for the architecture of the machine running this.
#
#     stage-package.sh <build-dir> <package-dir> <binary>...
#
# The binaries are taken from <build-dir>/src, together with everything they need
# to run on a Mac that has no Homebrew: every dylib they resolve to under the
# Homebrew prefix, rewritten to load from the package itself, and — for museld —
# the SPIR-V shaders and subtitle font it loads from its own directory, plus the
# MoltenVK driver the Vulkan loader needs to find Metal.
#
# Two of these trees, one per architecture, are combined by merge-universal.sh.

set -euo pipefail

if [ $# -lt 3 ]; then
    echo "usage: $0 <build-dir> <package-dir> <binary>..." >&2
    exit 2
fi

build_dir=$1; shift
pkg=$1; shift
binaries=("$@")

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
brew_prefix=$(brew --prefix)

rm -rf "$pkg"
mkdir -p "$pkg/lib"

for binary in "${binaries[@]}"; do
    cp "$build_dir/src/$binary" "$pkg/"
done

cp "$repo_root/gpl-3.0.txt" "$pkg/"

# A dependency that lives under Homebrew has to travel with us; anything else is
# part of macOS and stays where it is.
is_bundled_dep() {
    case "$1" in
        "$brew_prefix"/*|/opt/homebrew/*|/usr/local/opt/*|/usr/local/Cellar/*) return 0 ;;
        *) return 1 ;;
    esac
}

deps_of() {
    otool -L "$1" | tail -n +2 | awk '{print $1}'
}

macho_files() {
    find "$pkg" -type f \( -perm -u+x -o -name '*.dylib' \) -print | while read -r f; do
        if file -b "$f" | grep -q "Mach-O"; then echo "$f"; fi
    done
}

if [ -f "$pkg/museld" ]; then
    # museld runs through a launcher that points the Vulkan loader at the driver
    # below, so the real binary moves aside. It still finds shaders/ and fonts/
    # next to itself, since both sit in the package root either way.
    mv "$pkg/museld" "$pkg/museld-bin"
    cp "$repo_root/packaging/macos/museld-launcher.sh" "$pkg/museld"
    chmod +x "$pkg/museld"

    mkdir -p "$pkg/shaders" "$pkg/fonts" "$pkg/vulkan/icd.d"
    cp "$build_dir/src/shaders"/*.spv "$pkg/shaders/"
    cp "$build_dir/src/fonts/NotoSansJP-Regular.ttf" "$pkg/fonts/"
    cp "$repo_root/packaging/macos/README-museld.txt" "$pkg/README.txt"

    # MoltenVK implements Vulkan on top of Metal. The loader discovers it through
    # this manifest, whose library_path we make relative to the manifest itself so
    # that it resolves inside the package wherever the user unpacks it.
    molten_prefix=$(brew --prefix molten-vk)
    cp "$molten_prefix/lib/libMoltenVK.dylib" "$pkg/lib/"
    chmod u+w "$pkg/lib/libMoltenVK.dylib"
    sed -E 's|"library_path"[[:space:]]*:[[:space:]]*"[^"]*"|"library_path": "../../lib/libMoltenVK.dylib"|' \
        "$molten_prefix/share/vulkan/icd.d/MoltenVK_icd.json" > "$pkg/vulkan/icd.d/MoltenVK_icd.json"
    grep -q '\.\./\.\./lib/libMoltenVK\.dylib' "$pkg/vulkan/icd.d/MoltenVK_icd.json" \
        || { echo "failed to point the MoltenVK manifest at the bundled driver" >&2; exit 1; }
else
    cp "$repo_root/packaging/macos/README-ac3rf-efm-decode.txt" "$pkg/README.txt"
fi

# Copy the Homebrew dylibs the binaries need, transitively.
changed=1
while [ "$changed" = 1 ]; do
    changed=0
    for f in $(macho_files); do
        for dep in $(deps_of "$f"); do
            if is_bundled_dep "$dep"; then
                base=$(basename "$dep")
                if [ ! -f "$pkg/lib/$base" ]; then
                    cp "$dep" "$pkg/lib/$base"
                    chmod u+w "$pkg/lib/$base"
                    changed=1
                fi
            fi
        done
    done
done

# Point every reference at the copy inside the package: dylibs find their
# neighbours through @loader_path, the executables through @executable_path.
# install_name_tool invalidates the signature, so each file is signed after it is
# rewritten — ad-hoc, which is all an unpaid certificate allows, and enough for
# the arm64 kernel to agree to run it at all.
for f in $(macho_files); do
    case "$f" in
        */lib/*.dylib)
            install_name_tool -id "@loader_path/$(basename "$f")" "$f"
            prefix="@loader_path"
            ;;
        *)
            prefix="@executable_path/lib"
            ;;
    esac
    for dep in $(deps_of "$f"); do
        if is_bundled_dep "$dep"; then
            install_name_tool -change "$dep" "$prefix/$(basename "$dep")" "$f"
        fi
    done
    codesign --force --sign - "$f"
done

echo "$pkg: $(ls "$pkg/lib" | wc -l | tr -d ' ') dylibs, $(du -sh "$pkg" | cut -f1) staged"
