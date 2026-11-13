#ifndef ALBW_PICA_H
#define ALBW_PICA_H

#include <stdint.h>

// The GPU's register file, and the command lists that write it.
//
// This keeps state; it does not draw. Keeping the state is the part a rasteriser has to sit on, and
// it is worth having on its own because it identifies the registers by evidence: the addresses of
// the colour and depth buffers are already known from the fills and transfers, so whichever
// registers hold them are the render target registers, whatever any document calls them.
//
// There are 0x300 registers, each a word, written through a command list with a byte mask.
#define ALBW_PICA_REGS 0x300u

// Applies a command list. `surfaces` are addresses known from elsewhere -- the colour and depth
// buffers -- so that the register holding each can be pointed out the first time a list is applied.
void albw_pica_execute(uint32_t addr, uint32_t size);

// Notes an address the GPU is known to be drawing into, so the register dump can name it.
void albw_pica_note_surface(uint32_t addr, const char *what);

// Everything the register file says, once, after the first list that touches the geometry
// pipeline. Off unless ALBW_DUMP_REGS is set.
void albw_pica_dump_registers(void);

uint32_t albw_pica_reg(uint32_t index);

// One vertex as the shader sees it: up to twelve attributes of four floats. The hardware fills the
// components an attribute does not supply with 0, 0, 0, 1.
#define ALBW_PICA_MAX_ATTRS 12
struct albw_pica_vertex_in {
    float attr[ALBW_PICA_MAX_ATTRS][4];
    uint32_t count;
};

// Reads one vertex out of guest memory through the attribute loaders.
void albw_pica_fetch_vertex(uint32_t index, struct albw_pica_vertex_in *out);

// The index of the n-th vertex of the current draw, following the index array if there is one.
uint32_t albw_pica_vertex_index(uint32_t n, int indexed);

// A fixed attribute's component, for attributes the guest supplies by register rather than from
// memory.
float albw_pica_fixed_attr(uint32_t attr, int component);

// The hardware's 24-bit float, as an ordinary one.
float albw_pica_float24(uint32_t raw);

#endif
