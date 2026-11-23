#!/bin/bash
# make_windows_package.sh -- World Across for Windows, as it is published: the launcher and the kit it
# makes the game with, as one folder and one .zip.
#
# The package holds nothing of the game. On its first start the launcher asks for the player's card
# and makes the game from it, on their machine (src/runtime/albw_builder.c): it reads the card's
# code, finds its functions, translates them to C and compiles them into game.dll, beside the
# player's saves. For that the kit brings what Windows does not have: the translating tools, Python
# with capstone, and a C compiler -- GCC from w64devkit, cut down to what one C file needs.
#
# What it bundles comes from external/kit/, as downloaded -- this script downloads nothing -- and is
# checked against the SHA-256 sums below before anything is taken from it:
#   python-3.14.10-embed-amd64.zip        https://www.python.org/ftp/python/3.14.10/
#   capstone-5.0.9-py3-none-win_amd64.whl  https://pypi.org/project/capstone/5.0.9/#files
#   w64devkit-x64-2.11.0.7z.exe           https://github.com/skeeto/w64devkit/releases/tag/v2.11.0
#
# Needs the Windows build of world-across (build-win/WorldAcross.exe with SDL3.dll beside it; see
# the README's Windows section), 7z (to unpack w64devkit) and python3.
#
#   tools/make_windows_package.sh   -> build-win/World-Across-Windows/ and build-win/World-Across-Windows.zip
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
build="$root/build-win"
downloads="$root/external/kit"
name="World-Across-Windows"
out="$build/$name"
work="$build/package-work"

die() { echo "make_windows_package: $*" >&2; exit 1; }
exe="$build/WorldAcross.exe"
[ -f "$exe" ] || die "no build-win/WorldAcross.exe yet; build the world-across target first (see the README)"
[ -f "$build/SDL3.dll" ] || die "no SDL3.dll beside it: the build has no window (SDL3's MinGW files missing)"
command -v 7z > /dev/null || die "7z is missing (package 7zip)"
command -v python3 > /dev/null || die "python3 is missing"
strip=x86_64-w64-mingw32-strip
command -v "$strip" > /dev/null || die "x86_64-w64-mingw32-strip is missing (package mingw-w64-binutils)"

# The program has to be the launcher, which carries no game: albw-boot, the developer's build, has
# the translation linked in and must never be packaged. The launcher is the one that loads a module.
grep -q albw_game_connect "$exe" || die "build-win/WorldAcross.exe is not the launcher (world-across)"
[ "$(stat -c %s "$exe")" -lt 16000000 ] || die "build-win/WorldAcross.exe is too large to be the launcher"

python_zip="$downloads/python-3.14.10-embed-amd64.zip"
capstone_whl="$downloads/capstone-5.0.9-py3-none-win_amd64.whl"
w64devkit="$downloads/w64devkit-x64-2.11.0.7z.exe"
sha256sum -c --quiet <<SUMS || die "a download in external/kit/ is missing or not the expected file"
90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159  $python_zip
732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0  $capstone_whl
54c2bb9962e8a30c8270e34c5efc3d5fee579f53087fa7d435b95fbba2af7b2b  $w64devkit
SUMS

