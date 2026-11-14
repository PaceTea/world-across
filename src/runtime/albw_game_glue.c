// Built into the game module (albw_game.h), beside the translated code, on the player's machine.
//
// The translated code calls the runtime by name -- albw_svc, albw_dispatch and the rest -- and reads
// guest memory through albw_mem. In the module those names are these: forwards to the host's
// functions, and a copy of its memory pointer, which never changes once the host has made it. The
// module links no C library, so the little the compiler may still ask of one is here as well.
//
// Compiled with -fno-math-errno, so that sqrtf below is the processor's instruction and not a call
// to itself.
#include "albw_game.h"
#include "albw_generated.h"
#include "albw_fn_count.h"

#include <stddef.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

extern const char albw_build_product[];
extern const uint64_t albw_build_code_hash;
extern const uint32_t albw_build_code_size;

uint8_t *albw_mem;
static const struct albw_game_host *host;

void albw_svc(albw_cpu *c, uint32_t imm) { host->svc(c, imm); }
void albw_dispatch(albw_cpu *c, uint32_t target) { host->dispatch(c, target); }
void albw_preempt(albw_cpu *c) { host->preempt(c); }
void albw_missing_function(albw_cpu *c, uint32_t pc, uint32_t target) {
    host->missing_function(c, pc, target);
}
void albw_unimplemented(albw_cpu *c, uint32_t pc, const char *what) {
    host->unimplemented(c, pc, what);
}

float sqrtf(float x) { return __builtin_sqrtf(x); }

// What a compiler may emit for a large copy or clear. Plain loops, kept from being turned back into
// calls to themselves.
#define NO_PATTERNS __attribute__((optimize("no-tree-loop-distribute-patterns")))
NO_PATTERNS void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
NO_PATTERNS void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}
NO_PATTERNS void *memset(void *dst, int v, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)v;
    return dst;
}
NO_PATTERNS int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y) return *x < *y ? -1 : 1;
    }
    return 0;
}

EXPORT const struct albw_game *albw_game_connect(const struct albw_game_host *h) {
    static struct albw_game game;
    if (!h || h->abi != ALBW_GAME_ABI) return NULL;
    host = h;
    albw_mem = h->mem;
    game.abi = ALBW_GAME_ABI;
    game.count = ALBW_FN_COUNT;
    game.addrs = albw_fn_addrs;
    game.ptrs = albw_fn_ptrs;
    game.product = albw_build_product;
    game.code_hash = albw_build_code_hash;
    game.code_size = albw_build_code_size;
    return &game;
}

#ifdef _WIN32
// The module's entry point, with no C library to start: there is nothing to set up.
int __stdcall DllMainCRTStartup(void *module, unsigned long reason, void *reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return 1;
}
#endif
