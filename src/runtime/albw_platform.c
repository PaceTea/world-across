// The systems the port runs on (albw_platform.h): POSIX first, then Windows.
#ifndef _WIN32
#define _GNU_SOURCE
#endif

#include "albw_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
// ---------------------------------------------------------------------------------------------
// POSIX.
#include <sys/mman.h>
#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#else
#include <sched.h>
#endif

void albw_platform_init(void) {}

void *albw_reserve(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                   -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

const uint8_t *albw_map_file(const char *path, uint64_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *size = (uint64_t)st.st_size;
    return p;
}

void albw_wait_on(volatile uint32_t *word, uint32_t value) {
#ifdef __linux__
    syscall(SYS_futex, (uint32_t *)word, FUTEX_WAIT_PRIVATE, value, NULL, NULL, 0);
#else
    if (*word == value) sched_yield();
#endif
}

void albw_wake_all(volatile uint32_t *word) {
#ifdef __linux__
    syscall(SYS_futex, (uint32_t *)word, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
#else
    (void)word;
#endif
}

int albw_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

int albw_open(const char *path, int flags, int mode) { return open(path, flags, mode); }

albw_ssize albw_pread(int fd, void *buf, size_t n, uint64_t offset) {
    return pread(fd, buf, n, (off_t)offset);
}

albw_ssize albw_pwrite(int fd, const void *buf, size_t n, uint64_t offset) {
    return pwrite(fd, buf, n, (off_t)offset);
}

int albw_ftruncate(int fd, uint64_t size) { return ftruncate(fd, (off_t)size); }
int albw_fsync(int fd) { return fsync(fd); }

void albw_fsync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

int albw_rename(const char *from, const char *to) { return rename(from, to); }

int albw_mkdir(const char *path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

int albw_realpath(const char *path, char *out, size_t n) {
    char *r = realpath(path, NULL);
    if (!r) return -1;
    snprintf(out, n, "%s", r);
    free(r);
    return 0;
}

void albw_config_path(char *out, size_t n, const char *name) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/albw-port/%s", xdg, name);
    else snprintf(out, n, "%s/.config/albw-port/%s", home ? home : ".", name);
}

void albw_data_root(char *out, size_t n) {
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/albw-port", xdg);
    else snprintf(out, n, "%s/.local/share/albw-port", home ? home : ".");
}

void albw_exe_dir(char *out, size_t n) {
    char exe[1024];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    out[0] = '\0';
    if (len <= 0) return;
    exe[len] = '\0';
    char *slash = strrchr(exe, '/');
    if (!slash) return;
    *slash = '\0';
    snprintf(out, n, "%s", exe);
}

int albw_setenv(const char *name, const char *value) { return setenv(name, value, 1); }

void albw_cache_root(char *out, size_t n) {
    const char *xdg = getenv("XDG_CACHE_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, n, "%s/albw-port", xdg);
    else snprintf(out, n, "%s/.cache/albw-port", home ? home : ".");
}

#include <dlfcn.h>
void *albw_library_open(const char *path, char *why, size_t n) {
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib && why) snprintf(why, n, "%s", dlerror());
    return lib;
}

void *albw_library_symbol(void *library, const char *name) { return dlsym(library, name); }

#include <sys/wait.h>
int albw_run(const char *const *argv, const char *cwd, const char *log) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Only what is safe between fork and exec.
        if (cwd && chdir(cwd) != 0) _exit(126);
        int out = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        if (out >= 0) {
            dup2(out, 1);
            dup2(out, 2);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
}

uint64_t albw_memory_available(void) {
    // What the kernel reckons can be had without swapping: free memory and the caches it would
    // give up.
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        unsigned long long kb;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                fclose(f);
                return (uint64_t)kb * 1024;
            }
        }
        fclose(f);
    }
    long pages = sysconf(_SC_AVPHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    return pages > 0 && size > 0 ? (uint64_t)pages * (uint64_t)size : 0;
}

