#!/bin/bash
# make_appimage.sh -- your own build of World Across as one AppImage, for your own machines.
#
# The AppImage holds build/albw-boot and rom-extract/code.bin, and those are the game's code --
# translated, and as it is on the card: as little yours to hand on as the cartridge itself. Make
# it from your own dump, keep it on your own machines, and do not share it. Like the build, it
# reads the card image at runtime: give its path once (the launcher asks for it, or start the
# AppImage with it), and it is remembered.
#
# It bundles SDL3, which many distributions do not have yet, and DLSS's library where the build has
# DLSS; the Vulkan loader and the C and C++ libraries stay the system's, since they have to match
# its drivers. So it runs where the build would: on this distribution and on any with a C library
# at least as new.
#
# Needs a finished build (see the README), mksquashfs (squashfs-tools), and the AppImage runtime --
# AppImage's own type 2 runtime, runtime-x86_64 from github.com/AppImage/type2-runtime -- named by
# APPIMAGE_RUNTIME or placed at build/appimage/runtime-x86_64. This script downloads nothing.
#
#   tools/make_appimage.sh        -> build/World_Across-x86_64.AppImage
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
bin="$root/build/albw-boot"
work="$root/build/appimage"
runtime=${APPIMAGE_RUNTIME:-$work/runtime-x86_64}
out="$root/build/World_Across-x86_64.AppImage"

die() { echo "make_appimage: $*" >&2; exit 1; }
[ -x "$bin" ] || die "no build/albw-boot yet; build it first (see the README)"
command -v mksquashfs > /dev/null || die "mksquashfs is missing (package squashfs-tools)"
[ -f "$runtime" ] || die "no AppImage runtime at $runtime; get runtime-x86_64 from
    https://github.com/AppImage/type2-runtime/releases (continuous) and put it there, or name it
    with APPIMAGE_RUNTIME"

code="$root/rom-extract/code.bin"
[ -f "$code" ] || die "no rom-extract/code.bin: extract your card first (see the README)"

appdir="$work/AppDir"
rm -rf -- "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib" "$appdir/usr/rom-extract" "$appdir/usr/share/applications" \
         "$appdir/usr/share/icons/hicolor/256x256/apps" "$appdir/usr/share/doc/world-across"
cp "$bin" "$appdir/usr/bin/albw-boot"
# The card's code, which the program loads beside the translation of it: it looks for it at
# ../rom-extract/code.bin from where it is.
cp "$code" "$appdir/usr/rom-extract/code.bin"

# SDL3, as the build found it.
sdl=$(ldd "$bin" | awk '/libSDL3\.so/ { print $3 }')
[ -n "$sdl" ] && [ -f "$sdl" ] || die "the build does not link SDL3: there is no window to package"
cp -L "$sdl" "$appdir/usr/lib/$(basename "$sdl")"

# DLSS's library, beside the program where it looks for it, with NVIDIA's licence.
for dlss in "$root"/external/dlss-sdk-*/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.*; do
    [ -f "$dlss" ] || continue
    cp "$dlss" "$appdir/usr/bin/"
    sdk=${dlss%/lib/Linux_x86_64/rel/*}
    [ -f "$sdk/LICENSE.txt" ] && cp "$sdk/LICENSE.txt" "$appdir/usr/share/doc/world-across/NVIDIA-DLSS-LICENSE.txt"
done

cp "$root/LICENSE" "$appdir/usr/share/doc/world-across/LICENSE"
cp "$root/third_party/fsr1/license.txt" "$appdir/usr/share/doc/world-across/FSR1-LICENSE.txt"
cp "$root/tools/appimage/world-across.desktop" "$appdir/world-across.desktop"
cp "$root/tools/appimage/world-across.desktop" "$appdir/usr/share/applications/"
cp "$root/tools/appimage/world-across.png" "$appdir/world-across.png"
cp "$root/tools/appimage/world-across.png" "$appdir/usr/share/icons/hicolor/256x256/apps/"
ln -s world-across.png "$appdir/.DirIcon"

cat > "$appdir/AppRun" <<'APPRUN'
#!/bin/sh
# The program, with the libraries that came with it found first.
here=$(dirname "$(readlink -f "$0")")
export LD_LIBRARY_PATH="$here/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$here/usr/bin/albw-boot" "$@"
APPRUN
chmod +x "$appdir/AppRun"

# The AppImage: the runtime, and the directory as a SquashFS image right behind it.
rm -f -- "$work/AppDir.squashfs"
mksquashfs "$appdir" "$work/AppDir.squashfs" -root-owned -noappend -comp zstd -quiet -no-progress
cat "$runtime" "$work/AppDir.squashfs" > "$out"
chmod +x "$out"
rm -f -- "$work/AppDir.squashfs"
echo "make_appimage: $out ($(du -h "$out" | cut -f1)) -- for your own machines; do not share it"
