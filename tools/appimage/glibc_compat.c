// glibc_compat.c -- linked into the AppImage's world-across (tools/make_appimage.sh), which is to start
// on any distribution with glibc 2.34 or later.
//
// glibc_floor.h binds every function the program calls to the version it had in 2.34; these are
// the ones it did not have yet, which the headers of a newer glibc make a program call all the same:
// the C23 strtol and scanf variants (they read a 0b prefix as well, which nothing here gives them),
// strlcpy and its kin, and __memcmpeq, which the compiler calls for memcmp(...) == 0. Defined in
// the program, they are what its calls -- and SDL's, linked in with it -- resolve to.
//
// Compiled as plain C11, without _GNU_SOURCE, so that the functions called here are the old ones.
#undef _GNU_SOURCE
#define _DEFAULT_SOURCE
#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// Here the plain names have to be the plain functions, or each of these would call itself.
#if defined(__GLIBC_USE) && __GLIBC_USE(C23_STRTOL)
#error "glibc_compat.c is compiled with the C23 strtol variants switched on"
#endif

long __isoc23_strtol(const char *s, char **end, int base) { return strtol(s, end, base); }
unsigned long __isoc23_strtoul(const char *s, char **end, int base) { return strtoul(s, end, base); }
long long __isoc23_strtoll(const char *s, char **end, int base) { return strtoll(s, end, base); }
unsigned long long __isoc23_strtoull(const char *s, char **end, int base) {
    return strtoull(s, end, base);
}
intmax_t __isoc23_strtoimax(const char *s, char **end, int base) { return strtoimax(s, end, base); }
uintmax_t __isoc23_strtoumax(const char *s, char **end, int base) { return strtoumax(s, end, base); }
long __isoc23_wcstol(const wchar_t *s, wchar_t **end, int base) { return wcstol(s, end, base); }
unsigned long __isoc23_wcstoul(const wchar_t *s, wchar_t **end, int base) {
    return wcstoul(s, end, base);
}
long long __isoc23_wcstoll(const wchar_t *s, wchar_t **end, int base) { return wcstoll(s, end, base); }
unsigned long long __isoc23_wcstoull(const wchar_t *s, wchar_t **end, int base) {
    return wcstoull(s, end, base);
}
intmax_t __isoc23_wcstoimax(const wchar_t *s, wchar_t **end, int base) {
    return wcstoimax(s, end, base);
}
uintmax_t __isoc23_wcstoumax(const wchar_t *s, wchar_t **end, int base) {
    return wcstoumax(s, end, base);
}

int __isoc23_vsscanf(const char *s, const char *format, va_list ap) { return vsscanf(s, format, ap); }
int __isoc23_vfscanf(FILE *f, const char *format, va_list ap) { return vfscanf(f, format, ap); }
int __isoc23_vscanf(const char *format, va_list ap) { return vscanf(format, ap); }
int __isoc23_vswscanf(const wchar_t *s, const wchar_t *format, va_list ap) {
    return vswscanf(s, format, ap);
}
int __isoc23_vfwscanf(FILE *f, const wchar_t *format, va_list ap) { return vfwscanf(f, format, ap); }
int __isoc23_vwscanf(const wchar_t *format, va_list ap) { return vwscanf(format, ap); }

#define VARIADIC(call)                                                                             \
    va_list ap;                                                                                    \
    va_start(ap, format);                                                                          \
    int n = call;                                                                                  \
    va_end(ap);                                                                                    \
    return n
int __isoc23_sscanf(const char *s, const char *format, ...) { VARIADIC(vsscanf(s, format, ap)); }
int __isoc23_fscanf(FILE *f, const char *format, ...) { VARIADIC(vfscanf(f, format, ap)); }
int __isoc23_scanf(const char *format, ...) { VARIADIC(vscanf(format, ap)); }
int __isoc23_swscanf(const wchar_t *s, const wchar_t *format, ...) {
    VARIADIC(vswscanf(s, format, ap));
}
int __isoc23_fwscanf(FILE *f, const wchar_t *format, ...) { VARIADIC(vfwscanf(f, format, ap)); }
int __isoc23_wscanf(const wchar_t *format, ...) { VARIADIC(vwscanf(format, ap)); }

size_t strlcpy(char *restrict dst, const char *restrict src, size_t size) {
    size_t length = strlen(src);
    if (size) {
        size_t n = length < size - 1 ? length : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return length;
}
size_t strlcat(char *restrict dst, const char *restrict src, size_t size) {
    size_t used = strnlen(dst, size);
    if (used == size) return size + strlen(src);
    return used + strlcpy(dst + used, src, size - used);
}
size_t wcslcpy(wchar_t *restrict dst, const wchar_t *restrict src, size_t size) {
    size_t length = wcslen(src);
    if (size) {
        size_t n = length < size - 1 ? length : size - 1;
        wmemcpy(dst, src, n);
        dst[n] = L'\0';
    }
    return length;
}
size_t wcslcat(wchar_t *restrict dst, const wchar_t *restrict src, size_t size) {
    size_t used = wcsnlen(dst, size);
    if (used == size) return size + wcslen(src);
    return used + wcslcpy(dst + used, src, size - used);
}

int __memcmpeq(const void *a, const void *b, size_t n) { return memcmp(a, b, n) != 0; }