rm -rf -- "$out" "$out.zip" "$work"
mkdir -p "$out/licences" "$work"
unzip_to() { python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$1" "$2"; }

# The launcher. Without its debugging information it is a tenth of the size.
"$strip" -o "$out/WorldAcross.exe" "$exe"
"$strip" -o "$out/SDL3.dll" "$build/SDL3.dll"

kit="$out/kit"
mkdir -p "$kit/tools" "$kit/entries" "$kit/include" "$kit/glue"
# The translating tools, the addresses the game is known to branch to, and what the translation is
# compiled with: the header it includes and the glue that makes it a module.
cp "$root/tools/extract_rom.py" "$root/tools/find_functions.py" "$root/tools/recompile.py" "$kit/tools/"
cp "$root"/entries/*.txt "$root"/entries/*.starts "$kit/entries/"
cp "$root/src/runtime/albw_cpu.h" "$root/src/runtime/albw_game.h" "$kit/include/"
cp "$root/src/runtime/albw_game_glue.c" "$kit/glue/"

# Python: the embeddable distribution, without what the tools never load (TLS, SQLite, sockets and
# the like). Its python314._pth puts its own folder on the path, so capstone goes right into it.
unzip_to "$python_zip" "$work/python"
mkdir -p "$kit/python"
for f in python.exe python314.dll python3.dll vcruntime140.dll vcruntime140_1.dll python314.zip \
         python314._pth _ctypes.pyd libffi-8.dll LICENSE.txt; do
    [ -f "$work/python/$f" ] || die "the embeddable Python has no $f"
    cp "$work/python/$f" "$kit/python/"
done
cp "$kit/python/LICENSE.txt" "$out/licences/Python-LICENSE.txt"
unzip_to "$capstone_whl" "$work/capstone"
cp -r "$work/capstone/capstone" "$kit/python/"
find "$kit/python/capstone" -name '__pycache__' -prune -exec rm -rf {} +
cp "$work/capstone/capstone-5.0.9.dist-info/LICENSE.TXT" "$out/licences/capstone-LICENSE.txt"

# GCC: the driver, the C compiler proper, the assembler and the linker, libgcc, and the C library's
# headers -- the closure of the standard headers, which is all the translation includes. The
# assembler and the linker go where the driver looks for them on its own, the target's bin/.
7z x -y -o"$work" "$w64devkit" > /dev/null
dk="$work/w64devkit"
target=x86_64-w64-mingw32
gccver=16.2.0
gcc="$kit/gcc"
mkdir -p "$gcc/bin" "$gcc/libexec/gcc/$target/$gccver" "$gcc/lib/gcc/$target/$gccver" "$gcc/$target/bin" "$gcc/src"
cp "$dk/bin/gcc.exe" "$gcc/bin/"
cp "$dk/libexec/gcc/$target/$gccver/cc1.exe" "$dk/libexec/gcc/$target/$gccver/collect2.exe" \
   "$gcc/libexec/gcc/$target/$gccver/"
cp "$dk/bin/as.exe" "$dk/bin/ld.exe" "$gcc/$target/bin/"
cp "$dk/lib/gcc/$target/$gccver/libgcc.a" "$gcc/lib/gcc/$target/$gccver/"
while read -r header; do
    [ -n "$header" ] || continue
    mkdir -p "$gcc/$(dirname "$header")"
    cp "$dk/$header" "$gcc/$header"
done <<'HEADERS'
include/assert.h
include/complex.h
include/corecrt.h
include/corecrt_stdio_config.h
include/corecrt_wconio.h
include/corecrt_wctype.h
include/corecrt_wstdlib.h
include/crtdefs.h
include/ctype.h
include/errno.h
include/fenv.h
include/float.h
include/inttypes.h
include/limits.h
include/locale.h
include/malloc.h
include/math.h
include/_mingw.h
include/_mingw_mac.h
include/_mingw_off_t.h
include/_mingw_secapi.h
include/_mingw_stat64.h
include/_mingw_stdarg.h
include/pthread_compat.h
include/pthread_signal.h
include/pthread_time.h
include/sec_api/stdio_s.h
include/sec_api/stdlib_s.h
include/sec_api/string_s.h
include/sec_api/sys/timeb_s.h
include/sec_api/wchar_s.h
include/sec_api/wconio_s.h
include/setjmp.h
include/signal.h
include/stdarg.h
include/stddef.h
include/stdint.h
include/stdio.h
include/stdlib.h
include/string.h
include/swprintf.inl
include/sys/timeb.h
include/time.h
include/_timeval.h
include/uchar.h
include/vadefs.h
include/wchar.h
include/wctype.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/float.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/iso646.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/limits.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/mm_malloc.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/stdalign.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/stdarg.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/stdatomic.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/stdbool.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/stddef.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/stdint.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/stdnoreturn.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/syslimits.h
lib/gcc/x86_64-w64-mingw32/16.2.0/include/tgmath.h
HEADERS
# How w64devkit built it: its recipe and its patches to GCC, binutils and Mingw-w64, beside where
# their sources are (licences/SOURCES.txt).
cp "$dk/Dockerfile" "$dk/README.md" "$gcc/src/"
for part in gcc gcc-final binutils mingw; do cp -r "$dk/src/$part" "$gcc/src/"; done
cp "$dk/COPYING.MinGW-w64-runtime.txt" "$out/licences/MinGW-w64-LICENSE.txt"
cp "$root/third_party/kit/GPL-3.0.txt" "$root/third_party/kit/LGPL-3.0.txt" \
   "$root/third_party/kit/GCC-RUNTIME-LIBRARY-EXCEPTION.txt" "$out/licences/"

cp "$root/LICENSE" "$out/LICENSE.txt"
cp "$root/third_party/fsr1/license.txt" "$out/licences/FSR1-LICENSE.txt"
# The menu's type, beside the program where the launcher looks for it.
mkdir -p "$out/fonts"
cp "$root"/third_party/fonts/*.ttf "$out/fonts/"
cp "$root/third_party/fonts/OFL.txt" "$out/licences/FiraSans-OFL.txt"
# The launcher's picture, by teethegrinder, beside the program as well.
mkdir -p "$out/art"
cp "$root/third_party/art/launcher-background.jpg" "$out/art/"
sdl_licence=
for sdl in "$root"/external/SDL3-*/LICENSE.txt; do [ -f "$sdl" ] && sdl_licence=$sdl; done
[ -n "$sdl_licence" ] || die "no SDL3 licence in external/SDL3-*/"
cp "$sdl_licence" "$out/licences/SDL3-LICENSE.txt"

# Plain text with Windows line ends, for Notepad.
crlf() { sed 's/$/\r/'; }
crlf > "$out/licences/SOURCES.txt" <<'TEXT'
Where the sources of what this package bundles are

World Across (WorldAcross.exe, kit\tools, kit\include, kit\glue) -- MIT, LICENSE.txt
    https://github.com/PaceTea/world-across

SDL 3 (SDL3.dll) -- zlib licence, SDL3-LICENSE.txt
    https://github.com/libsdl-org/SDL

AMD FidelityFX Super Resolution 1 (in WorldAcross.exe) -- MIT, FSR1-LICENSE.txt
    https://github.com/GPUOpen-Effects/FidelityFX-FSR

Fira Sans Condensed 4.301 (fonts) -- SIL Open Font License 1.1, FiraSans-OFL.txt
    https://github.com/bBoxType/FiraSans

The launcher's picture (art\launcher-background.jpg) -- by teethegrinder, made for World Across on
commission; not under the MIT licence of the code
    https://www.instagram.com/teethegrinder

Python 3.14.10, the embeddable distribution (kit\python) -- PSF licence; libffi and the rest it
includes are listed in Python-LICENSE.txt
    https://www.python.org/downloads/release/python-31410/

capstone 5.0.9 (kit\python\capstone) -- BSD, capstone-LICENSE.txt
    https://github.com/capstone-engine/capstone/tree/5.0.9

The compiler (kit\gcc): from w64devkit 2.11.0, unchanged, of which this package keeps the C
compiler, the assembler, the linker, libgcc and the C headers.
    w64devkit 2.11.0 (its recipe: public domain) -- https://github.com/skeeto/w64devkit/tree/v2.11.0
    GCC 16.2.0 -- GPL 3 (GPL-3.0.txt), libgcc with the GCC Runtime Library Exception
        https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/gcc-16.2.0.tar.xz
    GNU binutils 2.47 (as, ld) -- GPL 3
        https://ftp.gnu.org/gnu/binutils/binutils-2.47.tar.xz
    GMP 6.3.0, MPFR 4.2.2, MPC 1.4.1 (built into the compiler) -- LGPL 3 (LGPL-3.0.txt)
        https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz
        https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.2.tar.xz
        https://ftp.gnu.org/gnu/mpc/mpc-1.4.1.tar.xz
    Mingw-w64 14.0.0 (the C headers) -- MinGW-w64-LICENSE.txt
        https://sourceforge.net/projects/mingw-w64/files/mingw-w64/mingw-w64-release/
w64devkit's Dockerfile, which names each of these with its SHA-256, and its patches to them are
in kit\gcc\src.
TEXT

crlf > "$out/README.txt" <<'TEXT'
World Across for Windows

World Across plays The Legend of Zelda: A Link Between Worlds natively, from a dump of your own
cartridge. This package is only the launcher: it holds no part of the game.

1. Unpack the whole .zip (right-click, Extract All) and start WorldAcross.exe from the unpacked
   folder. Windows may warn that the program is unknown: More info, then Run anyway.
2. Choose your card: a decrypted dump of your own cartridge, .3ds or .cci.
3. The launcher makes the game from it, on this machine -- it reads the card, translates the
   game's code and compiles it. That takes a few minutes, once. Then press Play.

It needs Windows 10 or later, 64-bit, and about 4 GB of memory for the one build. The graphics card
draws the game through Vulkan, which every current AMD, NVIDIA and Intel driver provides; without
it the processor draws instead.

Where things are kept:
  card\                             your card, moved here when you choose it
  %LOCALAPPDATA%\albw-port\build    the game as the launcher made it, and build.log
  %APPDATA%\albw-port               settings and saves; last-run.log, the log of the last run
What the launcher makes from your card is yours alone: do not share it.

To remove World Across, delete this folder and the two folders named above (%LOCALAPPDATA%\albw-port
and %APPDATA%\albw-port). Nothing else is installed.

kit\ holds what the build uses: the translating tools, Python with capstone and the GCC compiler.
licences\ has their licences and says where their sources are.
TEXT

# Nothing of a card may be in the package: no code.bin, no card image, no module built from one.
if find "$out" \( -iname 'code.bin' -o -iname '*.3ds' -o -iname '*.cci' -o -iname 'game.dll' \
                 -o -iname 'albw_generated*' \) | grep -q .; then
    die "something made from a card ended up in the package"
fi

rm -rf -- "$work"
(cd "$build" && 7z a -tzip -mx=9 "$name.zip" "$name" > /dev/null)
echo "make_windows_package: $out.zip ($(du -h "$out.zip" | cut -f1)), the launcher and its kit"
