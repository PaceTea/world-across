#!/bin/bash
# make_windows_package.sh -- your own Windows build of World Across as one folder and one .zip,
# to copy to your own Windows machine.
#
# The package holds the translated game (WorldAcross.exe) and the card's code.bin: as little yours
# to hand on as the cartridge itself. Make it from your own dump, keep it on your own machines, and
# do not share it.
#
# Needs a finished Windows build (build-win, see the README's Windows section) and 7z or Python to
# write the .zip.
#
#   tools/make_windows_package.sh   -> build-win/World-Across-Windows/ and build-win/World-Across-Windows.zip
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build="$root/build-win"
name="World-Across-Windows"
out="$build/$name"

die() { echo "make_windows_package: $*" >&2; exit 1; }
[ -f "$build/albw-boot.exe" ] || die "no build-win/albw-boot.exe yet; build it first (see the README)"
[ -f "$build/SDL3.dll" ] || die "no SDL3.dll beside it: the build has no window (SDL3's MinGW files missing)"
[ -f "$root/rom-extract/code.bin" ] || die "no rom-extract/code.bin: extract your card first"
strip=x86_64-w64-mingw32-strip
command -v "$strip" > /dev/null || strip=strip

rm -rf -- "$out" "$out.zip"
mkdir -p "$out/rom-extract"
# Without its debugging information the program is a tenth of the size.
"$strip" -o "$out/WorldAcross.exe" "$build/albw-boot.exe"
cp "$build/SDL3.dll" "$out/"
cp "$root/rom-extract/code.bin" "$out/rom-extract/"
cp "$root/LICENSE" "$out/LICENSE.txt"
cp "$root/third_party/fsr1/license.txt" "$out/FSR1-LICENSE.txt"
for sdl in "$root"/external/SDL3-*/LICENSE.txt; do
    [ -f "$sdl" ] && cp "$sdl" "$out/SDL3-LICENSE.txt"
done
# Plain text with Windows line ends, for Notepad.
sed 's/$/\r/' > "$out/README.txt" <<'TEXT'
World Across -- your own build, for your own machines

Start WorldAcross.exe. The first time, choose your card image (a decrypted dump of your own
cartridge) with Choose card; it is remembered.

It needs Windows 10 or later. The graphics card draws the game through Vulkan, which every
current AMD, NVIDIA and Intel driver provides; without it the processor draws instead.

Settings and saves are kept in %APPDATA%\albw-port. If something goes wrong, the log of the last
run is %APPDATA%\albw-port\last-run.log.

This folder holds the game's code, translated, and the card's code.bin. It is yours alone: do not
share it.
TEXT

if command -v 7z > /dev/null; then
    (cd "$build" && 7z a -tzip -mx=9 "$name.zip" "$name" > /dev/null)
else
    python3 -I -c 'import shutil, sys; shutil.make_archive(sys.argv[1], "zip", sys.argv[2], sys.argv[3])' \
        "$out" "$build" "$name"
fi
echo "make_windows_package: $out.zip ($(du -h "$out.zip" | cut -f1)) -- for your own machines; do not share it"