#else
// ---------------------------------------------------------------------------------------------
// Windows. Paths are UTF-8 throughout: the program's manifest sets the process's code page to
// UTF-8, so the narrow functions take them as they are.
#include <direct.h>
#include <windows.h>

static const char *app_data(void) {
    const char *d = getenv("APPDATA");
    if (!d || !*d) d = getenv("USERPROFILE");
    return d && *d ? d : ".";
}

void albw_platform_init(void) {
    // Binary unless a file says otherwise: open() and fopen() would otherwise turn every 0x0A in a
    // save file into two bytes.
    _set_fmode(_O_BINARY);
    // A program with a window has no console of its own. Handed output by whoever started it (a
    // redirection, a pipe, Wine's terminal), it keeps that; started from a console, it writes
    // there; otherwise its log goes to last-run.log beside its settings, for when something goes
    // wrong.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (freopen("CONOUT$", "w", stdout)) setvbuf(stdout, NULL, _IONBF, 0);
        if (freopen("CONOUT$", "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
        return;
    }
    char dir[1100], path[1100];
    snprintf(dir, sizeof dir, "%s/albw-port", app_data());
    _mkdir(dir);
    albw_config_path(path, sizeof path, "last-run.log");
    if (freopen(path, "w", stdout)) {
        setvbuf(stdout, NULL, _IONBF, 0);
        _dup2(_fileno(stdout), _fileno(stderr));
        setvbuf(stderr, NULL, _IONBF, 0);
    }
}

void *albw_reserve(size_t size) {
    // Committed whole, but a page only takes memory once it is touched, as with mmap.
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

const uint8_t *albw_map_file(const char *path, uint64_t *size) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) {
        CloseHandle(f);
        return NULL;
    }
    HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(f);                                      // the mapping keeps the file
    if (!m) return NULL;
    void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);                                      // and the view the mapping
    if (!p) return NULL;
    *size = (uint64_t)sz.QuadPart;
    return p;
}

void albw_wait_on(volatile uint32_t *word, uint32_t value) {
    WaitOnAddress((volatile VOID *)word, &value, sizeof value, INFINITE);
}

void albw_wake_all(volatile uint32_t *word) { WakeByAddressAll((PVOID)word); }

int albw_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (int)n : 1;
}

int albw_open(const char *path, int flags, int mode) {
    (void)mode;
    DWORD access;
    switch (flags & (O_RDONLY | O_WRONLY | O_RDWR)) {
        case O_RDONLY: access = GENERIC_READ; break;
        case O_WRONLY: access = GENERIC_WRITE; break;
        default: access = GENERIC_READ | GENERIC_WRITE; break;
    }
    DWORD disposition;
    if ((flags & O_CREAT) && (flags & O_EXCL)) disposition = CREATE_NEW;
    else if ((flags & O_CREAT) && (flags & O_TRUNC)) disposition = CREATE_ALWAYS;
    else if (flags & O_CREAT) disposition = OPEN_ALWAYS;
    else if (flags & O_TRUNC) disposition = TRUNCATE_EXISTING;
    else disposition = OPEN_EXISTING;
    // Shared for deleting as well: the save code renames finished files over ones it still has
    // open, which POSIX allows and Windows refuses without it.
    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT
              : e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS ? EEXIST : EACCES;
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_BINARY | (access == GENERIC_READ ? _O_RDONLY : 0)
                                          | ((flags & O_APPEND) ? _O_APPEND : 0));
    if (fd < 0) CloseHandle(h);
    return fd;
}

albw_ssize albw_pread(int fd, void *buf, size_t n, uint64_t offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED o;
    memset(&o, 0, sizeof o);
    o.Offset = (DWORD)offset;
    o.OffsetHigh = (DWORD)(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, &o)) return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    return (albw_ssize)got;
}

albw_ssize albw_pwrite(int fd, const void *buf, size_t n, uint64_t offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED o;
    memset(&o, 0, sizeof o);
    o.Offset = (DWORD)offset;
    o.OffsetHigh = (DWORD)(offset >> 32);
    DWORD put = 0;
    if (!WriteFile(h, buf, (DWORD)n, &put, &o)) return -1;
    return (albw_ssize)put;
}

