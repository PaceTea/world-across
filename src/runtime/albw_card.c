// The card image, kept beside the program (albw_card.h).
#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include "albw_card.h"
#include "albw_platform.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define CARD_MIN (16ull * 1024 * 1024)                    // as looks_like_card in albw_runtime.c
#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Sizes past 2 GB, which Windows' plain stat cannot give.
static uint64_t size_of(const char *path) {
#ifdef _WIN32
    struct __stat64 st;
    return _stat64(path, &st) == 0 ? (uint64_t)st.st_size : 0;
#else
    struct stat st;
    return stat(path, &st) == 0 ? (uint64_t)st.st_size : 0;
#endif
}

static char *last_separator(char *path) {
    char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    return a > b ? a : b;
}

// The directory a path is in ("" for a bare name), and the name after it.
static void parent_of(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s", path);
    char *s = last_separator(out);
    if (s) *s = '\0';
    else out[0] = '\0';
}

static const char *name_of(const char *path) {
    const char *a = strrchr(path, '/'), *b = strrchr(path, '\\');
    const char *s = a > b ? a : b;
    return s ? s + 1 : path;
}

static int same_path(const char *a, const char *b) {
#ifdef _WIN32
    for (; *a && *b; a++, b++) {
        char x = *a == '\\' ? '/' : *a, y = *b == '\\' ? '/' : *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
#else
    return !strcmp(a, b);
#endif
}

// Every directory down to `path`, as mkdir -p.
static void make_dirs(const char *path) {
    char p[2200];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/' && *s != '\\') continue;
        char c = *s;
        *s = '\0';
        if (s[-1] != ':') albw_mkdir(p);                  // not a drive's root, "C:"
        *s = c;
    }
    albw_mkdir(p);
}

// Where card folders can be, the one wanted most first: the program's directory, then the place
// for one that cannot be written to. Returns how many.
#define FOLDERS 2
static int card_folders(char out[FOLDERS][1100]) {
    int k = 0;
    char exe[1024], base[1024] = "";
    albw_exe_dir(exe, sizeof exe);
#ifndef _WIN32
    // An AppImage runs from a mount that goes when it ends; to the player the program is the file.
    // APPDIR says the program is the AppImage's own, not one started from inside another.
    const char *image = getenv("APPIMAGE"), *mount = getenv("APPDIR");
    if (image && *image && mount && *mount && exe[0] && !strncmp(exe, mount, strlen(mount)))
        parent_of(image, base, sizeof base);
    else
#endif
    if (exe[0]) {
        // A build directory in the source tree (build/, build-win/): the tree, which outlasts it.
        char up[1024], probe[1100];
        parent_of(exe, up, sizeof up);
        snprintf(probe, sizeof probe, "%s/src/runtime/albw_runtime.c", up);
        snprintf(base, sizeof base, "%s", up[0] && exists(probe) ? up : exe);
    }
    if (base[0]) snprintf(out[k++], 1100, "%s" SEP "card", base);
#ifdef _WIN32
    albw_cache_root(base, sizeof base);                   // %LOCALAPPDATA%, which does not roam
#else
    albw_data_root(base, sizeof base);
#endif
    snprintf(out[k++], 1100, "%s" SEP "card", base);
    return k;
}

// Whether files can be made in `dir`: tried, as the only answer Windows' permissions give.
static int writable(const char *dir) {
    char probe[2300];
    snprintf(probe, sizeof probe, "%s" SEP ".world-across-probe", dir);
    FILE *f = fopen(probe, "wb");
    if (!f) return 0;
    fclose(f);
    remove(probe);
    return 1;
}

int albw_card_folder(char *out, size_t n) {
    char folders[FOLDERS][1100];
    int k = card_folders(folders);
    for (int i = 0; i < k; i++) {
        if (i == 0) albw_mkdir(folders[i]);               // beside the program: that folder only
        else make_dirs(folders[i]);
        if (is_dir(folders[i]) && writable(folders[i])) {
            snprintf(out, n, "%s", folders[i]);
            return 1;
        }
    }
    return 0;
}

// Whether the file at `abs` (a real path) is in one of the card folders.
static int in_a_folder(const char *abs) {
    char folders[FOLDERS][1100], dir[1024], real[1024];
    int k = card_folders(folders);
    parent_of(abs, dir, sizeof dir);
    for (int i = 0; i < k; i++)
        if (albw_realpath(folders[i], real, sizeof real) == 0 && same_path(dir, real)) return 1;
    return 0;
}

static int move_file(const char *from, const char *to) {
#ifdef _WIN32
    // Without MOVEFILE_COPY_ALLOWED: across volumes it fails, and the copy below shows its progress.
    return MoveFileExA(from, to, MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);                              // `to` is a name nothing has yet
#endif
}

