#include "albw_savedata.h"
#include "albw_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

static char root[512];

const char *albw_save_root(void) {
    if (root[0]) return root;

    const char *explicit_dir = getenv("ALBW_SAVE_DIR");
    if (explicit_dir && *explicit_dir) {
        snprintf(root, sizeof root, "%s", explicit_dir);
        return root;
    }
    albw_data_root(root, sizeof root);
    return root;
}

// Creates a directory and every parent of it. Anything already there is fine.
static int make_path(const char *path) {
    char buf[600];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        if (p[-1] == ':') continue;                      // a Windows drive's root
        char sep = *p;
        *p = '\0';
        if (albw_mkdir(buf) < 0) return 0;
        *p = sep;
    }
    return albw_mkdir(buf) == 0;
}

// The guest's path becomes one file name inside the archive's directory. Separators are
// flattened and anything outside a conservative set is replaced, so a path the guest composes
// -- including one built from data in the card image -- cannot name a file outside that
// directory however it is spelled.
static void flatten(const char *guest_path, char *out, size_t n) {
    size_t w = 0;
    const char *start = guest_path;
    while (*start == '/') start++;                       // so a save is named what it is called
    for (const char *p = start; *p && w + 1 < n; p++) {
        unsigned char ch = (unsigned char)*p;
        int safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                 || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
        out[w++] = safe ? (char)ch : '_';
    }
    if (!w) out[w++] = '_';
    out[w] = '\0';

    // A name of only dots would still refer to a directory.
    int only_dots = 1;
    for (const char *p = out; *p; p++) {
        if (*p != '.') { only_dots = 0; break; }
    }
    if (only_dots) out[0] = '_';
}

// ---------------------------------------------------------------------------------------------
// Commits.
//
// A save archive formatted with duplicate data -- this game's is -- is journaled on the console:
// what the game writes becomes the save only when it commits (fs ControlArchive), all files at
// once, and a power cut before that leaves the save as it was. Writing straight into the files, as
// this used to, left half a save behind whenever the port stopped in the middle of saving.
//
// So until the commit, a file the game changes in such an archive is a copy in archive-N.pending/
// beside the archive, made from the committed file at the first change. The commit puts a
// marker down, archive-N.committing, moves every pending file over its committed one, and takes
// the marker away. A run that stops before the marker is down leaves the old save; one that stops
// after finds the marker at the next start and finishes the moves. Pending copies without a marker
// are writes the game never committed, and are dropped at the next start, as the console drops
// them.

#define MAX_OPEN 32
static struct { int fd, pending; uint32_t archive; char name[256]; } open_files[MAX_OPEN];

static int journaled(uint32_t archive_id) {
    struct albw_save_format f;
    return albw_save_format_info(archive_id, &f) && f.duplicate_data;
}

static void archive_dir(uint32_t archive_id, char *out, size_t n) {
    snprintf(out, n, "%s/archive-%u", albw_save_root(), archive_id);
}
static void pending_dir(uint32_t archive_id, char *out, size_t n) {
    snprintf(out, n, "%s/archive-%u.pending", albw_save_root(), archive_id);
}
static void marker_path(uint32_t archive_id, char *out, size_t n) {
    snprintf(out, n, "%s/archive-%u.committing", albw_save_root(), archive_id);
}

static void sync_dir(const char *dir) {
    albw_fsync_dir(dir);
}

// A copy of a file, on disk before this returns.
static int copy_file(const char *from, const char *to) {
    int in = albw_open(from, O_RDONLY, 0644);
    if (in < 0) return 0;
    int out = albw_open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        close(in);
        return 0;
    }
    char buf[65536];
    ssize_t n;
    int ok = 1;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            ok = 0;
            break;
        }
    }
    if (n < 0) ok = 0;
    if (albw_fsync(out) < 0) ok = 0;
    close(in);
    close(out);
    return ok;
}

static int skip_entry(const struct dirent *e) {
    return e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]));
}

// Moves every pending file over its committed one.
static int apply_pending(uint32_t archive_id) {
    char pend[600], dir[600];
    pending_dir(archive_id, pend, sizeof pend);
    archive_dir(archive_id, dir, sizeof dir);
    if (!make_path(dir)) return 0;
    DIR *d = opendir(pend);
    if (!d) return 1;
    int ok = 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (skip_entry(e)) continue;
        char from[900], to[900];
        snprintf(from, sizeof from, "%s/%s", pend, e->d_name);
        snprintf(to, sizeof to, "%s/%s", dir, e->d_name);
        if (albw_rename(from, to) < 0) {
            printf("[save ] cannot move %s into the save: %s\n", from, strerror(errno));
            ok = 0;
        }
    }
    closedir(d);
    sync_dir(dir);
    return ok;
}

