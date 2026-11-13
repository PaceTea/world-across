#!/bin/bash
# profile_report.sh <profile> [count] [function] -- read what ALBW_PROFILE wrote
#
# Without a function: the hottest functions, by samples and share, after the libraries. With one:
# that function's hottest source lines. The profile holds offsets into the executable; addr2line
# turns them into names with the binary's own debug information.
#
#   ALBW_PROFILE=prof.txt ALBW_PROFILE_FROM=1000 ... build/albw-boot
#   tools/profile_report.sh prof.txt 30
#   tools/profile_report.sh prof.txt 20 shade_pixel
set -u
profile=$1
count=${2:-25}
function=${3:-}
root=$(cd "$(dirname "$0")/.." && pwd)
binary="$root/build/albw-boot"

work=$(mktemp -d "${TMPDIR:-/tmp}/albw-profile.XXXXXX") || exit 2
trap 'rm -rf -- "$work"' EXIT
head -1 "$profile"
grep -v '^#\|^lib:' "$profile" > "$work/pcs"
awk '{ print "0x" $1 }' "$work/pcs" | addr2line -f -e "$binary" 2>/dev/null | paste - - > "$work/names"

if [ -z "$function" ]; then
    grep '^lib:' "$profile" | awk '{ c[$1] += $2 } END { for (k in c) print c[k], k }' \
        | sort -nr | head -5
    paste <(awk '{ print $2 }' "$work/pcs") "$work/names" \
        | awk '{ c[$2] += $1; t += $1 } END { for (f in c) printf "%7d %5.1f%% %s\n", c[f], 100 * c[f] / t, f }' \
        | sort -nr | head -"$count"
else
    paste <(awk '{ print $2 }' "$work/pcs") "$work/names" \
        | awk -v fn="$function" '$2 == fn { sub(/ .*/, "", $3); c[$3] += $1 } END { for (l in c) print c[l], l }' \
        | sort -nr | head -"$count" \
        | while read -r n where; do
              file=${where%%:*}
              line=${where##*:}
              text=""
              [ -f "$file" ] && text=$(sed -n "${line}p" "$file" | cut -c1-100)
              printf "%7s %s:%s  %s\n" "$n" "${file##*/}" "$line" "$text"
          done
fi
