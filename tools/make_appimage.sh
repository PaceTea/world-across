#!/bin/bash
# make_appimage.sh -- World Across for Linux, as it is published: the launcher and the kit it makes the
# game with, as one AppImage.
#
# The AppImage holds nothing of the game. On its first start the launcher asks for the player's card
# and makes the game from it, on their machine (src/runtime/albw_builder.c): it reads the card's
# code, finds its functions, translates them to C and compiles them into libgame.so, in
# ~/.cache/albw-port/build. For that the kit brings the translating tools, Python with capstone and
# a C compiler -- GCC from musl-cross, cut down to what one C file needs -- so nothing has to be
# installed first.
#
# It is to start on any distribution with glibc 2.34 or later (Ubuntu 22.04, Debian 12, Fedora 35
# and newer). So SDL3 is built here as a static library, and it and the launcher are held to glibc
# 2.34 (tools/appimage/glibc_floor.py, glibc_compat.c); the Vulkan loader, the C and C++ libraries
# and the drivers stay the system's, as they have to. DLSS is left out: NVIDIA's library is not
# ours to hand on.
#
# What it bundles comes from external/kit/, as downloaded -- this script downloads nothing -- and is
# checked against the SHA-256 sums below before anything is taken from it:
#   SDL3-3.4.20.tar.gz              https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20
#   cpython-3.14.10+20261110-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz
#                                   https://github.com/astral-sh/python-build-standalone/releases/tag/20261110
#   capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl
#                                   https://pypi.org/project/capstone/5.0.9/#files
#   x86_64-unknown-linux-musl.tar.xz
#                                   https://github.com/cross-tools/musl-cross/releases/tag/20261001
#   runtime-x86_64                  https://github.com/AppImage/type2-runtime/releases (continuous)
#
# Needs cmake, ninja, gcc and g++, glslc (FSR 1 and the GPU renderer), SDL3's build dependencies
# (the X11, Wayland, libdecor, PipeWire, PulseAudio and ALSA headers), python3, objdump and
# mksquashfs (squashfs-tools).
#
#   tools/make_appimage.sh   -> build-appimage/World-Across-x86_64.AppImage
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work="$root/build-appimage"
downloads="$root/external/kit"
out="$work/World-Across-x86_64.AppImage"
floor=2.34

die() { echo "make_appimage: $*" >&2; exit 1; }
for tool in cmake ninja gcc g++ python3 objdump mksquashfs strip; do
    command -v "$tool" > /dev/null || die "$tool is missing"
done

sdl_src="$downloads/SDL3-3.4.20.tar.gz"
python_tgz="$downloads/cpython-3.14.10+20261110-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
capstone_whl="$downloads/capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl"
gcc_txz="$downloads/x86_64-unknown-linux-musl.tar.xz"
runtime="$downloads/runtime-x86_64"
sha256sum -c --quiet <<SUMS || die "a download in external/kit/ is missing or not the expected file"
f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f  $sdl_src
c2ff2e97422f5d3896a16e0317fd212ba977bbf2688d93a6f7a03d817506bd30  $python_tgz
273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e  $capstone_whl
faeaf3c7c377cb4351490988d9a1c8fed7d403a5b8066e5103af5d4dd96450fe  $gcc_txz
617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46  $runtime
SUMS
mkdir -p "$work"

# The newest glibc version an ELF file asks for.
newest_glibc() { objdump -T "$1" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1; }
# The toolchain's archive has read-only directories.
remove() { if [ -e "$1" ]; then chmod -R u+w "$1"; rm -rf -- "$1"; fi; }
above_floor() { [ -n "$1" ] && [ "$(printf '%s\n%s\n' "$1" "$floor" | sort -V | tail -1)" != "$floor" ]; }

# --- The launcher, held to glibc 2.34 -----------------------------------------------------------
header="$work/glibc_floor.h"
python3 "$root/tools/appimage/glibc_floor.py" "$floor" "$header" > /dev/null

