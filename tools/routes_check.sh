#!/bin/bash
# routes_check.sh save|check -- does the game still play the same?
#
# frames_check.sh looks at the title screen. This plays the game, headless, along four routes
# whose button presses are in tools/routes/:
#
#   opening.press  a new file from the title screen: the name, the opening, Link's house, outside,
#                  the weather vane, and saving there (frame 14500 is the end of it);
#   gulley.press   from the save the opening made: load it, walk west to Gulley outside the
#                  blacksmith's (his line is on screen at frame 4300), and on to the river (6300);
#   items.press    from the same save: touch Items on the bottom screen. The menu opens over a
#                  picture the game takes of its top screen with a texture copy (frame 2700);
#   quit.press     from the same save: pause, quit, and confirm; the title screen comes back, the
#                  file select (4250) loads the save again, and Link stands at the vane (5300).
#
# The screens at those frames and the save files are kept. `save` makes them the reference;
# `check` plays again and compares, byte for byte. The emulation is deterministic, so a change to
# the translation, the kernel or the function database that is meant to leave the game as it was
# must come out identical -- and one that is meant to change it gets looked at and saved anew.
# A route that runs into code the build has not translated fails either way.
#
# Everything stays in build/, which git ignores; each route's saves live in a directory made for
# it, never the player's.
#
#   ALBW_ROM=/path/to/card.3ds tools/routes_check.sh save
#   ALBW_ROM=/path/to/card.3ds tools/routes_check.sh check
set -u
mode=${1:-check}
root=$(cd "$(dirname "$0")/.." && pwd)
ref="$root/build/routes-ref"
new="$root/build/routes-new"
if [ -z "${ALBW_ROM:-}" ]; then
    echo "routes_check: set ALBW_ROM to the card image" >&2
    exit 2
fi

rm -rf -- "$new"
mkdir -p "$new"

# route <name> <press file> <render from> <frames to dump: first every last> <save dir>
route() {
    local name=$1 press=$2 from=$3 first=$4 every=$5 last=$6 save=$7
    mkdir -p "$new/$name"
    env ALBW_HEADLESS=1 ALBW_SAVE_DIR="$save" ALBW_PRESS="$(cat "$root/tools/routes/$press")" \
        ALBW_RENDER=$((last - from + 1)) ALBW_RENDER_FROM="$from" \
        ALBW_DUMP_FRAMES="$new/$name" ALBW_DUMP_FROM="$first" ALBW_DUMP_EVERY="$every" \
        ALBW_STOP_AT=$((last + 1)) timeout 1500 "$root/build/albw-boot" > "$new/$name/run.log" 2>&1
    if [ -s "$save/missing_entries.txt" ]; then
        echo "routes_check: the $name route ran into untranslated code at" \
             "$(tr '\n' ' ' < "$save/missing_entries.txt"); see $new/$name/run.log" >&2
        exit 1
    fi
    if ! ls "$new/$name"/frame-*.ppm > /dev/null 2>&1; then
        echo "routes_check: the $name route wrote no frames; see $new/$name/run.log" >&2
        exit 1
    fi
    cp -r "$save" "$new/$name/save"
}

work=$(mktemp -d "${TMPDIR:-/tmp}/albw-routes.XXXXXX") || exit 2
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/opening"
route opening opening.press 14400 14500 1000 14500 "$work/opening"
cp -r "$work/opening" "$work/gulley"
cp -r "$work/opening" "$work/items"
cp -r "$work/opening" "$work/quit"
route gulley gulley.press 4200 4300 2000 6300 "$work/gulley"
route items items.press 2600 2700 1000 2700 "$work/items"
route quit quit.press 4150 4250 1050 5300 "$work/quit"

if [ "$mode" = save ]; then
    rm -rf -- "$ref"
    cp -r "$new" "$ref"
    find "$ref" -name run.log -delete
    echo "routes_check: saved the reference in $ref"
    exit 0
fi

if [ ! -d "$ref" ]; then
    echo "routes_check: no reference yet; run with 'save' on a build that plays right" >&2
    exit 2
fi
status=0
for name in opening gulley items quit; do
    for f in "$ref/$name"/frame-*.ppm; do
        b=$(basename "$f" .ppm)
        if cmp -s "$f" "$new/$name/$b.ppm"; then
            echo "$name $b identical"
        else
            echo "$name $b DIFFERS"
            status=1
        fi
    done
    if diff -rq "$ref/$name/save" "$new/$name/save" > /dev/null; then
        echo "$name saves identical"
    else
        echo "$name saves DIFFER"
        diff -rq "$ref/$name/save" "$new/$name/save" | sed 's/^/  /'
        status=1
    fi
done
exit $status
