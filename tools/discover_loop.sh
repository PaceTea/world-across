#!/usr/bin/env bash
# Close the discovery loop: run the recompiled guest, let it name the indirect targets
# that have no translated function, translate those, and repeat until none are left.
#
# Static discovery (tools/find_functions.py) finds the entry points that something in the image
# refers to, which is nearly all of them; what it cannot see is a pointer computed at runtime. An
# address the guest actually branches to is an entry point by construction, so the guest is the
# oracle for what is left, the same way Unicorn is the oracle for "was this translated correctly".
#
# Discovered addresses accumulate in entries/<product code>.txt, which is an input to every
# later build - delete it to start over.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PRODUCT=$(cat "$ROOT/rom-extract/product_code.txt" 2>/dev/null)
if [[ -z $PRODUCT ]]; then
    echo "no rom-extract/product_code.txt: run tools/extract_rom.py on the card again" >&2
    exit 1
fi
ENTRIES=$ROOT/entries/$PRODUCT.txt
MAX=${1:-20}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cd "$ROOT" || exit 1

# The boot path reads the RomFS now, and without it the guest raises a fatal error twelve
# syscalls in -- long before it reaches anything worth discovering. Better to say so than to
# report "no missing targets left".
if [[ -z ${ALBW_ROM:-} ]]; then
    echo "set ALBW_ROM to the card image first; the boot path needs its RomFS" >&2
    exit 1
fi

mkdir -p "$ROOT/entries"
touch "$ENTRIES"

# With ALBW_FRESH_SAVES=1 every run starts from an empty save directory, so that a scripted run
# (ALBW_PRESS) sees the same screens each round even after the game has written a save. Only a
# directory named by ALBW_SAVE_DIR is ever emptied -- never the default one, which holds real saves.
fresh_saves() {
    [[ ${ALBW_FRESH_SAVES:-} == 1 ]] || return 0
    if [[ -z ${ALBW_SAVE_DIR:-} ]]; then
        echo "ALBW_FRESH_SAVES needs ALBW_SAVE_DIR: the default save directory is never emptied" >&2
        exit 1
    fi
    rm -rf "${ALBW_SAVE_DIR:?}"/archive-*
}

for ((round = 1; round <= MAX; round++)); do
    before=$(sort -u "$ENTRIES" | wc -l)
    fresh_saves

    ALBW_DISCOVER="$WORK/found.txt" ALBW_DISCOVER_CAP=20000 \
        timeout 60 ./build/albw-boot rom-extract/code.bin > "$WORK/run.log" 2>&1
    status=$?
    svc=$(grep -cE '^\[svc ' "$WORK/run.log")

    if [[ ! -s "$WORK/found.txt" ]]; then
        echo "round $round: no missing targets left, $svc syscalls (exit $status)"
        tail -3 "$WORK/run.log" | sed 's/^/  /'
        exit 0
    fi

    cat "$WORK/found.txt" "$ENTRIES" | LC_ALL=C sort -u > "$WORK/merged.txt"
    mv "$WORK/merged.txt" "$ENTRIES"
    after=$(wc -l < "$ENTRIES")

    echo "round $round: $svc syscalls, entries $before -> $after"
    if (( after == before )); then
        echo "  no new entries; the loop cannot make progress this way"
        sort -u "$WORK/found.txt" | head -20 | sed 's/^/    still missing: /'
        exit 1
    fi

    python3 tools/find_functions.py --known "$ENTRIES" > "$WORK/find.log" 2>&1 || {
        echo "  FINDING FUNCTIONS FAILED"; tail -3 "$WORK/find.log" | sed 's/^/    /'; exit 1; }
    python3 tools/recompile.py --all --split 16 2>&1 | grep -E 'fully translated' | sed 's/^/  /'

    ninja -C build albw-boot > "$WORK/build.log" 2>&1 || {
        echo "  BUILD FAILED"
        grep -E 'error:|undefinierter' "$WORK/build.log" | head -5 | sed 's/^/    /'
        exit 1
    }
done

echo "stopped after $MAX rounds"
