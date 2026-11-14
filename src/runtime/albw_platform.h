#ifndef ALBW_PLATFORM_H
#define ALBW_PLATFORM_H

// What differs between the systems the port runs on, in one place: Linux (and anything POSIX), and
// Windows. Everything else is written against these.
//
// The differences that matter are few but sharp. Windows opens files as text unless told
// otherwise, will not rename onto a file that exists or that anyone holds open, has no
// positional reads on a descriptor, keeps its per-user files under %APPDATA% rather than
// ~/.config and ~/.local/share, and has neither mmap nor futexes, only their equivalents.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#ifdef _WIN32
#include <io.h>
typedef long long albw_ssize;
#else
#include <unistd.h>
typedef ssize_t albw_ssize;
#endif

// Called first thing in main(): on Windows, files are opened as binary unless they say otherwise.
void albw_platform_init(void);

// Memory: `size` bytes, read-write and zero, committed as they are touched. NULL on failure.
void *albw_reserve(size_t size);

// A whole file mapped read-only; NULL on failure. The mapping lasts as long as the process.
const uint8_t *albw_map_file(const char *path, uint64_t *size);

// Waiting on a word: blocks while *word == value (or until woken, or spuriously); wakes all.
void albw_wait_on(volatile uint32_t *word, uint32_t value);
void albw_wake_all(volatile uint32_t *word);

int albw_cpu_count(void);                              // logical processors

// Files by descriptor, with the semantics the save code relies on. albw_open opens binary and, on
// Windows, so that the file can be renamed over and deleted while it is open, as POSIX allows.
int albw_open(const char *path, int flags, int mode);
albw_ssize albw_pread(int fd, void *buf, size_t n, uint64_t offset);
albw_ssize albw_pwrite(int fd, const void *buf, size_t n, uint64_t offset);
int albw_ftruncate(int fd, uint64_t size);
int albw_fsync(int fd);
// Makes a directory's entries durable (POSIX: fsync on the directory); nothing on Windows, whose
// renames below are written through.
void albw_fsync_dir(const char *dir);
// rename(), replacing `to` if it exists -- atomically, as POSIX's does.
int albw_rename(const char *from, const char *to);
int albw_mkdir(const char *path);                      // one level; 0 if made or already there
int albw_realpath(const char *path, char *out, size_t n);

// Where the port keeps its things: settings and the card's path (~/.config/albw-port, or
// %APPDATA%\albw-port), and saves (~/.local/share/albw-port, or %APPDATA%\albw-port\saves).
// `name` is appended after a separator; an empty name gives the directory itself.
void albw_config_path(char *out, size_t n, const char *name);
void albw_data_root(char *out, size_t n);
// The directory the running program is in, or "" if it cannot be told.
void albw_exe_dir(char *out, size_t n);

int albw_setenv(const char *name, const char *value);

// Memory aligned to `align` bytes (a power of two), and its release: Windows' C library has no
// aligned_alloc, since its free() could not release such a block.
#ifdef _WIN32
#include <malloc.h>
#define albw_aligned_alloc(align, size) _aligned_malloc((size), (align))
#define albw_aligned_free(p) _aligned_free(p)
#else
#include <stdlib.h>
#define albw_aligned_alloc(align, size) aligned_alloc((align), (size))
#define albw_aligned_free(p) free(p)
#endif

// localtime_r, which Windows calls localtime_s and takes the other way round.
#ifdef _WIN32
#define albw_localtime(t, tm) (localtime_s((tm), (t)) == 0 ? (tm) : NULL)
#else
#define albw_localtime(t, tm) localtime_r((t), (tm))
#endif

#endif
