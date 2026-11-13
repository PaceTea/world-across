#!/usr/bin/env bash
# catch_up.sh -- translate what a play session ran into, and rebuild
#
# When the game reaches code this build has not translated, the port stops and appends the address
# (and the rest of the function table it came from) to missing_entries.txt beside the saves. This
# folds those addresses into entries/<product code>.txt, retranslates and rebuilds; then start the
# game again and load your save.
#
# The save directory is the port's: $ALBW_SAVE_DIR, else $XDG_DATA_HOME/albw-port, else
# ~/.local/share/albw-port. Only missing_entries.txt is read there, and it is removed once the
# build has succeeded; nothing else in the directory is touched.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIR=${ALBW_SAVE_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/albw-port}
MISSING=$DIR/missing_entries.txt
PRODUCT=$(cat "$ROOT/rom-extract/product_code.txt" 2>/dev/null)
if [[ -z $PRODUCT ]]; then
    echo "no rom-extract/product_code.txt: run tools/extract_rom.py on the card again" >&2
    exit 1
fi
ENTRIES=$ROOT/entries/$PRODUCT.txt

if [[ ! -s $MISSING ]]; then
    echo "nothing to catch up on: $MISSING is empty or not there"
    exit 0
fi

cd "$ROOT" || exit 1
mkdir -p entries
touch "$ENTRIES"
before=$(sort -u "$ENTRIES" | wc -l)
merged=$(mktemp)
cat "$MISSING" "$ENTRIES" | LC_ALL=C sort -u > "$merged"
mv "$merged" "$ENTRIES"
after=$(wc -l < "$ENTRIES")
echo "entry points: $before -> $after"

# The new entry points go into the function database first, so their bodies are walked like every
# other and whatever they call is found along with them.
if ! python3 tools/find_functions.py --known "$ENTRIES" > /dev/null; then
    echo "finding functions failed; $MISSING is kept" >&2
    exit 1
fi
python3 tools/recompile.py --all --split 16 2>&1 | grep -E 'fully translated' | sed 's/^/  /'
if ! ninja -C build albw-boot > /dev/null; then
    echo "the build failed; $MISSING is kept" >&2
    ninja -C build albw-boot 2>&1 | grep -E 'error' | head -5 >&2
    exit 1
fi
rm -f -- "$MISSING"
echo "done: start the game again"
