#ifndef ALBW_WINDOW_H
#define ALBW_WINDOW_H

#include <stdint.h>

#include "albw_input.h"

// The window: both screens on the desktop, and the keyboard, a gamepad and the mouse as the console's
// buttons, circle pad and touch screen.
//
// It lives outside the kernel on purpose. The kernel is also built as a shared library for the
// reference run, which must not open windows or read a keyboard -- so the kernel calls a frame hook
// it is handed, and only the port itself hands it one.
//
// Headless when ALBW_HEADLESS is set, when the discovery loop is running, or when there is no display
// to open a window on; the run then behaves exactly as it did before there was a window.

// Opens the window if one is wanted and possible. Returns whether one is open.
int albw_window_open(void);

// The launcher, before the game starts. Returns 1 when the player chose Play (or there is no
// launcher), 0 when they quit.
int albw_window_launch(void);

// Runs the game -- `run`, the scheduler -- on a thread of its own, and the window on this one
// until the game has stopped; returns what `run` returned. The main thread is the window's because
// SDL wants its video and events there.
const char *albw_window_play(const char *(*run)(void));

// Called once per guest frame by the kernel's frame hook: shows the screens, reads the input, keeps
// the pace. Returns 0 when the user has closed the window.
int albw_window_frame(uint32_t shared_addr, uint32_t thread_index, unsigned long frame);

// One frame of the DSP's output, interleaved stereo at ALBW_DSP_SAMPLE_RATE, for the speakers.
void albw_window_audio(const int16_t *stereo, unsigned frames);

// The input as of the last frame.
const struct albw_input *albw_window_input(void);

// A message the player has to read before the window goes: why the run is ending, say.
// The same, before or without a window: for what stops the port before it opens one.
void albw_window_alert(const char *title, const char *text);
void albw_window_message(const char *title, const char *text);

void albw_window_close(void);

#endif