# SDL3, static, built with the same header; again only when the header or the source changed.
sdl="$work/sdl3"
stamp=$(cat "$header" "$sdl_src" | sha256sum | cut -c1-16)
if [ "$(cat "$sdl/.stamp" 2>/dev/null)" != "$stamp" ]; then
    echo "make_appimage: building SDL3"
    rm -rf -- "$work/SDL3-3.4.20" "$work/sdl3-build" "$sdl"
    tar -xzf "$sdl_src" -C "$work"
    cmake -S "$work/SDL3-3.4.20" -B "$work/sdl3-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$sdl" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF \
        -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_C_FLAGS="-include $header" -DCMAKE_CXX_FLAGS="-include $header" \
        > "$work/sdl3-configure.log" 2>&1 || die "configuring SDL3 failed; see $work/sdl3-configure.log"
    for need in SDL_WAYLAND SDL_X11 SDL_PIPEWIRE SDL_PULSEAUDIO SDL_ALSA SDL_LIBUDEV SDL_VULKAN; do
        grep -qE "^--   $need +\(Wanted: ON\): ON" "$work/sdl3-configure.log" ||
            die "SDL3 would be built without $need: its development files are missing"
    done
    cmake --build "$work/sdl3-build" > "$work/sdl3-build.log" 2>&1 || die "building SDL3 failed; see $work/sdl3-build.log"
    cmake --install "$work/sdl3-build" > /dev/null
    echo "$stamp" > "$sdl/.stamp"
fi

echo "make_appimage: building the launcher"
launcher="$work/launcher"
cmake -S "$root" -B "$launcher" -G Ninja -DSDL3_DIR="$sdl/lib/cmake/SDL3" \
    -DALBW_DLSS_SDK="$work/no-dlss" -DCMAKE_C_FLAGS="-include $header" \
    -DCMAKE_CXX_FLAGS="-include $header" \
    -DALBW_LAUNCHER_EXTRA_SOURCES="$root/tools/appimage/glibc_compat.c" \
    > "$work/launcher-configure.log" 2>&1 || die "configuring failed; see $work/launcher-configure.log"
cmake --build "$launcher" --target world-across > "$work/launcher-build.log" 2>&1 ||
    die "building the launcher failed; see $work/launcher-build.log"
exe="$launcher/world-across"

# The program has to be the launcher, which carries no game: albw-boot, the developer's build, has
# the translation linked in and must never be packaged.
grep -q albw_game_connect "$exe" || die "$exe is not the launcher (world-across)"
[ "$(stat -c %s "$exe")" -lt 24000000 ] || die "$exe is too large to be the launcher"
if objdump -p "$exe" | grep -q 'NEEDED.*libSDL3'; then die "the launcher links a shared SDL3"; fi

# --- The AppDir ---------------------------------------------------------------------------------
appdir="$work/AppDir"
remove "$appdir"
remove "$work/kit-work"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" "$appdir/usr/share/icons/hicolor/256x256/apps" \
         "$work/kit-work"
doc="$appdir/usr/share/doc/world-across"
mkdir -p "$doc/licences"
strip -o "$appdir/usr/bin/world-across" "$exe"

