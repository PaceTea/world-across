#ifndef ALBW_LAUNCHER_H
#define ALBW_LAUNCHER_H

#include <SDL3/SDL.h>

// The launcher: a menu over the game's own title scene, which boots and runs behind it. Play hands
// the controls to the game; Settings changes how the window shows it, and is saved; Quit ends the
// run. In game, Escape brings the menu back and F1 the settings, with the game paused under them.
//
// Nothing in it is the game's: the background is whatever the game is drawing, and the text is set
// in a font the system has, through stb_truetype.

// What the window applies, and what is saved in $XDG_CONFIG_HOME/albw-port/settings. An environment
// variable of the old kind (ALBW_LAYOUT and the rest) still wins over the saved value.
enum { ALBW_LAYOUT_STACKED, ALBW_LAYOUT_SIDE, ALBW_LAYOUT_TOP };

// How the screens are scaled up to the window. Off draws each pixel as a block, bilinear blends
// them; FSR 1 is AMD's spatial upscaler on the GPU, and Software is the same filter on the
// processor, for a renderer that cannot run its shaders. FSR 3 and DLSS are temporal upscalers,
// which need the renderer's depth and motion as well as its picture.
enum { ALBW_UPSCALE_OFF, ALBW_UPSCALE_BILINEAR, ALBW_UPSCALE_FSR1, ALBW_UPSCALE_FSR3,
       ALBW_UPSCALE_DLSS, ALBW_UPSCALE_SOFTWARE, ALBW_UPSCALERS };

// How many frames a second the window shows. The game always runs at the console's 60; anything
// beyond needs frames in between.
enum { ALBW_RATE_CONSOLE, ALBW_RATE_DISPLAY, ALBW_RATE_UNCAPPED, ALBW_RATES };

// Who draws the picture: the graphics card, from the frames the game records, at a multiple of the
// console's resolution -- or the processor, at the console's own, which is what the game's frames
// are checked against.
enum { ALBW_RENDERER_GPU, ALBW_RENDERER_SOFTWARE, ALBW_RENDERERS };

// The GPU renderer's resolution: as the window needs it, or a fixed multiple of the console's.
#define ALBW_RESOLUTIONS 9                               // auto, then 1x to 8x

// Its anti-aliasing: multisampling, with this many samples a pixel.
enum { ALBW_AA_OFF, ALBW_AA_2X, ALBW_AA_4X, ALBW_AA_8X, ALBW_AAS };

// What touches the touch screen besides the mouse, which always does: a gamepad's touchpad, its
// gyroscope (aimed after pressing the right stick in), its right stick, or all of them.
enum { ALBW_TOUCH_MOUSE, ALBW_TOUCH_TOUCHPAD, ALBW_TOUCH_GYRO, ALBW_TOUCH_STICK, ALBW_TOUCH_ALL,
       ALBW_TOUCHES };

// The sharpening that follows an upscaler, in stops, as RCAS takes it: 0 is the most, each stop
// halves it.
#define ALBW_FSR_SHARPNESS_LEVELS 5
static const float albw_fsr_stops[ALBW_FSR_SHARPNESS_LEVELS] = { 0.0f, 0.2f, 0.5f, 1.0f, 2.0f };

// What this build, on this machine, can do: 1 if `choice` of `setting` is available; else 0, and
// `why` (if not NULL) says what it would take. The launcher offers only what is available.
enum { ALBW_SET_UPSCALE, ALBW_SET_FRAME_RATE, ALBW_SET_RENDERER,
       ALBW_SET_RESOLUTION, ALBW_SET_AA };
int albw_window_offers(int setting, int choice, const char **why);

struct albw_settings {
    int fullscreen;
    int layout;
    int renderer;             // ALBW_RENDERER_*
    int resolution;           // 0 as the window needs, else that multiple
    int antialias;            // ALBW_AA_*
    int upscale;              // ALBW_UPSCALE_*
    int sharpness;            // an index into albw_fsr_stops: 0 strongest
    int frame_rate;           // ALBW_RATE_*
    int integer_scale;
    int sound;
    int gamepads;
    int touch_controls;       // ALBW_TOUCH_*
    int language;             // index into the launcher's list; 0 follows the system
};

// Reads the saved settings (and the environment over them), and passes the language on to the
// game. Called before the window opens.
const struct albw_settings *albw_settings_load(void);
const struct albw_settings *albw_settings(void);

// Called once the window and renderer exist. Returns 0 when there is no font to set text in, and
// the game then starts without the launcher.
int albw_launcher_init(SDL_Window *window, SDL_Renderer *renderer);

// The card the game is running from, for the line that says it is ready: its product code, and
// where it is.
void albw_launcher_set_card(const char *product, const char *path);

// The launcher's build (world-across): `ready` says whether the game is there to play, `build`
// starts making it from the card, `progress` says how that goes. With them set, Play makes the game
// first when it is not there, showing how far it is, and plays it once it is.
struct albw_build_progress;
void albw_launcher_set_builder(int (*ready)(void), int (*build)(void),
                               void (*progress)(struct albw_build_progress *out));

// With no card known, Play asks for one in a file dialog and hands the path to `chooser`, which
// checks it, moves it into the card folder (albw_card.h), remembers it and gives the game its
// RomFS: it returns 1, the product code and where the card is now (`where`), or 0 and why not.
// It runs on a thread of its own, and sets *progress from 0 to 1 while a move copies. Set by main()
// before the window opens.
void albw_launcher_set_card_chooser(int (*chooser)(const char *path, char product[17], char *where,
                                                   size_t where_n, char *why, size_t n,
                                                   volatile float *progress));

enum albw_launcher_action {
    ALBW_LAUNCHER_NONE,       // nothing for the window to do
    ALBW_LAUNCHER_PLAY,       // close the menu, give the game its controls
    ALBW_LAUNCHER_QUIT,
    ALBW_LAUNCHER_APPLY,      // a setting changed: apply albw_settings()
};

// Called each frame while the launcher is up: ALBW_LAUNCHER_PLAY once a game that Play asked for
// has been made.
enum albw_launcher_action albw_launcher_poll(void);

// The software keyboard's field (albw_kernel_set_text_prompt): asked from the game's thread,
// answered from the window's, which shows it, hands it its events and draws it while it is up.
int albw_prompt_ask(const uint16_t *text, unsigned n, unsigned max_len);
int albw_prompt_answer(uint16_t *text, unsigned *n);
int albw_prompt_visible(void);
void albw_prompt_event(const SDL_Event *e);
void albw_prompt_draw(void);

// The top screen as the game was left, kept as the menu's backdrop for its next start.
void albw_launcher_keep_backdrop(SDL_Surface *shot);

int albw_launcher_visible(void);
void albw_launcher_show(int settings_page);
enum albw_launcher_action albw_launcher_event(const SDL_Event *e);

// Draws the launcher over the whole window, with the game's top screen behind it.
void albw_launcher_draw(SDL_Texture *top_screen);

#endif
