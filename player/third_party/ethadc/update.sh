#!/bin/sh
# Refresh the vendored ethadc stream headers from a checkout of the ethadc
# repository (default: ../../../ethadc relative to this directory).  The copy
# is verbatim; VERSION.txt records the commit it came from.  (Not "VERSION": on
# a case-insensitive filesystem that name answers to #include <version>.)
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=${1:-$here/../../../../ethadc}
for f in Protocol.h Net.h ByteRing.h StreamReassembler.h StreamReceiver.h; do
    cp "$src/stream/$f" "$here/$f"
done
{
    echo "ethadc stream headers, copied verbatim from $(git -C "$src" remote get-url origin 2>/dev/null || echo "$src")"
    echo "commit $(git -C "$src" rev-parse HEAD)$(git -C "$src" diff --quiet HEAD -- stream || echo ' (with uncommitted changes)')"
    echo "on $(date -u +%Y-%m-%dT%H:%MZ)"
} > "$here/VERSION.txt"
cat "$here/VERSION.txt"
