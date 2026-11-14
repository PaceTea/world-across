#ifndef ALBW_GAME_H
#define ALBW_GAME_H

// The translated game as a module of its own -- game.dll on Windows, libgame.so elsewhere -- built on
// the player's machine from their own cartridge (albw_builder.c) and loaded by the runtime. The
// program that is published carries nothing of the game: the launcher makes the game the first time
// it starts.
//
// The two sides meet in two tables. The host, the runtime, hands over the guest's memory and the
// few functions the translated code calls; the module hands back its dispatch table and which code
// it is a translation of. The translated code calls nothing else, not even the C library, so the
// module is built without one and does not care which the host was built with.
#include "albw_cpu.h"

#define ALBW_GAME_ABI 1

struct albw_game_host {
    uint32_t abi;
    uint8_t *mem;
    void (*svc)(albw_cpu *c, uint32_t imm);
    void (*dispatch)(albw_cpu *c, uint32_t target);
    void (*preempt)(albw_cpu *c);
    void (*missing_function)(albw_cpu *c, uint32_t pc, uint32_t target);
    void (*unimplemented)(albw_cpu *c, uint32_t pc, const char *what);
};

struct albw_game {
    uint32_t abi;
    uint32_t count;                     // functions in the dispatch table
    const uint32_t *addrs;              // their guest addresses, ascending
    const albw_fn *ptrs;
    const char *product;                // the release translated, e.g. CTR-P-BZLE
    uint64_t code_hash;                 // FNV-1a of the code.bin it was translated from
    uint32_t code_size;
};

// The one function a module exports.
typedef const struct albw_game *(*albw_game_connect_fn)(const struct albw_game_host *host);
#define ALBW_GAME_CONNECT "albw_game_connect"

#endif
