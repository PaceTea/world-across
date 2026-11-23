// The card image, kept beside the program. A card chosen anywhere -- in the launcher's file dialog
// or on the command line -- is moved into the program's card folder, so that tidying up Downloads
// or the desktop cannot take it away; a start with no card remembered, or with the remembered one
// gone, plays the one it finds there.
//
// The card folder is `card` in the program's directory: the folder WorldAcross.exe is in, the one
// the AppImage file is in, or the source tree for a build made inside it (build/ is made again
// from time to time; the tree is not). Where that cannot be written to -- a package installed under
// /opt or Program Files -- it is ~/.local/share/albw-port/card, or %LOCALAPPDATA%\albw-port\card.
#pragma once

#include <stddef.h>

// The card folder, made if it is not there yet and checked to take files. 0 if none can be had.
int albw_card_folder(char *out, size_t n);

// Moves the card at `from` into the card folder under its own name (with " (2)" and so on before
// the extension when a file of that name is there already). Within one file system that is a
// rename; across two the card is copied -- `*progress`, if given, going from 0 to 1 meanwhile --
// and the original removed once the copy is whole. `out` is where the card is afterwards.
//
// Returns 2 if it was moved now, 1 if it was in the folder already, 0 if it stays where it was
// (`why` says why). `why` can also hold a note when it was moved: a copy whose original could not
// be removed, on a medium that is read only, leaves both.
int albw_card_keep(const char *from, char *out, size_t n, volatile float *progress, char *why,
                   size_t why_n);

// A card image in the card folder: the first by name of the .3ds and .cci files there of 16 MB and
// more that `accept` (if given) takes. 0 if there is none.
int albw_card_find(char *out, size_t n, int (*accept)(const char *path));
