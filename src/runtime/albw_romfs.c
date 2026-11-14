// Locates the RomFS inside a .3ds card image.
//
// The layout is two nested containers: an NCSD image holds up to eight partitions, and
// partition 0 is an NCCH whose header gives the RomFS offset and size in media units of
// 0x200 bytes. Both offsets are relative to their own container, so they add.
//
// This refuses an encrypted image rather than producing nonsense: without the NoCrypto flag
// the RomFS bytes are ciphertext, and the failure would otherwise look like a corrupt
// filesystem much later on.
#include "albw_romfs.h"

#include "albw_platform.h"

#include <stdio.h>
#include <string.h>

#define MEDIA_UNIT 0x200u

static const uint8_t *rom_map;
static size_t rom_len;
static const uint8_t *ncch_map;                          // partition 0, once validated
static size_t ncch_len;
static const uint8_t *romfs_base;
static uint64_t romfs_len;

static uint32_t read32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t read64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

int albw_romfs_open(const char *rom_path) {
    if (romfs_base) return 1;
    if (!rom_path) {
        fprintf(stderr, "[romfs] no card image given; pass it as the second argument\n");
        return 0;
    }

    uint64_t size = 0;
    const uint8_t *map = albw_map_file(rom_path, &size);
    if (!map) {
        fprintf(stderr, "[romfs] cannot map the card image %s\n", rom_path);
        return 0;
    }
    if (size < 0x8000) {
        fprintf(stderr, "[romfs] %s is too small to be a card image\n", rom_path);
        return 0;
    }
    rom_len = (size_t)size;
    rom_map = map;

    if (memcmp(rom_map + 0x100, "NCSD", 4)) {
        fprintf(stderr, "[romfs] %s does not start with an NCSD header\n", rom_path);
        return 0;
    }
    uint32_t part_off = read32(rom_map + 0x120) * MEDIA_UNIT;
    if (part_off + 0x200 > rom_len) {
        fprintf(stderr, "[romfs] partition 0 lies outside the image\n");
        return 0;
    }

    const uint8_t *ncch = rom_map + part_off;
    if (memcmp(ncch + 0x100, "NCCH", 4)) {
        fprintf(stderr, "[romfs] partition 0 has no NCCH header\n");
        return 0;
    }
    // Flags byte 7, bit 2 is NoCrypto. Anything else means the contents are encrypted.
    if (!(ncch[0x188 + 7] & 0x04)) {
        fprintf(stderr, "[romfs] %s is encrypted; decrypt it first "
                        "(the RomFS would otherwise read as noise)\n", rom_path);
        return 0;
    }
    ncch_map = ncch;
    ncch_len = rom_len - part_off;

    uint64_t off = (uint64_t)read32(ncch + 0x1B0) * MEDIA_UNIT;
    uint64_t len = (uint64_t)read32(ncch + 0x1B4) * MEDIA_UNIT;
    if (!off || !len || part_off + off + len > rom_len) {
        fprintf(stderr, "[romfs] the NCCH declares a RomFS that does not fit the image\n");
        return 0;
    }

    const uint8_t *ivfc = ncch + off;
    if (memcmp(ivfc, "IVFC", 4)) {
        fprintf(stderr, "[romfs] the RomFS does not start with an IVFC header\n");
        return 0;
    }

    // What the `romfs:` archive hands an application is the level 3 data, not the IVFC
    // container around it: the FS service walks the hash levels itself, so offset 0 of the
    // file the guest opens is the level 3 metadata header. Serving the IVFC header there
    // instead gave the guest its magic where a header length belonged, and it went on to read
    // a directory entry whose name length was nonsense.
    //
    // Level 3 begins right after the header and the master hash, aligned up to the level's
    // block size. The hash levels follow it, so the level offsets in the IVFC header describe
    // that trailing area and are not what locates the data.
    uint32_t master_hash = read32(ivfc + 0x08);
    uint32_t block_log = read32(ivfc + 0x0C + 2 * 0x18 + 0x10);
    uint64_t block = 1ull << block_log;
    uint64_t level3 = (0x60 + master_hash + block - 1) / block * block;
    uint64_t level3_len = read64(ivfc + 0x0C + 2 * 0x18 + 0x08);

    if (block_log > 31 || level3 + level3_len > len) {
        fprintf(stderr, "[romfs] the IVFC header describes a level 3 that does not fit\n");
        return 0;
    }
    // The level 3 header starts with its own length. Checking it turns a wrong window into a
    // refusal here rather than into garbage the guest tries to make sense of.
    if (read32(ivfc + level3) != 0x28) {
        fprintf(stderr, "[romfs] level 3 at +0x%llx does not start with a 0x28-byte header\n",
                (unsigned long long)level3);
        return 0;
    }

    romfs_base = ivfc + level3;
    romfs_len = level3_len;
    printf("[romfs] %s: RomFS at 0x%08llx, level 3 at +0x%llx, %llu MiB\n", rom_path,
           (unsigned long long)(part_off + off),
           (unsigned long long)level3,
           (unsigned long long)(level3_len >> 20));
    return 1;
}

const uint8_t *albw_romfs_data(void) { return romfs_base; }
uint64_t albw_romfs_size(void) { return romfs_len; }

// One file out of the ExeFS: a table of ten entries -- an eight-byte name, an offset and a size -- in
// the first 0x200 bytes, with the files after it. The game's icon lives here, and the icon carries the
// region the card was made for, which is the one thing the console's settings have to agree with.
const uint8_t *albw_exefs_file(const char *name, uint32_t *size) {
    if (!ncch_map) return NULL;
    uint64_t off = (uint64_t)read32(ncch_map + 0x1A0) * MEDIA_UNIT;
    uint64_t len = (uint64_t)read32(ncch_map + 0x1A4) * MEDIA_UNIT;
    if (!off || len < 0x200 || off + len > ncch_len) return NULL;
    const uint8_t *exefs = ncch_map + off;
    for (int i = 0; i < 10; i++) {
        const uint8_t *entry = exefs + i * 16;
        if (strncmp((const char *)entry, name, 8)) continue;
        uint32_t at = read32(entry + 8), n = read32(entry + 12);
        if (0x200ull + at + n > len) return NULL;
        if (size) *size = n;
        return exefs + 0x200 + at;
    }
    return NULL;
}
