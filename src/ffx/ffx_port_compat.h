// Put in front of every source of AMD's FidelityFX SDK the port builds (-include): the few things
// it takes from Microsoft's C library, for GCC on Linux. The SDK itself is left as AMD ships it.
#pragma once

#ifdef __cplusplus
#include <cmath>
#include <codecvt>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <locale>
#include <string>
#include <math.h>

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

// The bounded copies, in the two forms the SDK calls: with the size, and with an array whose size
// the template works out. Both always terminate, as Microsoft's do.
static inline int wcscpy_s(wchar_t *dst, size_t size, const wchar_t *src) {
    if (!dst || !size) return 1;
    std::wcsncpy(dst, src ? src : L"", size);
    dst[size - 1] = L'\0';
    return 0;
}
template <size_t N> static inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t *src) {
    return wcscpy_s(dst, N, src);
}
static inline int strcpy_s(char *dst, size_t size, const char *src) {
    if (!dst || !size) return 1;
    std::strncpy(dst, src ? src : "", size);
    dst[size - 1] = '\0';
    return 0;
}
template <size_t N> static inline int strcpy_s(char (&dst)[N], const char *src) {
    return strcpy_s(dst, N, src);
}
#define sprintf_s snprintf
#define swprintf_s swprintf
#endif