// Once per archive and run, before its first file is opened: finish a commit the last run was in
// the middle of, and drop what it wrote and never committed.
static void recover(uint32_t archive_id) {
    static uint32_t done;
    if (archive_id < 32) {
        if ((done >> archive_id) & 1u) return;
        done |= 1u << archive_id;
    }
    char marker[600], pend[600];
    marker_path(archive_id, marker, sizeof marker);
    pending_dir(archive_id, pend, sizeof pend);
    if (access(marker, F_OK) == 0) {
        printf("[save ] the last run stopped while saving; finishing that save\n");
        if (apply_pending(archive_id)) {
            unlink(marker);
            sync_dir(albw_save_root());
        }
    }
    DIR *d = opendir(pend);
    if (!d) return;
    int dropped = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (skip_entry(e)) continue;
        char path[900];
        snprintf(path, sizeof path, "%s/%s", pend, e->d_name);
        if (unlink(path) == 0) dropped++;
    }
    closedir(d);
    rmdir(pend);
    if (dropped) {
        printf("[save ] %d file(s) the last run wrote and never saved were dropped\n", dropped);
    }
}

static void track(int fd, uint32_t archive_id, const char *name, int pending) {
    for (int i = 0; i < MAX_OPEN; i++) {
        if (open_files[i].fd > 0) continue;
        open_files[i].fd = fd;
        open_files[i].pending = pending;
        open_files[i].archive = archive_id;
        snprintf(open_files[i].name, sizeof open_files[i].name, "%s", name);
        return;
    }
}

void albw_save_closed(int fd) {
    for (int i = 0; i < MAX_OPEN; i++) {
        if (open_files[i].fd == fd) open_files[i].fd = 0;
    }
}

// Puts `path`, opened with `flags`, under the descriptor fd.
static int reopen_as(int fd, const char *path, int flags) {
    int nfd = albw_open(path, flags, 0644);
    if (nfd < 0) return 0;
    int ok = dup2(nfd, fd) >= 0;
    close(nfd);
    return ok;
}

int albw_save_before_write(int fd) {
    for (int i = 0; i < MAX_OPEN; i++) {
        if (open_files[i].fd != fd) continue;
        if (open_files[i].pending) return 1;
        char dir[600], pend[600], from[900], to[900];
        archive_dir(open_files[i].archive, dir, sizeof dir);
        pending_dir(open_files[i].archive, pend, sizeof pend);
        snprintf(from, sizeof from, "%s/%s", dir, open_files[i].name);
        snprintf(to, sizeof to, "%s/%s", pend, open_files[i].name);
        if (!make_path(pend) || !copy_file(from, to) || !reopen_as(fd, to, O_RDWR)) {
            printf("[save ] cannot set %s aside for writing: %s\n", from, strerror(errno));
            return 0;
        }
        open_files[i].pending = 1;
        return 1;
    }
    return 1;                                            // not in a journaled archive
}

int albw_save_commit(uint32_t archive_id) {
    if (!journaled(archive_id)) return 1;
    char pend[600], marker[600];
    pending_dir(archive_id, pend, sizeof pend);
    marker_path(archive_id, marker, sizeof marker);
    DIR *d = opendir(pend);
    if (!d) return 1;                                    // nothing written since the last commit

    // Every pending file on disk before the marker says to use them.
    struct dirent *e;
    while ((e = readdir(d))) {
        if (skip_entry(e)) continue;
        char path[900];
        snprintf(path, sizeof path, "%s/%s", pend, e->d_name);
        int fd = albw_open(path, O_RDONLY, 0644);
        if (fd >= 0) {
            albw_fsync(fd);
            close(fd);
        }
    }
    closedir(d);
    sync_dir(pend);
    int m = albw_open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (m < 0) {
        printf("[save ] cannot commit archive %u: %s\n", archive_id, strerror(errno));
        return 0;
    }
    albw_fsync(m);
    close(m);
    sync_dir(albw_save_root());
    if (!apply_pending(archive_id)) return 0;            // the marker stays: the next start retries
    unlink(marker);
    sync_dir(albw_save_root());

    // A file still open now reads the committed file again, read-only, until its next change.
    char dir[600];
    archive_dir(archive_id, dir, sizeof dir);
    for (int i = 0; i < MAX_OPEN; i++) {
        if (open_files[i].fd <= 0 || open_files[i].archive != archive_id) continue;
        char path[900];
        snprintf(path, sizeof path, "%s/%s", dir, open_files[i].name);
        reopen_as(open_files[i].fd, path, O_RDONLY);
        open_files[i].pending = 0;
    }
    rmdir(pend);
    printf("[save ] archive %u committed\n", archive_id);
    return 1;
}

