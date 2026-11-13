#!/bin/bash
# frametime.sh [threads...] -- what a rendered frame costs, and where the time goes
#
# Renders the title screen's frames 1000 to 1199 once for each thread count given (default: 1, 4 and
# whatever the machine has) and prints the time per frame spent shading vertices, setting triangles
# up and rasterising, from the raster report's [time ] line. The run formats a save of its own in a
# directory made for it, as tools/frames_check.sh does.
#
#   ALBW_ROM=/path/to/card.3ds tools/frametime.sh 1 4 8
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${ALBW_ROM:-}" ]; then
    echo "frametime: set ALBW_ROM to the card image" >&2
    exit 2
fi
[ $# -eq 0 ] && set -- 1 4 "$(($(nproc) - 1))"
save=$(mktemp -d "${TMPDIR:-/tmp}/albw-frametime.XXXXXX") || exit 2
trap 'rm -rf -- "$save"' EXIT

for threads in "$@"; do
    line=$(env ALBW_HEADLESS=1 ALBW_SAVE_DIR="$save" ALBW_RENDER=200 ALBW_RENDER_FROM=1000 \
               ALBW_STOP_AT=1200 ALBW_RENDER_THREADS="$threads" \
               timeout 900 "$root/build/albw-boot" 2>&1 | grep -m1 '^\[time \]')
    if [ -z "$line" ]; then
        echo "threads $threads: the run printed no [time ] line"
        continue
    fi
    # The line holds five durations in seconds: shading shared out and alone, triangle setup,
    # rasterising shared out and alone. Two hundred frames, so seconds times five is ms a frame.
    echo "$line" | grep -oE '[0-9]+\.[0-9]+ s' | awk -v t="$threads" '
        { v[NR] = $1 * 5 }
        END { printf "threads %s: shading %.1f, setup %.1f, raster %.1f -> %.1f ms a frame\n",
                     t, v[1] + v[2], v[3], v[4] + v[5], v[1] + v[2] + v[3] + v[4] + v[5] }'
done
