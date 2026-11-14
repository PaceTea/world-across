#!/bin/bash
# frames_check.sh save|check -- did a renderer change change the picture?
#
# Renders the title screen's frames 1000 to 1100 and keeps the screens of frames 1050 and 1100.
# `save` makes them the reference; `check` compares a new run with it, pixel for pixel. Every
# speed-up of the rasteriser is meant to leave the frames exactly as they were, so "identical" is
# the only answer that passes. A change that is meant to alter the picture saves a new reference
# after its frames have been looked at.
#
# The frames are drawn from the card image, so they stay in build/, which git ignores. Each run
# formats a save of its own in a directory made for it and removed afterwards; the save directory
# a player uses is never touched.
#
#   ALBW_ROM=/path/to/card.3ds tools/frames_check.sh save
#   ALBW_ROM=/path/to/card.3ds tools/frames_check.sh check
set -u
mode=${1:-check}
root=$(cd "$(dirname "$0")/.." && pwd)
ref="$root/build/frames-ref"
new="$root/build/frames-new"
if [ -z "${ALBW_ROM:-}" ]; then
    echo "frames_check: set ALBW_ROM to the card image" >&2
    exit 2
fi

# ALBW_BOOT names another program to run -- world-across, say, with the game it built from the
# card -- in place of build/albw-boot, or of build-win/albw-boot.exe under Wine.
# ALBW_WINE=1 runs the Windows build (build-win/albw-boot.exe, see the README) under Wine instead,
# against the same reference: the two have to play the same game. Wine's own WINEPREFIX applies.
if [ -n "${ALBW_WINE:-}" ]; then
    boot=(wine "${ALBW_BOOT:-$root/build-win/albw-boot.exe}")
    winpath() { printf 'Z:%s' "$(realpath -m "$1")"; }
    export WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-mscoree,mshtml=}
else
    boot=("${ALBW_BOOT:-$root/build/albw-boot}")
    winpath() { printf '%s' "$1"; }
fi

save=$(mktemp -d "${TMPDIR:-/tmp}/albw-frames.XXXXXX") || exit 2
trap 'rm -rf -- "$save"' EXIT
rm -rf -- "$new"
mkdir -p "$new"
env ALBW_ROM="$(winpath "$ALBW_ROM")" ALBW_HEADLESS=1 ALBW_SAVE_DIR="$(winpath "$save")" \
    ALBW_RENDER=101 ALBW_RENDER_FROM=1000 ALBW_STOP_AT=1101 \
    ALBW_DUMP_FRAMES="$(winpath "$new")" ALBW_DUMP_FROM=1050 ALBW_DUMP_EVERY=50 \
    timeout 900 "${boot[@]}" > "$new/run.log" 2>&1

frames="frame-001050-top frame-001050-bottom frame-001100-top frame-001100-bottom"
for f in $frames; do
    if [ ! -f "$new/$f.ppm" ]; then
        echo "frames_check: the run wrote no $f; see $new/run.log" >&2
        exit 1
    fi
done

if [ "$mode" = save ]; then
    rm -rf -- "$ref"
    mkdir -p "$ref"
    for f in $frames; do cp "$new/$f.ppm" "$ref/"; done
    echo "frames_check: saved the reference in $ref"
    exit 0
fi

if [ ! -d "$ref" ]; then
    echo "frames_check: no reference yet; run with 'save' on a build whose frames are right" >&2
    exit 2
fi
status=0
for f in $frames; do
    if cmp -s "$ref/$f.ppm" "$new/$f.ppm"; then
        echo "$f identical"
    else
        echo "$f DIFFERS"
        status=1
    fi
done
exit $status
