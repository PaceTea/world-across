# World Across

A native Linux and Windows build of *The Legend of Zelda: A Link Between Worlds* (Nintendo 3DS),
made by translating the game's ARM11 code into C ahead of time rather than interpreting it at
runtime. Inside the tree the project still goes by its working name, albw-port: the developer's
program is `build/albw-boot`, and settings and saves live in directories of that name.
There is no emulator underneath: each guest function becomes a C function, and the parts of the
3DS operating system the game expects are reimplemented as a small kernel in `src/runtime`.

This repository contains no part of the game. To build and run it you need your own decrypted dump
of a cartridge you own; everything derived from it stays on your machine (see below). This
project is not affiliated with, endorsed by or connected to Nintendo. *The Legend of Zelda* and
*A Link Between Worlds* are trademarks of Nintendo.

**Status: the opening is playable.** The game gets through Horizon's service handshakes, reads
archives out of its RomFS, answers its configuration queries, formats and opens its save data and
starts its threads. It shows the loading screen, the title screen with its lit 3D scene, the file
select and the name entry -- the software keyboard is answered with your login name -- and plays
through the opening into Link's house and out of its door: Gulley's dialogue, walking, the map on
the bottom screen, the item menu, the weather vane that saves the game, and loading that save
again. Further in, the game reaches code nobody has translated yet; the port then stops, notes the
address, and `tools/catch_up.sh` translates it into the build (see below). Most of the game has
never been played here.

**Picture.** By default the graphics card draws the game, at up to eight times the console's
resolution (1440p for the top screen at six), with multisampled anti-aliasing; AMD FSR 3 and NVIDIA
DLSS can upscale it instead, and above 60 frames a second frames in between are drawn from the
game's real geometry. A software renderer draws the console's own resolution on the processor; it
is what the regression checks compare against.

**Sound** comes from the port's own version of the DSP's audio firmware: the game's voices --
ADPCM music streams, PCM sound effects -- decoded, resampled, filtered and mixed, 32,728 samples a
second.

What is missing: cube maps and the other GPU features this game has not been seen to use, the
DSP's reverb and compressor, frame generation, and the certainty that the rest of the game runs.
So this is a port whose first scenes can be played, plus the tooling to keep extending it as the
game goes on -- not yet one anybody has played through.

## Download

