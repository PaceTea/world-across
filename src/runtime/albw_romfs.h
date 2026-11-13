#ifndef ALBW_ROMFS_H
#define ALBW_ROMFS_H

#include <stddef.h>
#include <stdint.h>

// The game's data lives in the RomFS of its own card image. Rather than requiring a
// separate 672 MB extraction step, the runtime maps the .3ds read-only and finds the RomFS
// partition itself - which is also what a launcher would have to do.
//
// Opened lazily: a run that never touches files does not need the image at all.
int albw_romfs_open(const char *rom_path);

// NULL until a successful open. Size is the whole RomFS partition, IVFC header included,
// because that is what the guest expects the file it opened to contain.
const uint8_t *albw_romfs_data(void);
uint64_t albw_romfs_size(void);

// A file from the card's ExeFS by name -- "icon", "banner" -- or NULL. Valid once albw_romfs_open has
// succeeded; the bytes are the card image's own and read-only.
const uint8_t *albw_exefs_file(const char *name, uint32_t *size);

#endif