kit="$appdir/usr/share/world-across/kit"
mkdir -p "$kit/tools" "$kit/entries" "$kit/include" "$kit/glue"
cp "$root/tools/extract_rom.py" "$root/tools/find_functions.py" "$root/tools/recompile.py" "$kit/tools/"
cp "$root"/entries/*.txt "$root"/entries/*.starts "$kit/entries/"
cp "$root/src/runtime/albw_cpu.h" "$root/src/runtime/albw_game.h" "$kit/include/"
cp "$root/src/runtime/albw_game_glue.c" "$kit/glue/"

# Python: the interpreter (the standard library built in) and the library, without what the tools
# never load -- Tk, IDLE, pip, the headers -- and without the compiled bytecode, which Python makes
# again as it goes. capstone goes into its site-packages.
tar -xzf "$python_tgz" -C "$work/kit-work"
py="$work/kit-work/python"
[ -x "$py/bin/python3.14" ] || die "the Python archive has no bin/python3.14"
mkdir -p "$kit/python/bin" "$kit/python/lib"
cp "$py/bin/python3.14" "$kit/python/bin/"
ln -s python3.14 "$kit/python/bin/python3"
cp -r "$py/lib/python3.14" "$kit/python/lib/"
lib="$kit/python/lib/python3.14"
rm -rf -- "$lib/idlelib" "$lib/tkinter" "$lib/turtledemo" "$lib/ensurepip" "$lib/pydoc_data" \
          "$lib/config-3.14-x86_64-linux-gnu" "$lib"/site-packages/pip* "$lib"/lib-dynload/_tkinter*
find "$kit/python" -name '__pycache__' -prune -exec rm -rf {} +
cp "$lib/LICENSE.txt" "$doc/licences/Python-LICENSE.txt"
mkdir -p "$doc/licences/Python-included"
cp "$root"/third_party/kit/python-linux/* "$doc/licences/Python-included/"
python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
    "$capstone_whl" "$work/kit-work/capstone"
cp -r "$work/kit-work/capstone/capstone" "$lib/site-packages/"
cp "$work/kit-work/capstone/capstone-5.0.9.dist-info/LICENSE.TXT" "$doc/licences/capstone-LICENSE.txt"

# GCC: the driver, the C compiler proper, the assembler and the linker, libgcc, and the C library's
# headers -- the closure of the standard headers, which is all the translation includes. The
# assembler and the linker go where the driver looks for them on its own, the target's bin/.
tar -xJf "$gcc_txz" -C "$work/kit-work"
target=x86_64-unknown-linux-musl
gccver=16.2.0
tc="$work/kit-work/$target"
gcc="$kit/gcc"
mkdir -p "$gcc/bin" "$gcc/libexec/gcc/$target/$gccver" "$gcc/lib/gcc/$target/$gccver" "$gcc/$target/bin" "$gcc/src"
cp "$tc/bin/$target-gcc" "$gcc/bin/gcc"
cp "$tc/libexec/gcc/$target/$gccver/cc1" "$tc/libexec/gcc/$target/$gccver/collect2" \
   "$tc/libexec/gcc/$target/$gccver/liblto_plugin.so" "$gcc/libexec/gcc/$target/$gccver/"
cp "$tc/$target/bin/as" "$tc/$target/bin/ld" "$gcc/$target/bin/"
cp "$tc/lib/gcc/$target/$gccver/libgcc.a" "$gcc/lib/gcc/$target/$gccver/"
while read -r header_file; do
    [ -n "$header_file" ] || continue
    mkdir -p "$gcc/$(dirname "$header_file")"
    cp "$tc/$header_file" "$gcc/$header_file"
done <<'HEADERS'
lib/gcc/x86_64-unknown-linux-musl/16.2.0/include/float.h
lib/gcc/x86_64-unknown-linux-musl/16.2.0/include/iso646.h
lib/gcc/x86_64-unknown-linux-musl/16.2.0/include/limits.h
lib/gcc/x86_64-unknown-linux-musl/16.2.0/include/stdalign.h
lib/gcc/x86_64-unknown-linux-musl/16.2.0/include/stdarg.h
lib/gcc/x86_64-unknown-linux-musl/16.2.0/include/stdatomic.h
lib/gcc/x86_64-unknown-linux-musl/16.2.0/include/stdbool.h
lib/gcc/x86_64-unknown-linux-musl/16.2.0/include/stddef.h
lib/gcc/x86_64-unknown-linux-musl/16.2.0/include/stdint.h
lib/gcc/x86_64-unknown-linux-musl/16.2.0/include/stdnoreturn.h
lib/gcc/x86_64-unknown-linux-musl/16.2.0/include/syslimits.h
x86_64-unknown-linux-musl/sysroot/usr/include/alloca.h
x86_64-unknown-linux-musl/sysroot/usr/include/assert.h
x86_64-unknown-linux-musl/sysroot/usr/include/bits/alltypes.h
x86_64-unknown-linux-musl/sysroot/usr/include/bits/errno.h
x86_64-unknown-linux-musl/sysroot/usr/include/bits/fenv.h
x86_64-unknown-linux-musl/sysroot/usr/include/bits/limits.h
x86_64-unknown-linux-musl/sysroot/usr/include/bits/setjmp.h
x86_64-unknown-linux-musl/sysroot/usr/include/bits/signal.h
x86_64-unknown-linux-musl/sysroot/usr/include/bits/stdint.h
x86_64-unknown-linux-musl/sysroot/usr/include/complex.h
x86_64-unknown-linux-musl/sysroot/usr/include/ctype.h
x86_64-unknown-linux-musl/sysroot/usr/include/errno.h
x86_64-unknown-linux-musl/sysroot/usr/include/features.h
x86_64-unknown-linux-musl/sysroot/usr/include/fenv.h
x86_64-unknown-linux-musl/sysroot/usr/include/inttypes.h
x86_64-unknown-linux-musl/sysroot/usr/include/limits.h
x86_64-unknown-linux-musl/sysroot/usr/include/locale.h
x86_64-unknown-linux-musl/sysroot/usr/include/math.h
x86_64-unknown-linux-musl/sysroot/usr/include/setjmp.h
x86_64-unknown-linux-musl/sysroot/usr/include/signal.h
x86_64-unknown-linux-musl/sysroot/usr/include/stdc-predef.h
x86_64-unknown-linux-musl/sysroot/usr/include/stdint.h
x86_64-unknown-linux-musl/sysroot/usr/include/stdio.h
x86_64-unknown-linux-musl/sysroot/usr/include/stdlib.h
x86_64-unknown-linux-musl/sysroot/usr/include/string.h
x86_64-unknown-linux-musl/sysroot/usr/include/strings.h
x86_64-unknown-linux-musl/sysroot/usr/include/tgmath.h
x86_64-unknown-linux-musl/sysroot/usr/include/time.h
x86_64-unknown-linux-musl/sysroot/usr/include/uchar.h
x86_64-unknown-linux-musl/sysroot/usr/include/wchar.h
x86_64-unknown-linux-musl/sysroot/usr/include/wctype.h
HEADERS
chmod -R u+w "$gcc"
# How it was built -- crosstool-NG's configuration, which names every component's version -- and
# the licences the toolchain carries, beside where their sources are (licences/SOURCES.txt).
cp "$tc/share/$target-ct-ng.config.bz2" "$gcc/src/"
mkdir -p "$doc/licences/compiler"
for part in binutils crosstool-ng gcc gmp isl mpc mpfr musl zlib zstd; do
    [ -d "$tc/share/licenses/$part" ] && cp -r "$tc/share/licenses/$part" "$doc/licences/compiler/"
done
chmod -R u+w "$doc"
cp "$root/third_party/kit/GPL-3.0.txt" "$root/third_party/kit/LGPL-3.0.txt" \
   "$root/third_party/kit/GCC-RUNTIME-LIBRARY-EXCEPTION.txt" "$doc/licences/"

cp "$root/LICENSE" "$doc/LICENSE"
cp "$root/third_party/fsr1/license.txt" "$doc/licences/FSR1-LICENSE.txt"
# The menu's type, where the launcher looks for it in an AppImage.
mkdir -p "$appdir/usr/share/world-across/fonts"
cp "$root"/third_party/fonts/*.ttf "$appdir/usr/share/world-across/fonts/"
cp "$root/third_party/fonts/OFL.txt" "$doc/licences/FiraSans-OFL.txt"
# The launcher's picture, by teethegrinder, beside the fonts.
mkdir -p "$appdir/usr/share/world-across/art"
cp "$root/third_party/art/launcher-background.jpg" "$appdir/usr/share/world-across/art/"
ffx_licence="$root/external/fidelityfx-sdk-v1.1.4/sdk/LICENSE.txt"
if grep -q 'FidelityFX SDK found' "$work/launcher-configure.log"; then
    cp "$ffx_licence" "$doc/licences/FidelityFX-SDK-LICENSE.txt"
fi
cp "$work/SDL3-3.4.20/LICENSE.txt" "$doc/licences/SDL3-LICENSE.txt"
cat > "$doc/licences/SOURCES.txt" <<'TEXT'
Where the sources of what this AppImage bundles are

World Across (usr/bin/world-across, the kit's tools, include and glue) -- MIT, ../LICENSE
    https://github.com/PaceTea/world-across

SDL 3.4.20 (built into world-across) -- zlib licence, SDL3-LICENSE.txt
    https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20

Fira Sans Condensed 4.301 (usr/share/world-across/fonts) -- SIL Open Font License 1.1,
FiraSans-OFL.txt
    https://github.com/bBoxType/FiraSans

The launcher's picture (usr/share/world-across/art/launcher-background.jpg) -- by teethegrinder,
made for World Across on commission; not under the MIT licence of the code
    https://www.instagram.com/teethegrinder

AMD FidelityFX Super Resolution 1 and the FidelityFX SDK 1.1.4 (FSR 3), built into world-across --
MIT, FSR1-LICENSE.txt and FidelityFX-SDK-LICENSE.txt
    https://github.com/GPUOpen-Effects/FidelityFX-FSR
    https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/tag/v1.1.4

Python 3.14.10, from python-build-standalone 20261110 (kit/python) -- PSF licence,
Python-LICENSE.txt; the libraries built into it (OpenSSL, libffi, zlib, bzip2, xz, zstd, SQLite,
expat, mpdecimal, ncurses, libedit, libuuid) have their licences in Python-included/
    https://github.com/astral-sh/python-build-standalone/releases/tag/20261110
    https://www.python.org/downloads/release/python-31410/

capstone 5.0.9 (kit/python/lib/python3.14/site-packages/capstone) -- BSD, capstone-LICENSE.txt
    https://github.com/capstone-engine/capstone/tree/5.0.9

The compiler (kit/gcc): from musl-cross 20261001, built with crosstool-NG, unchanged, of which this
AppImage keeps the C compiler, the assembler, the linker, libgcc and the C headers. Its
configuration, which names every component, is kit/gcc/src/x86_64-unknown-linux-musl-ct-ng.config.bz2;
the licences it carries are in compiler/.
    musl-cross (MIT) -- https://github.com/cross-tools/musl-cross/releases/tag/20261001
    GCC 16.2.0 -- GPL 3 (GPL-3.0.txt), libgcc with the GCC Runtime Library Exception
        https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/gcc-16.2.0.tar.xz
    GNU binutils 2.47 (as, ld) -- GPL 3
        https://ftp.gnu.org/gnu/binutils/binutils-2.47.tar.xz
    GMP 6.3.0, MPFR 4.2.2, MPC 1.3.1 (built into the compiler) -- LGPL 3 (LGPL-3.0.txt)
        https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz
        https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.2.tar.xz
        https://ftp.gnu.org/gnu/mpc/mpc-1.3.1.tar.gz
    isl 0.27, zlib 1.3.1, zstd 1.5.7 (built into the compiler) -- MIT, zlib, BSD
        https://libisl.sourceforge.io/isl-0.27.tar.xz
        https://zlib.net/fossils/zlib-1.3.1.tar.gz
        https://github.com/facebook/zstd/releases/tag/v1.5.7
    musl 1.2.6 (the C headers) -- MIT
        https://musl.libc.org/releases/musl-1.2.6.tar.gz
TEXT

cp "$root/tools/appimage/world-across.desktop" "$appdir/world-across.desktop"
cp "$root/tools/appimage/world-across.desktop" "$appdir/usr/share/applications/"
cp "$root/tools/appimage/world-across.png" "$appdir/world-across.png"
cp "$root/tools/appimage/world-across.png" "$appdir/usr/share/icons/hicolor/256x256/apps/"
ln -s world-across.png "$appdir/.DirIcon"
cat > "$appdir/AppRun" <<'APPRUN'
#!/bin/sh
here=$(dirname "$(readlink -f "$0")")
exec "$here/usr/bin/world-across" "$@"
APPRUN
chmod +x "$appdir/AppRun"

# --- Checks -------------------------------------------------------------------------------------
# Nothing of a card may be in it: no code.bin, no card image, no module built from one.
if find "$appdir" \( -iname 'code.bin' -o -iname '*.3ds' -o -iname '*.cci' -o -iname 'libgame.so' \
                    -o -iname 'albw_generated*' \) | grep -q .; then
    die "something made from a card ended up in the AppImage"
fi
# Every program and library in it starts on glibc 2.34.
while IFS= read -r -d '' file; do
    head -c 4 "$file" | grep -q 'ELF' || continue
    v=$(newest_glibc "$file")
    if above_floor "$v"; then die "${file#"$appdir"/} needs glibc $v, newer than $floor"; fi
done < <(find "$appdir" -type f -print0)

# --- The AppImage: the runtime, and the directory as a SquashFS image right behind it ------------
rm -f -- "$work/AppDir.squashfs" "$out"
mksquashfs "$appdir" "$work/AppDir.squashfs" -root-owned -noappend -comp zstd -Xcompression-level 19 \
    -quiet -no-progress
cat "$runtime" "$work/AppDir.squashfs" > "$out"
chmod +x "$out"
rm -f -- "$work/AppDir.squashfs"
remove "$work/kit-work"
echo "make_appimage: $out ($(du -h "$out" | cut -f1)), the launcher and its kit"
