#!/bin/bash
#
# Check a staged or merged macOS package: that nothing in it still depends on
# Homebrew, that every binary really carries both architectures, and that the
# programs run.
#
#     smoke-test.sh <package> [--universal]
#
# Unlike on Windows there is no PATH to strip: a Mach-O file names the exact path
# of every library it loads, so the check is to read those names back and insist
# that none of them point outside the package.

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <package> [--universal]" >&2
    exit 2
fi

pkg=$1
universal=${2:-}

cd "$pkg"

macho_files() {
    find . -type f \( -perm -u+x -o -name '*.dylib' \) -print | while read -r f; do
        if file -b "$f" | grep -q "Mach-O"; then echo "$f"; fi
    done
}

status=0

for f in $(macho_files); do
    if otool -L "$f" | tail -n +2 | grep -qE "/opt/homebrew|/usr/local/(opt|Cellar)"; then
        echo "FAIL: $f still loads libraries from Homebrew:"
        otool -L "$f" | grep -E "/opt/homebrew|/usr/local/(opt|Cellar)" | sed 's/^/    /'
        status=1
    fi

    if [ "$universal" = "--universal" ]; then
        archs=$(lipo -archs "$f")
        case "$archs" in
            *arm64*) ;;
            *) echo "FAIL: $f has no arm64 slice (only: $archs)"; status=1 ;;
        esac
        case "$archs" in
            *x86_64*) ;;
            *) echo "FAIL: $f has no x86_64 slice (only: $archs)"; status=1 ;;
        esac
    fi
done

[ "$status" = 0 ] || exit 1

if [ -f ac3rf-efm-decode ]; then
    ./ac3rf-efm-decode --version
fi

if [ -f museld ]; then
    # museld's --help exits non-zero, so check that it printed its usage; a
    # package missing a library never gets as far as printing anything. --help
    # returns before a Vulkan device is needed, so this works without a GPU.
    ./museld --help > help.txt 2>&1 || true
    grep -q "^usage: museld" help.txt
    rm help.txt
fi

echo "$pkg: self-contained${universal:+, universal}, and the binaries run"
