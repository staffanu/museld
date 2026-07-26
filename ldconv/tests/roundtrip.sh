#!/bin/bash
# Copyright 2026 Staffan Ulfberg
# This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)
#
# Round-trip checks for ldconv.  Every conversion here is lossless, so each one
# has to come back byte for byte; anything else is a bug.
#
# Usage: roundtrip.sh /path/to/ldconv

set -u

LDCONV="${1:-ldconv}"
if [ ! -x "$LDCONV" ]; then
    echo "usage: $0 /path/to/ldconv" >&2
    exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

failures=0
check() {
    local name="$1" a="$2" b="$3"
    if cmp -s "$a" "$b"; then
        echo "ok    $name"
    else
        echo "FAIL  $name ($a != $b)"
        failures=$((failures + 1))
    fi
}

run() { "$LDCONV" -q -f "$@"; }

# 2 MiB of noise is a valid .lds: every bit pattern is a legal 10-bit sample.
head -c 2097150 /dev/urandom > "$WORK/src.lds"

# --- the lossless paths ------------------------------------------------------
run "$WORK/src.lds" "$WORK/a.s16"
run "$WORK/a.s16"   "$WORK/a.lds"
check "lds -> s16 -> lds" "$WORK/src.lds" "$WORK/a.lds"

run "$WORK/src.lds" "$WORK/b.ldf"
run "$WORK/b.ldf"   "$WORK/b.lds"
check "lds -> ldf -> lds" "$WORK/src.lds" "$WORK/b.lds"

run "$WORK/src.lds" "$WORK/c.flac"
run "$WORK/c.flac"  "$WORK/c.lds"
check "lds -> flac -> lds" "$WORK/src.lds" "$WORK/c.lds"

run "$WORK/src.lds" "$WORK/d.r30"
run "$WORK/d.r30"   "$WORK/d.lds"
check "lds -> r30 -> lds" "$WORK/src.lds" "$WORK/d.lds"

run "$WORK/a.s16" "$WORK/e.ldf"
run "$WORK/e.ldf" "$WORK/e.s16"
check "s16 -> ldf -> s16" "$WORK/a.s16" "$WORK/e.s16"

run "$WORK/a.s16"   "$WORK/f.s16be"
run "$WORK/f.s16be" "$WORK/f.s16"
check "s16 -> s16be -> s16" "$WORK/a.s16" "$WORK/f.s16"

run "$WORK/a.s16" "$WORK/g.u16"
run "$WORK/g.u16" "$WORK/g.s16"
check "s16 -> u16 -> s16" "$WORK/a.s16" "$WORK/g.s16"

run "$WORK/a.s16" "$WORK/h.rf"
run "$WORK/h.rf"  "$WORK/h.s16"
check "s16 -> f32 -> s16" "$WORK/a.s16" "$WORK/h.s16"

# --- containers are recognized by their header, not their name ---------------
cp "$WORK/c.flac" "$WORK/native.ldf"   # native FLAC wearing the Ogg extension
run "$WORK/native.ldf" "$WORK/native.lds"
check "native FLAC named .ldf" "$WORK/src.lds" "$WORK/native.lds"

# --- pipes -------------------------------------------------------------------
run --input-format lds --output-format s16 - - < "$WORK/src.lds" > "$WORK/pipe.s16"
check "stdin -> stdout" "$WORK/a.s16" "$WORK/pipe.s16"

# --- ranges ------------------------------------------------------------------
# 1000 samples from sample 4000, against the same bytes cut out by hand.  Done
# with tail/head rather than dd, whose status=none is not portable.
run -s 4000 -n 1000 "$WORK/a.s16" "$WORK/range.s16"
tail -c +8001 "$WORK/a.s16" | head -c 2000 > "$WORK/range-expected.s16"
check "s16 range" "$WORK/range-expected.s16" "$WORK/range.s16"

# The same range out of the packed format, where the start is not on a group
# boundary (4001 % 4 = 1) and neither is the length.
run -s 4001 -n 1001 "$WORK/src.lds" "$WORK/range2.s16"
run -s 4001 -n 1001 "$WORK/a.s16"   "$WORK/range2-expected.s16"
check "lds range across groups" "$WORK/range2-expected.s16" "$WORK/range2.s16"

run -s 4001 -n 1001 --input-format s16 - "$WORK/range3.s16" < "$WORK/a.s16"
check "range on a pipe" "$WORK/range2-expected.s16" "$WORK/range3.s16"

# A FLAC seek lands on a frame boundary, so the samples between that boundary
# and the target have to be dropped.  Streams written through a pipe carry no
# sample count, which is how ld-compress makes them, so try both kinds.
run --input-format lds --output-format ldf  - "$WORK/nototal.ldf"  < "$WORK/src.lds"
run --input-format lds --output-format flac - "$WORK/nototal.flac" < "$WORK/src.lds"
for name in b.ldf c.flac nototal.ldf nototal.flac; do
    run -s 4001 -n 1001 "$WORK/$name" "$WORK/seek-$name.s16"
    check "seek in $name" "$WORK/range2-expected.s16" "$WORK/seek-$name.s16"
done

# --- against ld-decode's own tools, when they are installed ------------------
if command -v ld-lds-converter > /dev/null; then
    ld-lds-converter -u < "$WORK/src.lds" > "$WORK/ldd.s16"
    check "matches ld-lds-converter -u" "$WORK/ldd.s16" "$WORK/a.s16"

    ld-lds-converter -p < "$WORK/a.s16" > "$WORK/ldd.lds"
    check "matches ld-lds-converter -p" "$WORK/ldd.lds" "$WORK/a.lds"
else
    echo "skip  ld-lds-converter comparison (not installed)"
fi

if command -v ffmpeg > /dev/null; then
    ffmpeg -hide_banner -loglevel error -y -i "$WORK/b.ldf" -f s16le -c:a pcm_s16le "$WORK/ff.s16"
    check "ffmpeg reads our .ldf" "$WORK/a.s16" "$WORK/ff.s16"
else
    echo "skip  ffmpeg comparison (not installed)"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures test(s) failed"
    exit 1
fi
echo "all tests passed"
