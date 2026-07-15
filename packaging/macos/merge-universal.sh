#!/bin/bash
#
# Combine two staged trees, one per architecture, into a universal one.
#
#     merge-universal.sh <arm64-package> <x86_64-package> <output-package>
#
# Homebrew only ever has libraries for the architecture of the machine it is on,
# so each architecture is built and staged on its own runner and the results are
# joined here: every Mach-O file that exists in both trees becomes one file with
# both slices, and everything else — shaders, font, manifest, launcher — is taken
# as it is, being identical either way.

set -euo pipefail

if [ $# -ne 3 ]; then
    echo "usage: $0 <arm64-package> <x86_64-package> <output-package>" >&2
    exit 2
fi

arm=$1
x86=$2
out=$3

for d in "$arm" "$x86"; do
    [ -d "$d" ] || { echo "$0: no such staged package: $d" >&2; exit 1; }
done

is_macho() {
    file -b "$1" | grep -q "Mach-O"
}

rm -rf "$out"
cp -R "$arm" "$out"

merged=0
skipped=0
while IFS= read -r -d '' f; do
    rel="${f#"$out"/}"
    counterpart="$x86/$rel"

    if [ ! -f "$counterpart" ]; then
        if is_macho "$f"; then
            echo "$0: $rel exists only for arm64" >&2
            exit 1
        fi
        continue
    fi

    if is_macho "$f" && is_macho "$counterpart"; then
        lipo -create "$f" "$counterpart" -output "$f.universal"
        mv "$f.universal" "$f"
        # lipo throws the signatures away along with the old file.
        codesign --force --sign - "$f"
        merged=$((merged + 1))
    else
        skipped=$((skipped + 1))
    fi
done < <(find "$out" -type f -print0)

echo "$out: $merged Mach-O files merged, $skipped other files passed through"