static int copy_file(const char *from, const char *to, uint64_t total, volatile float *progress,
                     char *why, size_t why_n) {
    FILE *in = fopen(from, "rb");
    if (!in) {
        snprintf(why, why_n, "%s cannot be read (%s)", from, strerror(errno));
        return 0;
    }
    FILE *out = fopen(to, "wb");
    if (!out) {
        snprintf(why, why_n, "%s cannot be written (%s)", to, strerror(errno));
        fclose(in);
        return 0;
    }
    size_t cap = 4u << 20;
    char *buf = malloc(cap);
    uint64_t done = 0;
    int ok = buf != NULL;
    while (ok) {
        size_t got = fread(buf, 1, cap, in);
        if (got == 0) break;
        if (fwrite(buf, 1, got, out) != got) ok = 0;
        done += got;
        if (progress && total) *progress = (float)((double)done / (double)total);
    }
    if (ok && ferror(in)) ok = 0;
    int err = errno;
    if (fflush(out) != 0 || albw_fsync(fileno(out)) != 0) ok = 0;
    if (fclose(out) != 0) ok = 0;
    fclose(in);
    free(buf);
    if (ok && done != total) {
        snprintf(why, why_n, "the copy came out at %llu bytes of %llu", (unsigned long long)done,
                 (unsigned long long)total);
        remove(to);
        return 0;
    }
    if (!ok) {
        snprintf(why, why_n, "copying it into %s failed (%s)", to, strerror(err ? err : errno));
        remove(to);
        return 0;
    }
    return 1;
}

int albw_card_keep(const char *from, char *out, size_t n, volatile float *progress, char *why,
                   size_t why_n) {
    char abs[1024], folder[1100], target[2400];
    why[0] = '\0';
    snprintf(out, n, "%s", from);
    if (albw_realpath(from, abs, sizeof abs) != 0) {
        snprintf(why, why_n, "the card %s is not there", from);
        return 0;
    }
    snprintf(out, n, "%s", abs);
    if (in_a_folder(abs)) return 1;
    if (!albw_card_folder(folder, sizeof folder)) {
        snprintf(why, why_n, "no card folder can be made beside the program, nor in the place for "
                             "one; the card stays at %s", abs);
        return 0;
    }

    // Its own name, or the first free one after it: "name (2).3ds".
    const char *name = name_of(abs), *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    snprintf(target, sizeof target, "%s" SEP "%s", folder, name);
    for (int i = 2; exists(target) && i < 100; i++)
        snprintf(target, sizeof target, "%s" SEP "%.*s (%d)%s", folder, stem, name, i, name + stem);
    if (exists(target)) {
        snprintf(why, why_n, "the card folder %s has 99 files of the name %s already", folder, name);
        return 0;
    }

    if (move_file(abs, target) == 0) {
        albw_fsync_dir(folder);
        snprintf(out, n, "%s", target);
        return 2;
    }
    // Another file system (or a place the file cannot be taken from): copied under a name of its
    // own, renamed once whole, and only then the original removed.
    char part[2500];
    snprintf(part, sizeof part, "%s.part", target);
    if (progress) *progress = 0.0f;
    if (!copy_file(abs, part, size_of(abs), progress, why, why_n)) return 0;
    if (albw_rename(part, target) != 0) {
        snprintf(why, why_n, "%s cannot be renamed to %s", part, target);
        remove(part);
        return 0;
    }
    albw_fsync_dir(folder);
    snprintf(out, n, "%s", target);
    if (remove(abs) != 0)
        snprintf(why, why_n, "copied; the original %s cannot be removed (%s) and stays as well", abs,
                 strerror(errno));
    return 2;
}

static int card_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    char ext[8];
    size_t i = 0;
    for (dot++; *dot && i < sizeof ext - 1; dot++) ext[i++] = (char)(*dot >= 'A' && *dot <= 'Z' ? *dot + 32 : *dot);
    ext[i] = '\0';
    return !strcmp(ext, "3ds") || !strcmp(ext, "cci");
}

int albw_card_find(char *out, size_t n, int (*accept)(const char *path)) {
    char folders[FOLDERS][1100];
    int k = card_folders(folders);
    for (int i = 0; i < k; i++) {
        DIR *d = opendir(folders[i]);
        if (!d) continue;
        char best[300] = "", path[2500];
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || !card_extension(e->d_name)) continue;
            if (best[0] && strcmp(e->d_name, best) >= 0) continue;
            snprintf(path, sizeof path, "%s" SEP "%s", folders[i], e->d_name);
            if (size_of(path) < CARD_MIN || (accept && !accept(path))) continue;
            snprintf(best, sizeof best, "%s", e->d_name);
        }
        closedir(d);
        if (best[0]) {
            snprintf(out, n, "%s" SEP "%s", folders[i], best);
            return 1;
        }
    }
    return 0;
}
