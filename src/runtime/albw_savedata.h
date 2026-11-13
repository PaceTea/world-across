#ifndef ALBW_SAVEDATA_H
#define ALBW_SAVEDATA_H

#include <stdint.h>

// Everything the guest asks for that is not the card image: save data, and the NAND archives a
// console has and this does not.
//
// On hardware these live in the console's own storage, and one of them -- shared extdata
// 0xF000000B, the word filter -- is system data we neither have nor could pass on. The guest
// treats a missing *archive* as fatal, so refusing outright stops the boot. Giving it a real
// but empty directory on the host lets it create what it wants to create and find nothing where
// it expects to find nothing, which is the situation on a console that has never run the game.
//
// Files land under $ALBW_SAVE_DIR, else $XDG_DATA_HOME/albw-port, else ~/.local/share/albw-port,
// one directory per archive. Guest paths are flattened and sanitised: they come from the guest
// and must not be able to name anything outside that directory.

// Opens a file inside an archive's directory. `create` makes it if absent. Returns a file
// descriptor, or -1. The directory is created on first use.
int albw_save_open(uint32_t archive_id, const char *guest_path, int create);

// Size of an open file, or 0 if it cannot be determined.
uint64_t albw_save_size(int fd);

// The file behind fd is being closed (by the caller, after this).
void albw_save_closed(int fd);

// The game is about to write to fd, or change its size. In a journaled save archive the first
// change since the last commit moves the file onto a copy of its own, under the same descriptor;
// returns 0 if that could not be done.
int albw_save_before_write(int fd);

// ControlArchive's commit: in a save archive formatted with duplicate data, what was written since
// the last commit becomes the save now, every file at once. Until then it is held aside, so that a
// run that stops in the middle of saving leaves the save as it was. Returns 1 on success.
int albw_save_commit(uint32_t archive_id);

// How a save archive was formatted, as the guest asked for it and as GetFormatInfo reports it back.
struct albw_save_format {
    uint32_t total_size, directories, files;
    uint32_t duplicate_data;
};

// Whether the archive has been formatted, and how. The record lives beside the archive's
// directory rather than in it, so that no file the guest names can be mistaken for it.
int albw_save_format_info(uint32_t archive_id, struct albw_save_format *out);

// Formats the archive: on a console that erases it. Here the files already in it are moved into a
// dated folder beside it instead, because a port's mistake about when to format should not cost
// anybody a save. Returns 1 on success.
int albw_save_format(uint32_t archive_id, const struct albw_save_format *format);

// Where the archives are kept, for the log line that says so once.
const char *albw_save_root(void);

#endif
