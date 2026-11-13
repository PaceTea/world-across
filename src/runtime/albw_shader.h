#ifndef ALBW_SHADER_H
#define ALBW_SHADER_H

#include <stdint.h>

// The vertex shader: its program, its uniforms, and the machine that runs it.
//
// The guest uploads the program through a handful of registers rather than putting it in memory: an
// index register and a data register, written in bursts. So the program has to be caught as it goes
// past, which is what this keeps. Nothing here reads guest memory.
//
// The hardware's limits, and the sizes of everything below: 512 instructions, 128 operand
// descriptors, 96 float uniform vectors, 4 integer ones and 16 booleans.
#define ALBW_VS_CODE_WORDS 512u
#define ALBW_VS_OPDESCS    128u
#define ALBW_VS_UNIFORMS   96u

// One word of the program, of the operand descriptors, or of a uniform, and the index registers that
// say where it lands. Called from the command list as the writes go by.
void albw_shader_code_index(uint32_t value);
void albw_shader_code_word(uint32_t value);
void albw_shader_opdesc_index(uint32_t value);
void albw_shader_opdesc_word(uint32_t value);
void albw_shader_uniform_index(uint32_t value);
void albw_shader_uniform_word(uint32_t value);
void albw_shader_bool_uniforms(uint32_t value);
void albw_shader_int_uniform(uint32_t which, uint32_t value);

// What was uploaded, as text: the program disassembled and the uniforms that are not zero. Off unless
// ALBW_DUMP_SHADER is set, and printed once.
void albw_shader_dump(uint32_t entry_point);

// Runs the program. `in` is the twelve input attributes as fetched; `out` receives the seven output
// registers. Returns 0 if the program did something this machine does not implement, having said what.
int albw_shader_run(uint32_t entry_point, const float in[][4], uint32_t attributes,
                    float out[7][4]);

// Decodes the program for albw_shader_run, if it changed since the last time. albw_shader_run does
// this itself, but shading is shared among threads and the decoding must not be: a draw calls this
// on its own thread first.
void albw_shader_prepare(void);

// The same for four vertices at once, `in[lane]` being one vertex's attributes: 1 when all four ran
// to the end, 0 when the program cannot run (as albw_shader_run would say for each), and -1 when
// the lanes took different branches, which the caller answers by running them one at a time. The
// results are the same bits albw_shader_run gives.
int albw_shader_run4(uint32_t entry_point, const float (*const in[4])[4], uint32_t attributes,
                     float out[4][7][4]);
// albw_shader_prepare and the uniforms laid out for the lanes, on the draw's own thread.
void albw_shader_prepare_lanes(void);

// The input register an attribute is routed to, from the permutation registers.
void albw_shader_permutation(uint32_t low, uint32_t high);

// How many times the machine met an instruction it does not implement, and the last such opcode.
unsigned long albw_shader_unsupported(uint32_t *which);

// How often a program ran past the end of the code memory, and how often it ran too long.
void albw_shader_failures(unsigned long *off_end, unsigned long *long_run);

#endif
