#ifndef ALBW_INPUT_H
#define ALBW_INPUT_H

#include <stdint.h>

// What the player is doing with the console's controls, in the console's own terms. The window fills
// it in from a keyboard, a gamepad and a mouse; the kernel's HID service hands it to the game. Kept
// apart from both, because the kernel must not depend on anything that opens a window.

// The console's buttons, numbered as its HID pad state numbers them.
#define ALBW_PAD_A          (1u << 0)
#define ALBW_PAD_B          (1u << 1)
#define ALBW_PAD_SELECT     (1u << 2)
#define ALBW_PAD_START      (1u << 3)
#define ALBW_PAD_RIGHT      (1u << 4)
#define ALBW_PAD_LEFT       (1u << 5)
#define ALBW_PAD_UP         (1u << 6)
#define ALBW_PAD_DOWN       (1u << 7)
#define ALBW_PAD_R          (1u << 8)
#define ALBW_PAD_L          (1u << 9)
#define ALBW_PAD_X          (1u << 10)
#define ALBW_PAD_Y          (1u << 11)
// The circle pad, also reported as four buttons once it is pushed far enough.
#define ALBW_PAD_CIRCLE_RIGHT (1u << 28)
#define ALBW_PAD_CIRCLE_LEFT  (1u << 29)
#define ALBW_PAD_CIRCLE_UP    (1u << 30)
#define ALBW_PAD_CIRCLE_DOWN  (1u << 31)

// The circle pad's reach in either direction, in the units the hardware reports.
#define ALBW_CIRCLE_MAX 156

struct albw_input {
    uint32_t buttons;
    int16_t circle_x, circle_y;                          // right and up are positive
    int touching;
    uint16_t touch_x, touch_y;                           // on the bottom screen, 320 by 240
};

#endif
