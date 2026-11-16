#!/bin/bash
# Mirror the curated clip collection from the NAS to this machine.
#
# Home network only: the NAS is mounted at /Volumes/video on the Macs at home and at
# /nas/video on multivac; anywhere else this script has nothing to copy from, and the
# regression suite's clips come from https://madeye.org/ld-captures/public/ on their own.
# See docs/captures.md.
set -e
for src in /Volumes/video/LD-Captures/clips /nas/video/LD-Captures/clips; do
    [ -d "$src" ] && break
    src=
done
if [ -z "$src" ]; then
    echo "captures-sync: the NAS is not mounted (it is reachable from the home network only)" >&2
    exit 1
fi
dst="${CAPTURES_DIR:-$HOME/data/LD-Captures}/clips"
mkdir -p "$dst"
exec rsync -a --info=progress2 "$@" "$src/" "$dst/"