The [releases](https://github.com/PaceTea/world-across/releases) have the launcher for Linux and
for Windows. It holds no part of the game: the first time it starts, you choose your card -- a
decrypted dump of a cartridge you own -- and it makes the game from it on your machine, reading
the card, translating the game's code to C and compiling it. That takes a few minutes, once; the
tools it needs (the translator, Python and a C compiler) come with it.

- **Linux**: `World-Across-x86_64.AppImage`. Make it executable (`chmod +x`) and start it. It
  runs on any distribution with glibc 2.34 or later -- Ubuntu 22.04, Debian 12, Fedora 35 and
  newer -- and needs nothing installed besides a graphics driver.
- **Windows**: `World-Across-Windows.zip`. Unpack it entirely and start `WorldAcross.exe` from the
  unpacked folder. Windows 10 or later, 64-bit.

The graphics card draws the game through Vulkan, which every current AMD, NVIDIA and Intel driver
provides; without it the processor draws instead. What the launcher makes from your card stays
on your machine -- `~/.cache/albw-port/build` on Linux, `%LOCALAPPDATA%\albw-port\build` on
Windows -- and is yours alone.

## What you need to supply

Nothing in this repository is derived from the game. No code, no assets, no addresses read out
of a binary anyone would have to own — with one exception noted below. To build something that
runs, you supply your own dump of a cartridge you own, and everything derived from it stays on
your machine: `rom-extract/`, `src/generated/`, `ghidra*/` and `*.3ds` are all ignored by git.

The exception is `entries/`, lists of guest addresses the running game branched to that nothing
in the image points at, one file per release by its product code (`CTR-P-BZLE.txt` is the American
one). They are kept so that a build is reproducible rather than depending on how far someone has
played. They are addresses, not code. Another release needs none of them to build: its own file
starts empty and fills as `tools/catch_up.sh` meets what is missing.

A dump must be **decrypted**; the RomFS reader refuses an encrypted image rather than
producing confusing garbage.

## Building

Required packages, by what they are for:

| For | Packages |
| --- | --- |
| Building | `cmake`, `ninja`, `gcc` |
| A window, input (optional) | `sdl3` — without it the port builds and runs headless |
| The launcher's text (optional) | `stb` (stb_truetype) and any TrueType font — without it the game starts straight away |
| The GPU renderer and AMD FSR 1 (optional) | `shaderc` (glslc) at build time, and a Vulkan driver — without them the processor draws and the picture is scaled plainly |
| AMD FSR 3 upscaling (optional) | AMD's FidelityFX SDK 1.1.4 in `external/` (below), `vulkan-headers`, `python` — without it the choice is dimmed |
| NVIDIA DLSS upscaling (optional) | NVIDIA's DLSS SDK in `external/` (below), `vulkan-headers`, and an NVIDIA RTX card to run it on |
| Translating | `python3`, `python-capstone` |
| Verifying (optional) | `python-unicorn` |

Then, from a checkout:

```bash
python3 tools/extract_rom.py /path/to/your.3ds
```

That writes `rom-extract/code.bin` — the executable segment, loaded at `0x00100000` — plus the
exheader and ExeFS pieces and the card's product code, and prints the segment layout and the
services the binary declares.

```bash
python3 tools/find_functions.py
python3 tools/recompile.py --all --split 16
```

The first finds the game's functions in its code -- where each one starts and which addresses
make up its body -- and writes them to `rom-extract/functions.tsv`; it takes about a minute and a
half. The second translates them into `src/generated/`: one C file per chunk of translated
functions, plus the dispatch table. It prints how much it could translate and what the remaining
gaps are.

```bash
cmake -B build -G Ninja && ninja -C build
```

### Windows

The Windows program is built on Linux -- or under WSL2 on Windows itself -- with the MinGW-w64
cross compiler, from the same translation (the extracting and translating steps above are the
same):

```bash
cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
ninja -C build-win
```

That needs `mingw-w64-gcc` (Arch; `gcc-mingw-w64-x86-64-posix` and `g++-mingw-w64-x86-64-posix`
on Debian and Ubuntu) and, for the window, SDL3's MinGW development files: unpack
`SDL3-devel-<version>-mingw.tar.gz` from SDL's releases into `external/` (or point `SDL3_DIR` at
its `x86_64-w64-mingw32/lib/cmake/SDL3`). Without them it is built headless. The result is
`build-win/albw-boot.exe`, with `SDL3.dll` beside it; it needs Windows 10 or later and, for the GPU
renderer, a Vulkan driver (every current AMD, NVIDIA and Intel driver has one). Settings and saves
go to `%APPDATA%\albw-port`, and the log of the last run, when it was not started from a console,
to `%APPDATA%\albw-port\last-run.log`.

The Windows build is checked the way the Linux one is, under Wine: `ALBW_WINE=1
tools/frames_check.sh check` and `ALBW_WINE=1 tools/routes_check.sh check` play it against the same
reference, and it has to come out byte for byte the same. FSR 3 and DLSS are not in the Windows
build yet. Like everything built from your cartridge, it is yours alone.

### The launcher and the packages

Besides `albw-boot`, the build makes `world-across` (`WorldAcross.exe` on Windows): the same
program without the translation linked in. It finds the game as a module -- `libgame.so`, or
`game.dll` -- in the cache folder, and where there is none yet, it makes one from the card
(`src/runtime/albw_builder.c`): it runs the three steps above with the tools of a kit and
compiles the translation, with the glue in `src/runtime/albw_game_glue.c`, into a module that
needs no C library (`src/runtime/albw_game.h` is the interface). `world-across --build-game` does
the same without a window. In a checkout the kit is the checkout itself, with the system's Python
and compiler.

The published packages are that launcher with a kit of their own:

```bash
tools/make_appimage.sh            # build-appimage/World-Across-x86_64.AppImage
tools/make_windows_package.sh     # build-win/World-Across-Windows.zip
```