int albw_save_open(uint32_t archive_id, const char *guest_path, int create) {
    static int announced;
    if (!announced) {
        announced = 1;
        printf("[save ] archives are kept in %s\n", albw_save_root());
    }

    char dir[560];
    snprintf(dir, sizeof dir, "%s/archive-%u", albw_save_root(), archive_id);
    if (!make_path(dir)) {
        printf("[save ] cannot create %s: %s\n", dir, strerror(errno));
        return -1;
    }

    char name[256];
    flatten(guest_path, name, sizeof name);

    char full[900];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    if (journaled(archive_id)) {
        recover(archive_id);
        char pend[600], copy[900];
        pending_dir(archive_id, pend, sizeof pend);
        snprintf(copy, sizeof copy, "%s/%s", pend, name);
        // Changed since the last commit: its copy. Committed: the file itself, read-only, until
        // albw_save_before_write moves it onto a copy. Neither: made, as a change, when asked to.
        int fd, pending;
        if (access(copy, F_OK) == 0) {
            fd = albw_open(copy, O_RDWR, 0644);
            pending = 1;
        } else if (access(full, F_OK) == 0) {
            fd = albw_open(full, O_RDONLY, 0644);
            pending = 0;
        } else if (create) {
            fd = make_path(pend) ? albw_open(copy, O_RDWR | O_CREAT, 0644) : -1;
            pending = 1;
        } else {
            printf("[save ] %s: %s\n", full, strerror(ENOENT));
            return -1;
        }
        if (fd < 0) {
            printf("[save ] %s: %s\n", pending ? copy : full, strerror(errno));
            return -1;
        }
        track(fd, archive_id, name, pending);
        printf("[save ] %s \"%s\"%s\n", create ? "created" : "opened", guest_path,
               pending ? " (a change, until the game commits it)" : "");
        return fd;
    }
    int fd = albw_open(full, create ? (O_RDWR | O_CREAT) : O_RDWR, 0644);
    if (fd < 0 && !create) fd = albw_open(full, O_RDONLY, 0644);
    if (fd < 0) {
        printf("[save ] %s: %s\n", full, strerror(errno));
        return -1;
    }
    printf("[save ] %s \"%s\" as %s\n", create ? "created" : "opened", guest_path, full);
    return fd;
}

uint64_t albw_save_size(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 ? (uint64_t)st.st_size : 0;
}

// ---------------------------------------------------------------------------------------------
// Formatting.
//
// The game asks GetFormatInfo what its save archive was formatted with, and formats it when the
// answer is not what it wants -- which is how a cartridge knows it is in a console for the first
// time. With no answer it formatted on every boot. That only did no harm because formatting did
// nothing; the two have to be right together, or a correct format becomes a wiped save each boot.

static void format_record_path(uint32_t archive_id, char *out, size_t n) {
    snprintf(out, n, "%s/archive-%u.format", albw_save_root(), archive_id);
}

int albw_save_format_info(uint32_t archive_id, struct albw_save_format *out) {
    char path[600];
    format_record_path(archive_id, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    struct albw_save_format got = { 0 };
    int n = fscanf(f, "%u %u %u %u", &got.total_size, &got.directories, &got.files,
                   &got.duplicate_data);
    fclose(f);
    if (n != 4) return 0;
    if (out) *out = got;
    return 1;
}

int albw_save_format(uint32_t archive_id, const struct albw_save_format *format) {
    char dir[560];
    snprintf(dir, sizeof dir, "%s/archive-%u", albw_save_root(), archive_id);
    if (!make_path(dir)) return 0;

    // Whatever is there already goes into a folder of its own, named for when it was moved.
    DIR *d = opendir(dir);
    if (d) {
        char aside[640] = "";
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) {
                continue;
            }
            if (!aside[0]) {
                time_t now = time(NULL);
                struct tm tm;
                albw_localtime(&now, &tm);
                snprintf(aside, sizeof aside, "%s/archive-%u.before-format-%04d%02d%02d-%02d%02d%02d",
                         albw_save_root(), archive_id, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec);
                if (!make_path(aside)) {
                    printf("[save ] cannot set the old files aside in %s; not formatting\n", aside);
                    closedir(d);
                    return 0;
                }
                printf("[save ] formatting archive %u: what was in it is now in %s\n", archive_id,
                       aside);
            }
            char from[900], to[900];
            snprintf(from, sizeof from, "%s/%s", dir, e->d_name);
            snprintf(to, sizeof to, "%s/%s", aside, e->d_name);
            if (albw_rename(from, to) < 0) {
                printf("[save ] cannot move %s aside: %s; not formatting\n", from, strerror(errno));
                closedir(d);
                return 0;
            }
        }
        closedir(d);
    }

    // Formatting starts the archive afresh: nothing pending, no commit half done.
    char pend[600], marker[600];
    pending_dir(archive_id, pend, sizeof pend);
    marker_path(archive_id, marker, sizeof marker);
    unlink(marker);
    DIR *pd = opendir(pend);
    if (pd) {
        struct dirent *e;
        while ((e = readdir(pd))) {
            if (skip_entry(e)) continue;
            char p2[900];
            snprintf(p2, sizeof p2, "%s/%s", pend, e->d_name);
            unlink(p2);
        }
        closedir(pd);
        rmdir(pend);
    }

    char path[600];
    format_record_path(archive_id, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "%u %u %u %u\n", format->total_size, format->directories, format->files,
            format->duplicate_data);
    fclose(f);
    printf("[save ] archive %u formatted: %u directories, %u files, %u bytes\n", archive_id,
           format->directories, format->files, format->total_size);
    return 1;
}