int albw_ftruncate(int fd, uint64_t size) { return _chsize_s(fd, (long long)size) == 0 ? 0 : -1; }
int albw_fsync(int fd) { return _commit(fd); }
void albw_fsync_dir(const char *dir) { (void)dir; }

int albw_rename(const char *from, const char *to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
}

int albw_mkdir(const char *path) { return _mkdir(path) == 0 || errno == EEXIST ? 0 : -1; }

int albw_realpath(const char *path, char *out, size_t n) {
    if (!_fullpath(out, path, n)) return -1;
    DWORD attrs = GetFileAttributesA(out);
    return attrs == INVALID_FILE_ATTRIBUTES ? -1 : 0;
}

void albw_config_path(char *out, size_t n, const char *name) {
    snprintf(out, n, "%s/albw-port/%s", app_data(), name);
}

void albw_data_root(char *out, size_t n) { snprintf(out, n, "%s/albw-port/saves", app_data()); }

void albw_exe_dir(char *out, size_t n) {
    char exe[MAX_PATH * 4];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof exe);
    out[0] = '\0';
    if (!len || len >= sizeof exe) return;
    exe[len] = '\0';
    char *slash = strrchr(exe, '\\');
    char *other = strrchr(exe, '/');
    if (other > slash) slash = other;
    if (!slash) return;
    *slash = '\0';
    snprintf(out, n, "%s", exe);
}

int albw_setenv(const char *name, const char *value) { return _putenv_s(name, value) == 0 ? 0 : -1; }

void albw_cache_root(char *out, size_t n) {
    const char *d = getenv("LOCALAPPDATA");
    snprintf(out, n, "%s/albw-port", d && *d ? d : app_data());
}

void *albw_library_open(const char *path, char *why, size_t n) {
    HMODULE lib = LoadLibraryA(path);
    if (!lib && why) snprintf(why, n, "LoadLibrary failed (%lu)", (unsigned long)GetLastError());
    return (void *)lib;
}

void *albw_library_symbol(void *library, const char *name) {
    return (void *)GetProcAddress((HMODULE)library, name);
}

// One argument as Windows' C runtimes take a command line apart again: in quotes when it has a
// space, a tab or a quote, with backslashes doubled where they come before a quote.
static void quote_arg(char *out, size_t n, size_t *at, const char *arg) {
#define PUT(ch) do { if (*at + 1 < n) out[(*at)++] = (ch); } while (0)
    int plain = *arg != 0 && !strpbrk(arg, " \t\"");
    if (plain) {
        for (const char *p = arg; *p; p++) PUT(*p);
        return;
    }
    PUT('"');
    for (const char *p = arg;; p++) {
        size_t slashes = 0;
        while (*p == '\\') {
            slashes++;
            p++;
        }
        if (!*p) {
            for (size_t i = 0; i < slashes * 2; i++) PUT('\\');
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < slashes * 2 + 1; i++) PUT('\\');
        } else {
            for (size_t i = 0; i < slashes; i++) PUT('\\');
        }
        PUT(*p);
    }
    PUT('"');
#undef PUT
}

int albw_run(const char *const *argv, const char *cwd, const char *log) {
    // One command line per call: the builder runs several at once.
    enum { LINE = 32768 };
    char *line = malloc(LINE);
    if (!line) return -1;
    size_t at = 0;
    for (int i = 0; argv[i]; i++) {
        if (i && at + 1 < LINE) line[at++] = ' ';
        quote_arg(line, LINE, &at, argv[i]);
    }
    line[at] = '\0';
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE out = CreateFileA(log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = out;
    si.hStdError = out;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
    free(line);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

uint64_t albw_memory_available(void) {
    MEMORYSTATUSEX m;
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? (uint64_t)m.ullAvailPhys : 0;
}
#endif