Each bundles the translating tools, Python with capstone and a C compiler cut down to what the
translation needs -- GCC 16 from [musl-cross](https://github.com/cross-tools/musl-cross) for
Linux, from [w64devkit](https://github.com/skeeto/w64devkit) for Windows -- with their licences
and where their sources are. The scripts download nothing: they take what they bundle from
`external/kit/` and check each file against the SHA-256 sum they name, together with where it
comes from. The AppImage builds SDL3 statically and holds the launcher to glibc 2.34
(`tools/appimage/`); the Windows package needs the Windows build of `world-across` first. Both
refuse to package anything made from a card.

A module the packages' compilers made has to play exactly as `albw-boot` does: `ALBW_BOOT` names
the program the checks run, so

```bash
XDG_CACHE_HOME=<its cache> ALBW_BOOT=build-appimage/World-Across-x86_64.AppImage tools/routes_check.sh check
ALBW_WINE=1 ALBW_BOOT=<unpacked package>/WorldAcross.exe tools/routes_check.sh check
```

play the AppImage and the Windows package against the same reference.

## Running

```bash
./build/albw-boot /path/to/your.3ds
```

The card image is needed at runtime as well as at extraction time, because the game reads its
RomFS through the filesystem service rather than from anything baked into the build. Its path is
remembered (in `~/.config/albw-port/card`), so after the first start `./build/albw-boot` alone is
enough, from any directory. Started with no card known, the launcher's first item is Choose card,
which asks for it in the desktop's file dialog. `ALBW_ROM=/path/to/your.3ds` names it too, which is
how the tools do it.

It will run until it hits something unimplemented and say what that was. Useful knobs:

| Variable | Effect |
| --- | --- |
| `ALBW_ROM` | the card image, for RomFS reads |
| `ALBW_SAVE_DIR` | where saves go; see below |
| `ALBW_DISCOVER=<file>` | record the first indirect branch target that has no translated function, then stop |
| `ALBW_NEIGHBOURS` | report where else a missing branch target appears in guest memory |
| `ALBW_LOG_FIRST`, `ALBW_LOG_TOTAL` | how many times each kind of event is logged, and how many lines in total |
| `ALBW_TRACE_FILE`, `ALBW_TRACE_SKIP`, `ALBW_TRACE_CAP` | where to write the instruction trace and which window of it (`albw-boot-trace` only) |
| `ALBW_DUMP_FRAMES`, `ALBW_DUMP_EVERY` | write the screens as PPMs into a directory, and how often |
| `ALBW_DUMP_CMDLIST`, `ALBW_DUMP_REGS`, `ALBW_DUMP_DRAWS` | what the guest asks the GPU for |
| `ALBW_DUMP_SHADER=<dir>` | the uploaded vertex shaders, disassembled, one file per entry point |
| `ALBW_RENDER=<n>`, `ALBW_RENDER_FROM=<frame>` | rasterise n frames, from this one, without a window |
| `ALBW_STOP_AT=<frame>` | end the run at that frame, for benchmarks; the raster report says where the time went |
| `ALBW_PROFILE=<file>` | a sampling profile of the run, as offsets `addr2line -f -e build/albw-boot` reads |
| `ALBW_PROFILE_WALL=1`, `ALBW_PROFILE_FROM=<frame>` | profile the main thread's real time instead, from that frame |
| `ALBW_DUMP_TARGET=<dir>` | the render target itself as a picture, one per draw |
| `ALBW_DUMP_FROM` | the first frame `ALBW_DUMP_FRAMES` writes |
| `ALBW_SCAN`, `ALBW_PROBE` | where the interesting frames are; what a draw reads and writes |
| `ALBW_WATCH_ADDR=<addr>` | backtrace every store to one address (`albw-boot-nan` only) |
| `ALBW_PRESS=<frame>:<buttons>,...` | scripted controls, e.g. `1300:A,1310:-,7900:CDOWN,8100:-`; buttons are `A B X Y L R START SELECT UP DOWN LEFT RIGHT`, the circle pad `CUP CDOWN CLEFT CRIGHT`, a touch on the bottom screen `T<x>.<y>` (e.g. `T250.225`), joined with `+`, and `-` lets go |
| `ALBW_FRESH_SAVES=1` | with `ALBW_SAVE_DIR`, `tools/discover_loop.sh` empties that directory before each round, so a script sees the same menus every time |
| `ALBW_TRACE_QUEUE=<frame>` | from that frame, each drain of the GPU command queue: lists and the target they left, transfers, fills |
| `ALBW_NO_BATCH=1` | rasterise every draw on its own, to tell a batching mistake from any other |
| `ALBW_DSP_LOG=<frame>` | from that frame, each buffer a voice starts (format, length, rate) and each voice switched on, off or run dry |
| `ALBW_THREADS=1` | with `ALBW_STOP_AT`, what each thread is waiting on when the run stops |
| `ALBW_SCREENSHOT=<frame>:<file.bmp>` | the whole window as it looked at that frame (works offscreen with `SDL_VIDEO_DRIVER=offscreen`) |
| `ALBW_DUMP_MEM=<frame>,...` | write the guest's data, heap and linear memory at those frames, into `ALBW_DUMP_MEM_DIR` |
| `ALBW_REPLAY_CHECK=1` | record every frame as the frames in between are drawn from, play it back unmoved into the shadow, and compare it with the game's own picture (it must come out byte for byte the same; works headless) |
| `ALBW_REPLAY_HALF=<frame>` | write that frame's top screen, the one before it, and three frames between them as PPMs into `ALBW_REPLAY_DIR` |
| `ALBW_REPLAY_VERBOSE=1` | what each recorded frame holds, and what a frame in between costs |
| `ALBW_FSR3_TEST=<frame>:<count>[:<scale>]` | run FSR 3 headless over those frames and write what goes in (the jittered picture, depth, motion) and what comes out into `ALBW_REPLAY_DIR` |
| `ALBW_GPU_FSR3_TEST=<first>-<last>:<render scale>:<shown scale>:<dir>` | the same through the GPU renderer: those frames drawn on the card at the render scale with the jitter and the upscaler's targets, upscaled by FSR 3 to the shown scale, and the last one written as `fsr3-<frame>.ppm` (with its input and motion) beside `gpu-<frame>-native.ppm`, the frame drawn straight at the shown scale |
| `ALBW_VKUP_TIMES=1` | what each FSR 3 call spends on its inputs, the dispatch, the card and the output |
| `ALBW_VK_VALIDATE=1` | FSR 3's Vulkan device with Khronos' validation layer, where it is installed |
| `ALBW_GPU_TEST=<first>[-<last>]:<scale>:<dir>[:alone]` | draw those recorded frames headless on a GPU device of the port's own (no window, no display touched), at that scale, and write the last one's screens as `gpu-<frame>-top.ppm` and `-bottom.ppm` into the directory; `:alone` has the processor draw nothing from the first frame on, as with the window; `ALBW_GPU_SAMPLES` sets the multisampling |
| `ALBW_REC_DUMP=<frame>` | what that frame's record holds, event by event: fills, transfers, texture copies, and each draw's target, viewport and state |
| `ALBW_FRAME_TIMES=<file>` | each frame's wall time, syscalls, file reads and their bytes, the renderer's time, and how long each guest thread ran (entry point and priority) -- for finding where a hitch comes from |
| `ALBW_WATCH_VRAM=1` | make VRAM inaccessible whenever guest code runs and report every place that touches it: the game's code never does, which is what lets the GPU renderer leave it undrawn |

## Playing

With SDL3 installed the port opens a window; without it, or with `ALBW_HEADLESS=1`, it runs headless
exactly as before.

The window opens on a launcher: Play, Settings and Quit. The game itself starts only on Play --
nothing runs or sounds before. Behind the menu goes a picture of your own if you put one at
`~/.config/albw-port/background.png` (or `.jpg`); none comes with the port. The menu is set in Fira
Sans Condensed where it is installed, and the title in Cinzel if you have that. Settings
(fullscreen, how the screens are arranged, the renderer, its resolution and anti-aliasing,
upscaling and its sharpening, the frame rate, whole-pixel scaling, sound, gamepads, the game's
language) are applied at once and saved in
`~/.config/albw-port/settings`; the environment variables below still win over them. Choices this
build or machine cannot do -- FSR 3 or DLSS without their SDKs, DLSS without an RTX card, frame
generation for now -- are shown dimmed, with what they would take. In game,
**Esc** brings the menu back and **F1** its settings, with the game paused underneath.
`ALBW_LAUNCHER=0` starts without it. The two screens are stacked as on the console, and the window can be resized —
the picture keeps its shape.

| Console | Keyboard | Gamepad |
| --- | --- | --- |
| Circle pad | W A S D | left stick |
| D-pad | arrow keys | D-pad |
| A, B | K, J | right and bottom face buttons |
| X, Y | I, U | top and left face buttons |
| L, R | Q, E | shoulder buttons or triggers |
| Start, Select | Enter, Backspace | Start, Back |
| Touch screen | left mouse button on the bottom screen | — |

The face buttons go by position, not by label: the console's A is on the right and its B at the
bottom, and that is what hands remember, whatever the pad prints on them.

| Key | Does |
| --- | --- |
| P | pause and resume — the game simply does not get its next frame |
| Tab (held) | run as fast as possible, for long texts and long walks |
| F11 | fullscreen |

| Variable | Effect |
| --- | --- |
| `ALBW_SCALE` | the window's starting size, as a multiple of the console's (default 2) |
| `ALBW_RENDERER` | `gpu` (default: the graphics card draws, see below) or `software` (the processor draws, at the console's resolution) |
| `ALBW_RESOLUTION` | the GPU renderer's resolution: `auto` (default: as many pixels as the screens take up in the window) or a multiple of the console's, `1` to `8` (`3` is 720p for the top screen, `6` 1440p) |
| `ALBW_ANTIALIAS` | the GPU renderer's multisampling: `off`, `2`, `4` (default) or `8` samples a pixel |
| `ALBW_LAYOUT` | `stacked` (default), `side` for side by side, `top` for the top screen alone |
| `ALBW_FRAME_RATE` | `console` (default: the game's own 60), `display` (frames in between, up to the display's refresh rate, waiting for it) or `uncapped` (as many frames in between as the machine has room for) |
| `ALBW_UPSCALE` | `off` (default: each pixel a block), `bilinear`, `fsr1` (AMD FSR 1, EASU upscaling and RCAS sharpening on the GPU), `fsr3` (AMD FSR 3, temporal, for the top screen; with the GPU renderer it draws the top screen at two thirds of the window's size, or at the Resolution setting's, and FSR 3 upscales it to the window -- see below), `dlss` (NVIDIA DLSS, the same way, on NVIDIA's RTX cards; `ALBW_DLSS_JITTER=-1` turns the jitter round should it shimmer) or `software` (FSR 1's filter on the processor, for the software renderer; about 4 ms for a screen at three times its size on twelve cores) |
| `ALBW_FILTER` | the older name: `nearest`, `linear` or `fsr` |
| `ALBW_INTEGER_SCALE` | keep every pixel the same size when scaling |
| `ALBW_MUTE=1` | no sound |
| `ALBW_GAMEPADS=0` | ignore gamepads, keyboard and mouse only |
| `ALBW_WAV=<file>` | also write the sound to a WAV file, with or without a window |
| `ALBW_LANGUAGE` | the console's language, as a code (`en`, `fr`, `de`, ...) or a number |
| `ALBW_USERNAME` | the name the game offers for the player (default: your login name) |

By default the graphics card draws the picture, at the window's resolution or at a fixed multiple
of the console's (Settings, Resolution: 3x is 720p for the top screen, 6x 1440p), with multisampled
anti-aliasing. The game's own drawing does not change for it: the console's vertex programs still
run on the processor and every frame is recorded, as for the frames in between below; the card
then draws the record -- the texture combiner, the fragment lighting, the depth and stencil tests
and the blending as the console does them -- into targets of its own, and copies each screen out
where the display transfers say. The processor sets up and rasterises nothing, which takes most of
its work off the game's thread: outdoors a frame drops from about 18 ms of the processor's time to
about 6. That is sound because the game's code never reads what the GPU drew -- `ALBW_WATCH_VRAM=1`
checks it -- and a texture the game makes by copying a target (the Items menu draws over a picture
of the top screen) is copied on the card as well. Settings, Renderer: Software draws on the
processor as before, at the console's resolution; that is what the frame checks compare.

FSR 3 and DLSS take the GPU renderer's picture of the top screen drawn below the window's size,
moved by a sub-pixel jitter each frame, with its depth, its motion back to the frame before (each
vertex's place in the frame before comes from the record of that frame) and a mask of what is
blended over the scene, and upscale it to the window's pixels exactly. Both run on a Vulkan device
of the port's own -- SDL's GPU API shares no handles to its own -- so the inputs are read back from
SDL's device and the picture comes back the same way; that happens on a thread of its own, one
frame behind the game, so the window keeps its 60 frames. The bottom screen is drawn as without
them. Before the GPU renderer, the upscalers were given the console's own 400 by 240 pixels, which
is more than they are made to make up.

With the software renderer everything is drawn on the processor, spread over its cores. The title bar shows the frame rate and
how much of each frame's 16.7 ms the game and the renderer took; on a twelve-core desktop the title
screen needs about 8 ms, and four cores of the same speed about 16. `ALBW_RENDER_THREADS` sets how
many threads draw (the default is one fewer than the processor has), and `ALBW_FPS=1` also prints
the title bar's numbers to the log once a second. The game draws its top screen once per eye; only
the left eye is shown, so the right eye's pass is left undrawn (`ALBW_RIGHT_EYE=1` draws it).

Above 60 frames a second (Settings, Frame rate: Display or Uncapped), the frames between the
game's own are drawn from the real geometry, not guessed from pictures. Every frame the game draws
is recorded -- each draw's shaded vertices and state, each fill and transfer -- and a frame part of
the way to the next is that frame drawn again with each vertex moved back towards where the frame
before had it. A draw is matched to its counterpart by what it draws (shader, vertex buffers,
textures, target, and its place among the draws that share those); one whose vertices jumped, or
that has no counterpart, is drawn where it is. These frames are drawn into a shadow of the GPU's
memory beyond the guest's, on the window's thread while the game works on its next frame on its
own; the renderer's threads always take the game's work first, and the window draws only as many
frames in between as leave the game its 60. On the twelve-core desktop that is about twice the
frames: the title scene shows some 120 a second. It adds next to no delay: the console itself
shows each frame a frame after drawing it, and the window shows it then too, with the frames in
between leading up to it. A cut, where most of the scene jumps, is shown as a cut.

The language follows `$LANG` when the card's region offers it, and falls back to the region's own
default when it does not: an American card has no German text, so `de_DE` comes out English.

### When the game reaches code that is not translated yet

The translation grows as the game is played: a function the game only reaches through a pointer is
found the first time the game goes there. When that happens the port stops, says so, and notes the
address in `missing_entries.txt` beside the saves. Then

```bash
tools/catch_up.sh
```

translates it into the build, and the game can be started again from its last save.

## Where saves go

Anything the game writes — its save, and the play coin file it expects the console to have —
lands under `$ALBW_SAVE_DIR`, or `$XDG_DATA_HOME/albw-port`, or `~/.local/share/albw-port`, one
directory per archive, as ordinary files you can copy and back up. Nothing is written inside the
card image, and a write to it is refused rather than silently dropped.

A save is written the way the console writes one: the game's save archive is journaled, so what it
writes is held beside the archive (`archive-4.pending`) until the game commits it, and then all of
it becomes the save at once. If the port stops in the middle of saving -- closed, crashed, power
gone -- the next start finishes that save if the commit had begun, and otherwise keeps the one
before; a save is never left half written.

The first time the game runs it formats its save, as it would on a new cartridge, and the format it
asked for is kept beside the archive (`archive-4.format`) so that it is not formatted again. On a
console formatting erases; here whatever was in the archive is moved into a dated
`archive-4.before-format-...` folder instead, so that a mistake in the port about when to format can
never cost you a save.

## How the pieces fit

**Finding functions.** `tools/find_functions.py` reads them off the image. A function starts
wherever something refers to one: the entry point; calls, and branches to an address that already
starts a function; words holding an address in the code -- virtual tables, callback tables, the
literal pools the code loads its function pointers from; and the C++ static initialisers, which
RVCT's `.init_array` keeps as offsets from each entry rather than as addresses, so that no word
anywhere points at the 1,230 of them. A body is what control flow reaches from its start --
branches, jump tables and fall-through, not calls -- and it ends at another function's start, at a
literal the body itself loads as data, or at a word the ARM11 cannot execute, which is how a body
notices it has run off a call that does not return into the data behind it.

This replaced a Ghidra analysis (its scripts are still in `tools/ghidra_scripts`, and an export in
the same format can be passed to the recompiler with `--tsv`). Against it, the bodies found here
hold 99.95% of the code Ghidra's did, plus about 2,000 entry points Ghidra lacked, and the game
runs the same: identical frames and identical save files through the opening.

What no static search can see is a pointer computed at runtime. `tools/discover_loop.sh` runs
the result and lets the guest name the indirect targets it actually branches to -- an address
the guest branches to is an entry point by construction -- and in play, `tools/catch_up.sh` does
the same for whatever a session met.

**Checking the translation.** `tools/oracle.py` runs the same code under Unicorn and compares
instruction by instruction, with `tools/trace_diff.py` reporting the first difference. Both
sides share one memory buffer and, importantly, the same kernel: `albw-kernel` is built as a
shared library precisely so the reference run cannot drift into answering a service call
differently. That reaches the boot path, not the game, so three narrower checks cover the rest:

- `tools/insn_check.py` takes every distinct form of instruction in the game, ARM and Thumb,
  translates a few real instances of each on its own, and runs them from random register, flag,
  VFP and memory states against Unicorn, in the floating-point mode the game sets.
- `tools/func_check.py <address>` does the same for one whole function with whatever it calls,
  for what lives between instructions: switches, a call that does not return the usual way.
- `tools/routes_check.sh save|check` plays the game along the routes in `tools/routes/` -- the
  opening up to saving at the weather vane, and from that save the walk west to Gulley, the
  item menu, and quitting to the title screen and loading again -- and
  compares the screens and the save files byte for byte with a reference. The emulation is
  deterministic, so a change meant to keep the game as it was has to come out identical.
  `tools/frames_check.sh` is the same for the title screen, in a few seconds.

**What the GPU is asked to do** is read and kept. Command lists are
applied to a register file of 768 words, and `ALBW_DUMP_REGS` prints it. Registers are identified by
arithmetic rather than by a register map: the colour and depth buffers are known from the fills that
clear them, and the registers holding those addresses — shifted by three and read as physical, VRAM
being physical `0x18000000` against virtual `0x1F000000` — come out exact at `0x11D` and `0x11C`.

`ALBW_DUMP_DRAWS` narrates the draws. A settled frame has 52 of them, every one six vertices, which
is two triangles: a quad. Together with a busy fixed-attribute port that says a screen of
two-dimensional work — a title screen or a menu.

**Drawing.** The vertex shader runs on the processor, and either renderer draws behind it. The GPU
renderer (`src/runtime/albw_gpu.c`, `src/shaders/pica.frag`) draws each recorded frame through SDL's
GPU API: the texture combiner in the console's eight-bit integer arithmetic, the fragment lighting
with its lookup tables, the alpha, depth and stencil tests and the blending, into targets the size of
the console's times the scale, which live on from frame to frame as VRAM does; textures are decoded
from guest memory and kept while their bytes stay the same. At the console's resolution it agrees
with the software renderer to within the rounding of the console's 16-bit target.

The software rasteriser covers the same ground on the processor: the attribute loaders,
the shader's own instruction set, the output semantics, the viewport transform, the texture units
including ETC1, the six combiner stages, blending, and the tiled render target the display transfer
then untiles. Every field position in the shader's encoding was read off this game's program and is
confirmed by the program making sense -- the first instructions come out as two matrix multiplies into
the position output, and a wrong field turns that into nonsense at once.

Triangles are clipped to the hardware's view volume before they are drawn, so geometry the camera
stands inside is cut rather than dropped.

The black screen this used to show was not the renderer's: the game builds its stereoscopic camera
from a configuration block the console keeps, the screen's size in millimetres, and with the block
unanswered it divided by zero and uploaded a projection matrix made of not-a-number. `albw-boot-nan`
found it — it prints the C stack at the first store of a NaN, and since the generated functions are
named after their guest addresses that stack is the game's own call chain — and the configuration
service answers it now.

**When a run stops,** the last lines say why. `[err:f]` is the game reporting a fatal error itself,
with the result code and the address that raised it; `[dispatch] no translated function` is code the
discovery loop has not reached yet; a black screen with draws behind it is a value to trace with
`albw-boot-nan`, which also watches any range of memory (`ALBW_WATCH_ADDR`, `ALBW_WATCH_LEN`,
`ALBW_WATCH_LINE`).

**Determinism** is a constraint, not a convenience. Both sides have to make the same scheduling
decision at the same instruction, or the comparison reports legitimate reordering as a
translation bug. So threads switch inside syscalls, and time advances when nothing is runnable.

The guest does not entirely cooperate: it submits GPU work and then busy-waits for its own
interrupt handler, with nothing in the loop that enters the kernel. On hardware the OS preempts
it. Here a thread can also be taken off the processor after a fixed number of **backward
branches** — a count, never a clock, so that both runs reach the threshold at the same
instruction. Anything that made this depend on time would quietly cost the project its only
defence against silent mistranslation.

## Layout

```
src/runtime/     the hand-written half: CPU state, dispatch, Horizon kernel, threads, RomFS and
                 save data, the GPU front end and both renderers, the DSP, the window and launcher
src/shaders/     the GPU renderer's shaders and FSR 1's, compiled to SPIR-V at build time
src/generated/   the translated game (not in git)
src/ffx/         what AMD's FidelityFX SDK needs to build on Linux
entries/         guest addresses the running game branched to, per release (see above)
tools/           extraction, recompiler, oracle, discovery loop, regression checks, Ghidra scripts,
                 the packaging scripts (tools/appimage: what the AppImage needs)
third_party/     AMD FSR 1, unchanged; the licences of what the packages bundle (kit/)
cmake/           build helpers
```

## Third-party code

AMD FSR 3 is built from AMD's FidelityFX SDK 1.1.4 (MIT), which is not in the repository. To have
it, fetch the parts it needs into `external/` and build again:

```bash
git clone --filter=blob:none --no-checkout --depth 1 --branch v1.1.4 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git external/fidelityfx-sdk-v1.1.4
git -C external/fidelityfx-sdk-v1.1.4 sparse-checkout set --no-cone /sdk/LICENSE.txt /sdk/include/ /sdk/src/components/fsr3upscaler/ /sdk/src/shared/ /sdk/src/backends/shared/ /sdk/src/backends/vk/ffx_vk.cpp /sdk/src/backends/vk/shaders/fsr3upscaler/
git -C external/fidelityfx-sdk-v1.1.4 checkout
```

That is a few megabytes. The SDK builds its shaders with a Windows program; `tools/ffx_permutations.py`
makes the same permutation headers with glslc instead. Two of the SDK's files are built from
copies the build makes: `ffx_types.h` with larger contexts (Linux's `wchar_t` is twice Windows'),
and the Vulkan backend with its tables aligned for GCC. `src/ffx/ffx_port_compat.h` supplies the
few functions of Microsoft's C library it calls. The upscaler runs on a Vulkan device of its own
(`src/runtime/albw_vkup.cpp`): SDL's GPU API does not share its device, so each frame's inputs are
read back from the GPU renderer and the upscaled picture comes back, on a thread of its own.

NVIDIA DLSS is built from NVIDIA's DLSS SDK, which is under NVIDIA's own licence (the NVIDIA RTX
SDKs License: NVIDIA graphics cards only, the SDK named where the program shows such things --
the launcher's first page does, in a build that has it -- and NVIDIA told before any commercial
release). Whoever fetches it accepts
that licence; nothing of it is committed. The parts the build uses, about 60 MB:

```bash
git clone --filter=blob:none --no-checkout --depth 1 --branch v310.9.16 https://github.com/NVIDIA/DLSS.git external/dlss-sdk-v310.9.16
git -C external/dlss-sdk-v310.9.16 sparse-checkout set --no-cone /LICENSE.txt /include/ /lib/Linux_x86_64/libnvsdk_ngx.a /lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16
git -C external/dlss-sdk-v310.9.16 checkout
```

DLSS then runs where NGX finds an RTX card, on the same kind of Vulkan device of the port's own as
FSR 3, and finds its library (`libnvidia-ngx-dlss.so.310.9.16`) in the SDK or beside the program.
A copy of the program that includes that library must carry the third-party notices of section 9.5
of NVIDIA's DLSS Programming Guide (in the SDK's `doc/`).

`third_party/fsr1/` is AMD's FidelityFX Super Resolution 1.0 (`ffx_a.h`, `ffx_fsr1.h`), unchanged,
under the MIT licence in `third_party/fsr1/license.txt`. The shaders in `src/shaders/` include it,
and `src/runtime/albw_upscale.c` follows its EASU and RCAS step by step in C.

## Licence

The port's own code is under the MIT licence; see `LICENSE`. Third-party code keeps its own: AMD
FSR 1 in `third_party/fsr1/` is under the MIT licence as well (`third_party/fsr1/license.txt`), and
the FidelityFX and DLSS SDKs are not part of the repository and come with their own terms (see
above). The packages carry the licences of everything they bundle, and say where its sources are;
DLSS is not in them. The licence covers this repository and nothing else: the game, its code and its assets are
Nintendo's, and nothing derived from a cartridge -- extracted files, translated code, a build of
the port -- may be redistributed.
