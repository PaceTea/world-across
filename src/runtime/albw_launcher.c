#include "albw_launcher.h"
#include "albw_builder.h"
#include "albw_platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if __has_include(<stb/stb_truetype.h>)
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb/stb_truetype.h>
#define HAVE_STB 1
#if __has_include(<stb/stb_image.h>)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <stb/stb_image.h>
#pragma GCC diagnostic pop
#define HAVE_STB_IMAGE 1
#endif
#else
#define HAVE_STB 0                                       // no text: the game starts without the menu
#endif

// ---------------------------------------------------------------------------------------------
// Settings.

static const struct { const char *code, *name; } languages[] = {
    { "", "System" }, { "en", "English" }, { "fr", "Fran\xc3\xa7" "ais" }, { "de", "Deutsch" },
    { "es", "Espa\xc3\xb1ol" }, { "it", "Italiano" }, { "nl", "Nederlands" },
    { "pt", "Portugu\xc3\xaas" }, { "ja", "Japanese" }, { "ru", "Russian" },
};
#define LANGUAGES ((int)(sizeof languages / sizeof languages[0]))

static struct albw_settings settings = {
    .layout = ALBW_LAYOUT_STACKED, .antialias = ALBW_AA_4X, .upscale = ALBW_UPSCALE_OFF,
    .sharpness = 1,
    .frame_rate = ALBW_RATE_CONSOLE, .frame_gen = ALBW_FRAMEGEN_OFF, .sound = 1, .gamepads = 1,
};

// The words the settings file uses for each choice, in the order of the enums.
static const char *const layout_keys[] = { "stacked", "side", "top" };
static const char *const upscale_keys[ALBW_UPSCALERS] = {
    "off", "bilinear", "fsr1", "fsr3", "dlss", "software",
};
static const char *const rate_keys[ALBW_RATES] = { "console", "display", "uncapped" };
static const char *const framegen_keys[ALBW_FRAMEGENS] = { "off", "fsr3", "dlss" };
static const char *const renderer_keys[ALBW_RENDERERS] = { "gpu", "software" };
static const char *const resolution_keys[ALBW_RESOLUTIONS] = {
    "auto", "1", "2", "3", "4", "5", "6", "7", "8",
};
static const char *const aa_keys[ALBW_AAS] = { "off", "2", "4", "8" };

static int key_index(const char *const *keys, int count, const char *v, int otherwise) {
    for (int i = 0; i < count; i++) {
        if (!strcmp(keys[i], v)) return i;
    }
    return otherwise;
}

static void settings_path(char *out, size_t n) {
    albw_config_path(out, n, "settings");
}

static void settings_save(void) {
    char path[600], dir[600];
    settings_path(path, sizeof path);
    // The directory, and the one it is in (~/.config, or %APPDATA%), may not be there yet.
    albw_config_path(dir, sizeof dir, "");
    size_t len = strlen(dir);
    if (len && (dir[len - 1] == '/' || dir[len - 1] == '\\')) dir[--len] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        albw_mkdir(dir);
        *slash = '/';
    }
    albw_mkdir(dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "fullscreen=%d\nlayout=%s\nrenderer=%s\nresolution=%s\nantialias=%s\nupscale=%s\n"
               "sharpness=%d\nframerate=%s\nframegen=%s\ninteger=%d\nsound=%d\ngamepads=%d\n"
               "language=%s\n",
            settings.fullscreen, layout_keys[settings.layout], renderer_keys[settings.renderer],
            resolution_keys[settings.resolution], aa_keys[settings.antialias],
            upscale_keys[settings.upscale],
            settings.sharpness, rate_keys[settings.frame_rate], framegen_keys[settings.frame_gen],
            settings.integer_scale, settings.sound, settings.gamepads,
            languages[settings.language].code);
    fclose(f);
}

static int env_flag(const char *name, int *out) {
    const char *s = getenv(name);
    if (!s || !*s) return 0;
    *out = strcmp(s, "0") != 0;
    return 1;
}

const struct albw_settings *albw_settings(void) { return &settings; }

const struct albw_settings *albw_settings_load(void) {
    char path[600], line[200];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *v = eq + 1;
        v[strcspn(v, "\r\n")] = '\0';
        if (!strcmp(line, "fullscreen")) settings.fullscreen = atoi(v) != 0;
        else if (!strcmp(line, "layout")) settings.layout = key_index(layout_keys, 3, v, ALBW_LAYOUT_STACKED);
        else if (!strcmp(line, "smooth"))                // the key before FSR was a choice
            settings.upscale = atoi(v) ? ALBW_UPSCALE_BILINEAR : ALBW_UPSCALE_OFF;
        else if (!strcmp(line, "filter"))                // the key before there were upscalers
            settings.upscale = !strcmp(v, "fsr") ? ALBW_UPSCALE_FSR1
                             : !strcmp(v, "smooth") ? ALBW_UPSCALE_BILINEAR : ALBW_UPSCALE_OFF;
        else if (!strcmp(line, "upscale"))
            settings.upscale = key_index(upscale_keys, ALBW_UPSCALERS, v, ALBW_UPSCALE_OFF);
        else if (!strcmp(line, "renderer"))
            settings.renderer = key_index(renderer_keys, ALBW_RENDERERS, v, ALBW_RENDERER_GPU);
        else if (!strcmp(line, "resolution"))
            settings.resolution = key_index(resolution_keys, ALBW_RESOLUTIONS, v, 0);
        else if (!strcmp(line, "antialias"))
            settings.antialias = key_index(aa_keys, ALBW_AAS, v, ALBW_AA_4X);
        else if (!strcmp(line, "sharpness")) {
            int n = atoi(v);
            if (n >= 0 && n < ALBW_FSR_SHARPNESS_LEVELS) settings.sharpness = n;
        }
        else if (!strcmp(line, "framerate"))
            settings.frame_rate = key_index(rate_keys, ALBW_RATES, v, ALBW_RATE_CONSOLE);
        else if (!strcmp(line, "framegen"))
            settings.frame_gen = key_index(framegen_keys, ALBW_FRAMEGENS, v, ALBW_FRAMEGEN_OFF);
        else if (!strcmp(line, "integer")) settings.integer_scale = atoi(v) != 0;
        else if (!strcmp(line, "sound")) settings.sound = atoi(v) != 0;
        else if (!strcmp(line, "gamepads")) settings.gamepads = atoi(v) != 0;
        else if (!strcmp(line, "language")) {
            for (int i = 0; i < LANGUAGES; i++) {
                if (!strcmp(languages[i].code, v)) settings.language = i;
            }
        }
    }
    if (f) fclose(f);

    // The variables of before the launcher still win, so that scripts and tools keep working.
    const char *l = getenv("ALBW_LAYOUT");
    if (l && *l) settings.layout = key_index(layout_keys, 3, l, ALBW_LAYOUT_STACKED);
    const char *fl = getenv("ALBW_FILTER");
    if (fl && *fl) settings.upscale = !strcmp(fl, "fsr") ? ALBW_UPSCALE_FSR1
                                    : !strcmp(fl, "linear") ? ALBW_UPSCALE_BILINEAR : ALBW_UPSCALE_OFF;
    const char *up = getenv("ALBW_UPSCALE");
    if (up && *up) settings.upscale = key_index(upscale_keys, ALBW_UPSCALERS, up, settings.upscale);
    const char *rend = getenv("ALBW_RENDERER");
    if (rend && *rend) settings.renderer = key_index(renderer_keys, ALBW_RENDERERS, rend, settings.renderer);
    const char *res = getenv("ALBW_RESOLUTION");
    if (res && *res) settings.resolution = key_index(resolution_keys, ALBW_RESOLUTIONS, res, settings.resolution);
    const char *aa = getenv("ALBW_ANTIALIAS");
    if (aa && *aa) settings.antialias = key_index(aa_keys, ALBW_AAS, aa, settings.antialias);
    const char *rate = getenv("ALBW_FRAME_RATE");
    if (rate && *rate) settings.frame_rate = key_index(rate_keys, ALBW_RATES, rate, settings.frame_rate);
    const char *gen = getenv("ALBW_FRAMEGEN");
    if (gen && *gen) settings.frame_gen = key_index(framegen_keys, ALBW_FRAMEGENS, gen, settings.frame_gen);
    env_flag("ALBW_INTEGER_SCALE", &settings.integer_scale);
    int mute;
    if (env_flag("ALBW_MUTE", &mute)) settings.sound = !mute;
    env_flag("ALBW_GAMEPADS", &settings.gamepads);
    // The language goes to the game the way it always has, as ALBW_LANGUAGE, read when it first
    // asks the console which one it is set to.
    if (settings.language && !getenv("ALBW_LANGUAGE")) {
        albw_setenv("ALBW_LANGUAGE", languages[settings.language].code);
    }
    return &settings;
}

// ---------------------------------------------------------------------------------------------
// Text, in a font the system has.

#define FIRST_CHAR 32
#define CHARS 224                                        // to 255: Latin-1 covers the menu's words

#if !HAVE_STB
typedef struct { unsigned short x0, y0, x1, y1; float xoff, yoff, xadvance; } stbtt_bakedchar;
#endif

struct font {
    float size;
    int w, h;
    stbtt_bakedchar cd[CHARS];
    SDL_Texture *tex;
};

static SDL_Window *window;
static SDL_Renderer *renderer;
static unsigned char *serif_data, *sans_data, *small_data;
static SDL_Texture *wallpaper;
static struct font f_title, f_kicker, f_item, f_small;
static int baked_for_h;

static unsigned char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = n > 0 ? malloc((size_t)n) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

// fontconfig's choice for a pattern, else the first of some usual places that exists. With a
// family named, fontconfig's answer counts only if it is that family and not its nearest stand-in.
static unsigned char *find_font_of(const char *pattern, const char *family,
                                   const char *const *fallbacks) {
    char path[600] = "";
#ifdef _WIN32
    // No fontconfig: the system's own fonts, by file name.
    (void)pattern;
    const char *windir = getenv("WINDIR");
    for (int i = 0; fallbacks[i]; i++) {
        if (strncmp(fallbacks[i], "%WINDIR%", 8)) continue;
        snprintf(path, sizeof path, "%s%s", windir ? windir : "C:\\Windows", fallbacks[i] + 8);
        unsigned char *d = read_file(path);
        if (d) return d;
    }
    path[0] = '\0';
#else
    char cmd[200];
    snprintf(cmd, sizeof cmd, "fc-match -f '%%{family}|%%{file}' '%s' 2>/dev/null", pattern);
    FILE *p = popen(cmd, "r");
    if (p) {
        if (!fgets(path, sizeof path, p)) path[0] = '\0';
        pclose(p);
    }
#endif
    char *bar = strchr(path, '|');
    if (bar) {
        *bar = '\0';
        if (family && !strstr(path, family)) path[0] = '\0';
        else memmove(path, bar + 1, strlen(bar + 1) + 1);
    }
    const char *ext = strrchr(path, '.');
    if (path[0] && ext && (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf"))) {
        unsigned char *d = read_file(path);
        if (d) return d;
    }
    for (int i = 0; fallbacks[i]; i++) {
        unsigned char *d = read_file(fallbacks[i]);
        if (d) return d;
    }
    return NULL;
}

static unsigned char *find_font(const char *pattern, const char *const *fallbacks) {
    return find_font_of(pattern, NULL, fallbacks);
}

static void bake(struct font *f, const unsigned char *data, float size) {
#if !HAVE_STB
    (void)f; (void)data; (void)size;
    return;
#endif
    if (f->tex) SDL_DestroyTexture(f->tex);
    f->tex = NULL;
    f->size = size;
#if HAVE_STB
    for (int h = 512; h <= 4096; h *= 2) {
        int w = 2048;
        unsigned char *alpha = calloc((size_t)w * (size_t)h, 1);
        if (!alpha) return;
        int r = stbtt_BakeFontBitmap(data, stbtt_GetFontOffsetForIndex(data, 0), size, alpha, w, h,
                                     FIRST_CHAR, CHARS, f->cd);
        if (r <= 0 && h < 4096) {                        // did not fit: a taller sheet
            free(alpha);
            continue;
        }
        uint32_t *rgba = malloc((size_t)w * (size_t)h * 4u);
        if (rgba) {
            for (int i = 0; i < w * h; i++) rgba[i] = 0x00FFFFFFu | ((uint32_t)alpha[i] << 24);
            f->tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                       w, h);
            if (f->tex) {
                SDL_UpdateTexture(f->tex, NULL, rgba, w * 4);
                SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(f->tex, SDL_SCALEMODE_LINEAR);
            }
            free(rgba);
        }
        free(alpha);
        f->w = w;
        f->h = h;
        return;
    }
#endif
}

// The next character of a UTF-8 string, as a code point.
static unsigned next_cp(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p++;
    if (c >= 0xC0 && c < 0xE0 && (*p & 0xC0) == 0x80) c = ((c & 0x1F) << 6) | (*p++ & 0x3F);
    else if (c >= 0xE0) {
        while ((*p & 0xC0) == 0x80) p++;
        c = '?';
    }
    *s = (const char *)p;
    return c;
}

static float text_width(const struct font *f, const char *s) {
    float w = 0;
    while (*s) {
        unsigned c = next_cp(&s);
        if (c >= FIRST_CHAR && c < FIRST_CHAR + CHARS) w += f->cd[c - FIRST_CHAR].xadvance;
    }
    return w;
}

// Text with its baseline at y. Returns where it ends.
static float text(const struct font *f, float x, float y, const char *s, SDL_Color c, float alpha) {
    if (!f->tex) return x;
#if HAVE_STB
    SDL_SetTextureColorMod(f->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaModFloat(f->tex, alpha < 0 ? 0 : alpha > 1 ? 1 : alpha);
    while (*s) {
        unsigned cp = next_cp(&s);
        if (cp < FIRST_CHAR || cp >= FIRST_CHAR + CHARS) continue;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(f->cd, f->w, f->h, (int)(cp - FIRST_CHAR), &x, &y, &q, 1);
        SDL_FRect src = { q.s0 * f->w, q.t0 * f->h, (q.s1 - q.s0) * f->w, (q.t1 - q.t0) * f->h };
        SDL_FRect dst = { q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0 };
        SDL_RenderTexture(renderer, f->tex, &src, &dst);
    }
#else
    (void)y; (void)s; (void)c; (void)alpha;
#endif
    return x;
}

// ---------------------------------------------------------------------------------------------
// The menu.

enum { PAGE_MAIN, PAGE_SETTINGS };
enum { ITEM_PLAY, ITEM_SETTINGS, ITEM_MODS, ITEM_QUIT, MAIN_ITEMS };
enum { ROW_FULLSCREEN, ROW_LAYOUT, ROW_RENDERER, ROW_RESOLUTION, ROW_AA, ROW_UPSCALE,
       ROW_SHARPNESS, ROW_FRAME_RATE, ROW_FRAME_GEN, ROW_INTEGER, ROW_SOUND, ROW_GAMEPADS,
       ROW_LANGUAGE, ROW_BACK, SETTINGS_ROWS };

// The settings page, row by row. A row with few choices shows them all side by side, the chosen
// one lit and the ones this build or machine cannot do dimmed; a row with many shows one, between
// arrows. Left and right step through what is available; a click picks a choice outright.
static const char *const on_off[] = { "Off", "On" };
static const char *const layout_names[] = { "Stacked", "Side by side", "Top only" };
static const char *const upscale_names[ALBW_UPSCALERS] = {
    "Off", "Bilinear", "FSR 1", "FSR 3", "DLSS", "Software",
};
static const char *const sharp_names[ALBW_FSR_SHARPNESS_LEVELS] = {
    "Strongest", "Strong", "Medium", "Light", "Lightest",
};
static const char *const rate_names[ALBW_RATES] = { "60 (console)", "Display", "Uncapped" };
static const char *const framegen_names[ALBW_FRAMEGENS] = { "Off", "FSR 3", "DLSS" };
static const char *const renderer_names[ALBW_RENDERERS] = { "GPU", "Software" };
static const char *const aa_names[ALBW_AAS] = { "Off", "2x MSAA", "4x MSAA", "8x MSAA" };
// Each multiple with the height it gives the top screen.
static const char *const resolution_names[ALBW_RESOLUTIONS] = {
    "Auto", "1x (240p)", "2x (480p)", "3x (720p)", "4x (960p)", "5x (1200p)", "6x (1440p)",
    "7x (1680p)", "8x (1920p)",
};
static const char *language_names[LANGUAGES];

static const struct row_def {
    const char *name;
    const char *const *choices;
    int count;
    int side_by_side;
    int offers;                                          // the setting albw_window_offers knows, or -1
} rows[SETTINGS_ROWS] = {
    [ROW_FULLSCREEN] = { "Fullscreen", on_off, 2, 1, -1 },
    [ROW_LAYOUT] = { "Screens", layout_names, 3, 1, -1 },
    [ROW_RENDERER] = { "Renderer", renderer_names, ALBW_RENDERERS, 1, ALBW_SET_RENDERER },
    [ROW_RESOLUTION] = { "Resolution", resolution_names, ALBW_RESOLUTIONS, 0, ALBW_SET_RESOLUTION },
    [ROW_AA] = { "Anti-aliasing", aa_names, ALBW_AAS, 1, ALBW_SET_AA },
    [ROW_UPSCALE] = { "Upscaling", upscale_names, ALBW_UPSCALERS, 1, ALBW_SET_UPSCALE },
    [ROW_SHARPNESS] = { "Sharpening", sharp_names, ALBW_FSR_SHARPNESS_LEVELS, 0, -1 },
    [ROW_FRAME_RATE] = { "Frame rate", rate_names, ALBW_RATES, 1, ALBW_SET_FRAME_RATE },
    [ROW_FRAME_GEN] = { "Frame generation", framegen_names, ALBW_FRAMEGENS, 1, ALBW_SET_FRAME_GEN },
    [ROW_INTEGER] = { "Whole-pixel scaling", on_off, 2, 1, -1 },
    [ROW_SOUND] = { "Sound", on_off, 2, 1, -1 },
    [ROW_GAMEPADS] = { "Gamepads", on_off, 2, 1, -1 },
    [ROW_LANGUAGE] = { "Language", language_names, LANGUAGES, 0, -1 },
    [ROW_BACK] = { "Back", NULL, 0, 0, -1 },
};

static int *row_value(int row) {
    switch (row) {
        case ROW_FULLSCREEN: return &settings.fullscreen;
        case ROW_LAYOUT: return &settings.layout;
        case ROW_RENDERER: return &settings.renderer;
        case ROW_RESOLUTION: return &settings.resolution;
        case ROW_AA: return &settings.antialias;
        case ROW_UPSCALE: return &settings.upscale;
        case ROW_SHARPNESS: return &settings.sharpness;
        case ROW_FRAME_RATE: return &settings.frame_rate;
        case ROW_FRAME_GEN: return &settings.frame_gen;
        case ROW_INTEGER: return &settings.integer_scale;
        case ROW_SOUND: return &settings.sound;
        case ROW_GAMEPADS: return &settings.gamepads;
        case ROW_LANGUAGE: return &settings.language;
        default: return NULL;
    }
}

static int offered(int row, int choice, const char **why) {
    if (why) *why = NULL;
    if (rows[row].offers < 0) return 1;
    return albw_window_offers(rows[row].offers, choice, why);
}

// Whether the sharpening row means anything with the upscaler as it is.
static int sharpening_applies(void) {
    return (settings.upscale == ALBW_UPSCALE_FSR1 || settings.upscale == ALBW_UPSCALE_SOFTWARE)
        && offered(ROW_UPSCALE, settings.upscale, NULL);
}

// A line on what a choice does, or on what it would need.
static const char *describe(int row, int choice) {
    const char *why;
    if (!offered(row, choice, &why) && why) return why;
    switch (row) {
        case ROW_UPSCALE: {
            static const char *const text[ALBW_UPSCALERS] = {
                "The console's pixels, each drawn as a square block.",
                "Neighbouring pixels blended: soft, with no steps.",
                "AMD FidelityFX Super Resolution 1: edge-aware scaling and sharpening, on any GPU.",
                "AMD FSR 3: temporal upscaling from depth and motion.",
                "NVIDIA DLSS: temporal upscaling by a trained network, on RTX cards.",
                "FSR 1's filter on the processor, for a GPU that cannot run it. Costs CPU time.",
            };
            return text[choice];
        }
        case ROW_SHARPNESS:
            return sharpening_applies() ? "How much the upscaler sharpens what it scaled."
                                        : "Applies to FSR 1 and Software.";
        case ROW_FRAME_RATE: {
            static const char *const text[ALBW_RATES] = {
                "Every frame the game draws, 60 a second, as the console shows them.",
                "Up to the display's rate: frames between the game's own, from the real geometry.",
                "As many frames between the game's own as the processor has room for.",
            };
            return text[choice];
        }
        case ROW_FRAME_GEN: {
            static const char *const text[ALBW_FRAMEGENS] = {
                "Every frame shown is one the renderer drew.",
                "AMD FSR 3 frame generation: frames made from the ones around them, on the GPU.",
                "NVIDIA DLSS frame generation.",
            };
            return text[choice];
        }
        case ROW_RENDERER: {
            static const char *const text[ALBW_RENDERERS] = {
                "The graphics card draws the game, at the resolution below. Smooth and sharp.",
                "The processor draws the game at the console's resolution, as checked against it.",
            };
            return text[choice];
        }
        case ROW_RESOLUTION:
            return choice == 0 ? "As many pixels as the screens take up in the window."
                               : "Each of the console's pixels becomes this many across and down.";
        case ROW_AA:
            return choice == 0 ? "Edges as they fall on the pixels."
                               : "Smooth edges: each pixel sampled this many times along them.";
        case ROW_FULLSCREEN: return "The game fills the screen. F11 switches at any time.";
        case ROW_LAYOUT: {
            static const char *const text[3] = {
                "The two screens one above the other, as the console has them.",
                "The two screens next to each other, for a wide display.",
                "The top screen alone, as large as it goes. The touch screen is not shown.",
            };
            return text[choice];
        }
        case ROW_INTEGER: return "Scales by whole numbers only, so every pixel is the same size.";
        case ROW_SOUND: return "The game's music and sound.";
        case ROW_GAMEPADS: return "Every connected gamepad plays, as well as the keyboard.";
        case ROW_LANGUAGE: return "The game's language, from the next start.";
        default: return NULL;
    }
}

static int want_visible = 1, page = PAGE_MAIN, sel_main, sel_settings, started;
static float fade = 0.0f, glow[SETTINGS_ROWS + MAIN_ITEMS], title_at = -1.0f, dim_at;
static uint64_t last_ns;
static char card_product[17];
static SDL_FRect hit[SETTINGS_ROWS];                    // where each row was drawn, for the mouse
static int hits;
// Where each choice of a side-by-side row was drawn, to be clicked.
static struct { SDL_FRect r; int row, choice; } segs[64];
static int seg_count, hover_row = -1, hover_choice = -1;
static int stick_dir;

void albw_launcher_set_card(const char *product) {
    snprintf(card_product, sizeof card_product, "%s", product ? product : "");
}

// The card, when none is known yet -- started from a desktop's menu, the first time: Play opens
// SDL's file dialog (which goes through the desktop's own), and what is picked is checked and
// remembered by the chooser main() hands over. The dialog answers on a thread of its own, so its
// answer waits here until the next frame takes it.
static int (*card_chooser)(const char *path, char product[17], char *why, size_t n);
static SDL_Mutex *card_mutex;
static char card_picked_path[1024], card_why[240];
static int card_dialog;                                  // 0 closed, 1 open, 2 a path waiting

void albw_launcher_set_card_chooser(int (*chooser)(const char *path, char product[17], char *why,
                                                   size_t n)) {
    card_chooser = chooser;
}

static void SDLCALL card_picked(void *userdata, const char *const *files, int filter) {
    (void)userdata;
    (void)filter;
    SDL_LockMutex(card_mutex);
    card_dialog = 0;
    if (files && files[0]) {
        snprintf(card_picked_path, sizeof card_picked_path, "%s", files[0]);
        card_dialog = 2;
    } else if (!files) {
        snprintf(card_why, sizeof card_why, "The file dialog did not open (%s). Start the port once "
                 "with the card's path instead.", SDL_GetError());
    }
    SDL_UnlockMutex(card_mutex);
}

static void card_ask(void) {
    if (!card_mutex) card_mutex = SDL_CreateMutex();
    if (!card_mutex) return;
    SDL_LockMutex(card_mutex);
    int open = card_dialog != 0;
    if (!open) card_dialog = 1;
    SDL_UnlockMutex(card_mutex);
    if (open) return;
    static const SDL_DialogFileFilter filters[] = {
        { "3DS card images", "3ds;cci" }, { "All files", "*" },
    };
    SDL_ShowOpenFileDialog(card_picked, NULL, window, filters, 2, NULL, false);
}

// The game, where the launcher builds it (world-across): whether Play plays, how to build it, and
// how that is going. Unset in a build with the translation in it.
static int (*game_ready)(void);
static int (*game_build)(void);
static void (*game_progress)(struct albw_build_progress *out);
static int play_when_ready;                              // Play was asked for while it was built

void albw_launcher_set_builder(int (*ready)(void), int (*build)(void),
                               void (*progress)(struct albw_build_progress *out)) {
    game_ready = ready;
    game_build = build;
    game_progress = progress;
}

static int game_building(struct albw_build_progress *p) {
    if (!game_progress) return 0;
    game_progress(p);
    return p->state == ALBW_BUILD_RUNNING;
}

// Play, with the game not built yet: building it, and playing once it is.
static void game_start_build(void) {
    struct albw_build_progress p;
    if (!game_building(&p) && game_build) game_build();
    play_when_ready = 1;
}

enum albw_launcher_action albw_launcher_poll(void) {
    if (play_when_ready && game_ready && game_ready()) {
        play_when_ready = 0;
        want_visible = 0;
        started = 1;
        return ALBW_LAUNCHER_PLAY;
    }
    return ALBW_LAUNCHER_NONE;
}

// A path the dialog picked, checked; called each frame the launcher is drawn.
static void card_take(void) {
    if (!card_mutex) return;
    char path[1024];
    SDL_LockMutex(card_mutex);
    int waiting = card_dialog == 2;
    if (waiting) {
        snprintf(path, sizeof path, "%s", card_picked_path);
        card_dialog = 0;
    }
    SDL_UnlockMutex(card_mutex);
    if (!waiting || !card_chooser) return;
    char product[17], why[240];
    if (card_chooser(path, product, why, sizeof why)) {
        snprintf(card_product, sizeof card_product, "%s", product);
        card_why[0] = '\0';
        // The launcher's build: a card chosen is a game to make, and then to play.
        if (game_ready && !game_ready()) game_start_build();
    } else {
        snprintf(card_why, sizeof card_why, "%s", why);
    }
}

int albw_launcher_init(SDL_Window *w, SDL_Renderer *r) {
    window = w;
    renderer = r;
    for (int i = 0; i < LANGUAGES; i++) language_names[i] = languages[i].name;
    // A condensed sans for the menu, as launchers of this kind have it: Fira Sans Condensed where it
    // is installed, else whatever sans the system has. The title takes Cinzel if the user has it.
    // On Windows the system's own: Segoe UI, which every release since Vista has.
    static const char *const title[] = {
        "%WINDIR%\\Fonts\\seguisb.ttf", "%WINDIR%\\Fonts\\segoeuib.ttf", "%WINDIR%\\Fonts\\arialbd.ttf",
        "%WINDIR%\\Fonts\\tahomabd.ttf", "%WINDIR%\\Fonts\\tahoma.ttf",
        "/usr/share/fonts/TTF/FiraSansCondensed-SemiBold.ttf",
        "/usr/share/fonts/noto/NotoSans-SemiCondensedSemiBold.ttf",
        "/usr/share/fonts/TTF/DejaVuSansCondensed-Bold.ttf", NULL,
    };
    static const char *const item[] = {
        "%WINDIR%\\Fonts\\segoeui.ttf", "%WINDIR%\\Fonts\\arial.ttf", "%WINDIR%\\Fonts\\tahoma.ttf",
        "/usr/share/fonts/TTF/FiraSansCondensed-Medium.ttf",
        "/usr/share/fonts/TTF/DejaVuSansCondensed.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf", NULL,
    };
    static const char *const small[] = {
        "%WINDIR%\\Fonts\\segoeui.ttf", "%WINDIR%\\Fonts\\arial.ttf", "%WINDIR%\\Fonts\\tahoma.ttf",
        "/usr/share/fonts/TTF/FiraSansCondensed-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL,
    };
#if HAVE_STB
    serif_data = find_font_of("Cinzel:bold", "Cinzel", title);
    sans_data = find_font_of("Fira Sans Condensed:medium", "Fira Sans Condensed", item);
    small_data = find_font_of("Fira Sans Condensed", "Fira Sans Condensed", small);
    if (!sans_data) sans_data = find_font("sans", item);
#endif
    if (!sans_data) {
        printf("[launch] no font found; starting without the launcher\n");
        want_visible = 0;
        return 0;
    }
    if (!serif_data) serif_data = sans_data;
    if (!small_data) small_data = sans_data;

    // A picture of the player's own for behind the menu: background.png or .jpg beside the
    // settings. Nothing of the kind comes with the port.
#if HAVE_STB_IMAGE
    static const char *const names[] = { "background.png", "background.jpg", "background.jpeg" };
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !wallpaper; i++) {
        char path[600];
        albw_config_path(path, sizeof path, names[i]);
        int iw, ih, n;
        unsigned char *px = stbi_load(path, &iw, &ih, &n, 4);
        if (!px) continue;
        wallpaper = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, iw, ih);
        if (wallpaper) {
            SDL_UpdateTexture(wallpaper, NULL, px, iw * 4);
            SDL_SetTextureScaleMode(wallpaper, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(wallpaper, SDL_BLENDMODE_BLEND);
            printf("[launch] background %s, %dx%d\n", path, iw, ih);
        }
        stbi_image_free(px);
    }
#endif
    const char *e = getenv("ALBW_LAUNCHER");
    if (e && !strcmp(e, "0")) want_visible = 0;
    if (e && !strcmp(e, "settings")) page = PAGE_SETTINGS;   // for screenshots of that page
    last_ns = SDL_GetTicksNS();
    return 1;
}

int albw_launcher_visible(void) { return want_visible || fade > 0.0f; }

void albw_launcher_show(int settings_page) {
    if (!sans_data) return;
    want_visible = 1;
    page = settings_page ? PAGE_SETTINGS : PAGE_MAIN;
}

// Steps a row's value: to the next available choice that way, round the end.
static void step(int row, int dir) {
    int *v = row_value(row);
    if (!v) return;
    int n = rows[row].count;
    // Sharpening reads strongest first; right makes it lighter, as a slider would.
    for (int i = 1; i <= n; i++) {
        int c = ((*v + dir * i) % n + n) % n;
        if (offered(row, c, NULL)) {
            *v = c;
            break;
        }
    }
    settings_save();
}

static enum albw_launcher_action pick(int row, int choice) {
    int *v = row_value(row);
    if (!v || !offered(row, choice, NULL)) return ALBW_LAUNCHER_NONE;
    *v = choice;
    settings_save();
    return ALBW_LAUNCHER_APPLY;
}

static enum albw_launcher_action confirm(void) {
    if (page == PAGE_SETTINGS) {
        if (sel_settings == ROW_BACK) {
            page = PAGE_MAIN;
            return ALBW_LAUNCHER_NONE;
        }
        step(sel_settings, 1);
        return ALBW_LAUNCHER_APPLY;
    }
    switch (sel_main) {
        case ITEM_PLAY:
            // With no card yet, Play asks for one first.
            if (!card_product[0] && card_chooser && !started) {
                card_ask();
                return ALBW_LAUNCHER_NONE;
            }
            // With the game not made yet, Play makes it, and plays when it is there.
            if (game_ready && !game_ready() && !started) {
                game_start_build();
                return ALBW_LAUNCHER_NONE;
            }
            want_visible = 0;
            started = 1;
            return ALBW_LAUNCHER_PLAY;
        case ITEM_SETTINGS:
            page = PAGE_SETTINGS;
            sel_settings = 0;
            return ALBW_LAUNCHER_NONE;
        case ITEM_QUIT:
            return ALBW_LAUNCHER_QUIT;
        default:
            return ALBW_LAUNCHER_NONE;                   // Mods: not yet
    }
}

static enum albw_launcher_action back(void) {
    if (page == PAGE_SETTINGS) {
        page = PAGE_MAIN;
        return ALBW_LAUNCHER_NONE;
    }
    if (started) {                                       // in game, back is back to it
        want_visible = 0;
        return ALBW_LAUNCHER_PLAY;
    }
    return ALBW_LAUNCHER_NONE;
}

static void move(int dir) {
    if (page == PAGE_SETTINGS) sel_settings = (sel_settings + SETTINGS_ROWS + dir) % SETTINGS_ROWS;
    else sel_main = (sel_main + MAIN_ITEMS + dir) % MAIN_ITEMS;
    hover_row = hover_choice = -1;
}

static enum albw_launcher_action sideways(int dir) {
    if (page != PAGE_SETTINGS || sel_settings == ROW_BACK) return ALBW_LAUNCHER_NONE;
    step(sel_settings, dir);
    hover_row = hover_choice = -1;
    return ALBW_LAUNCHER_APPLY;
}

enum albw_launcher_action albw_launcher_event(const SDL_Event *e) {
    if (!want_visible) return ALBW_LAUNCHER_NONE;
    switch (e->type) {
        case SDL_EVENT_KEY_DOWN:
            switch (e->key.scancode) {
                case SDL_SCANCODE_UP: case SDL_SCANCODE_W: move(-1); break;
                case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: move(1); break;
                case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: return sideways(-1);
                case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: return sideways(1);
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
                case SDL_SCANCODE_K:
                    if (!e->key.repeat) return confirm();
                    break;
                case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_J:
                    if (!e->key.repeat) return back();
                    break;
                default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (e->gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP: move(-1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: move(1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return sideways(-1);
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return sideways(1);
                case SDL_GAMEPAD_BUTTON_SOUTH: case SDL_GAMEPAD_BUTTON_EAST:
                case SDL_GAMEPAD_BUTTON_START:
                    return confirm();
                case SDL_GAMEPAD_BUTTON_BACK: case SDL_GAMEPAD_BUTTON_WEST: return back();
                default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
                int dir = e->gaxis.value > 20000 ? 1 : e->gaxis.value < -20000 ? -1 : 0;
                if (dir && dir != stick_dir) move(dir);
                stick_dir = dir;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            float x = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.x : e->button.x;
            float y = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.y : e->button.y;
            float scale = SDL_GetWindowPixelDensity(window);
            x *= scale;
            y *= scale;
            int click = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT;
            if (page == PAGE_SETTINGS) {
                hover_row = hover_choice = -1;
                for (int i = 0; i < seg_count; i++) {
                    const SDL_FRect *r = &segs[i].r;
                    if (x < r->x || x > r->x + r->w || y < r->y || y > r->y + r->h) continue;
                    sel_settings = hover_row = segs[i].row;
                    hover_choice = segs[i].choice;
                    if (click) return pick(segs[i].row, segs[i].choice);
                    return ALBW_LAUNCHER_NONE;
                }
            }
            for (int i = 0; i < hits; i++) {
                if (x < hit[i].x || x > hit[i].x + hit[i].w || y < hit[i].y || y > hit[i].y + hit[i].h)
                    continue;
                if (page == PAGE_SETTINGS) sel_settings = i;
                else sel_main = i;
                if (click) return confirm();
                break;
            }
            break;
        }
        default: break;
    }
    return ALBW_LAUNCHER_NONE;
}

// A rectangle shaded from one colour on the left to another on the right.
static void gradient(float x0, float y0, float x1, float y1, SDL_FColor left, SDL_FColor right,
                     int vertical) {
    SDL_Vertex v[4] = {
        { { x0, y0 }, left, { 0, 0 } }, { { x1, y0 }, vertical ? left : right, { 0, 0 } },
        { { x1, y1 }, right, { 0, 0 } }, { { x0, y1 }, vertical ? right : left, { 0, 0 } },
    };
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(renderer, NULL, v, 4, idx, 6);
}

static float approach(float v, float target, float rate, float dt) {
    float k = 1.0f - expf(-rate * dt);
    return v + (target - v) * k;
}

void albw_launcher_draw(SDL_Texture *top) {
    card_take();
    // The game being made, where the launcher makes it: shown in place of the card's line.
    struct albw_build_progress build;
    memset(&build, 0, sizeof build);
    int building = game_building(&build);
    int build_shown = game_ready && !game_ready() && (building || build.state == ALBW_BUILD_FAILED);
    uint64_t now = SDL_GetTicksNS();
    float dt = (float)(now - last_ns) * 1e-9f, t = (float)now * 1e-9f;
    if (dt > 0.1f) dt = 0.1f;
    last_ns = now;
    fade = approach(fade, want_visible ? 1.0f : 0.0f, 9.0f, dt);
    if (!want_visible && fade < 0.01f) fade = 0.0f;
    if (fade <= 0.0f) return;

    int W, H;
    SDL_GetCurrentRenderOutputSize(renderer, &W, &H);
    // Type sized by the window's height, as the rows that hold it are; only the title is held to
    // the width as well, so that it fits across a tall, narrow window.
    if (abs(H - baked_for_h) > H / 20) {
        bake(&f_title, serif_data, H * 0.085f > W * 0.075f ? W * 0.075f : H * 0.085f);
        bake(&f_kicker, small_data, H * 0.030f);
        bake(&f_item, sans_data, H * 0.045f);
        bake(&f_small, small_data, H * 0.023f);
        baked_for_h = H;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // The player's picture if there is one, else the game's own top screen as it was left (in game),
    // else nothing but shading; filling the window and drifting slowly.
    if (wallpaper) top = wallpaper;
    if (top) {
        float zoom = 1.04f + 0.02f * sinf(t * 0.08f);
        float tw, th;
        SDL_GetTextureSize(top, &tw, &th);
        float sw = (float)W * zoom, sh = sw * th / tw;
        if (sh < H * zoom) {
            sh = (float)H * zoom;
            sw = sh * tw / th;
        }
        SDL_FRect dst = { (W - sw) / 2.0f + 0.02f * W * sinf(t * 0.09f), (H - sh) / 2.0f, sw, sh };
        SDL_ScaleMode old;
        SDL_GetTextureScaleMode(top, &old);
        if (top != wallpaper) SDL_SetTextureScaleMode(top, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureAlphaModFloat(top, fade);
        SDL_RenderTexture(renderer, top, NULL, &dst);
        SDL_SetTextureAlphaModFloat(top, 1.0f);
        if (top != wallpaper) SDL_SetTextureScaleMode(top, old);
    }
    else {
        // No picture: a dark field with a warm light that wanders.
        gradient(0, 0, (float)W, (float)H, (SDL_FColor){ 0.05f, 0.04f, 0.06f, fade },
                 (SDL_FColor){ 0.20f + 0.05f * sinf(t * 0.2f), 0.12f, 0.06f, fade }, 0);
    }
    // The settings page dims the picture further, so that its rows -- the dimmed ones too -- read
    // against any part of it.
    dim_at = approach(dim_at, page == PAGE_SETTINGS ? 0.42f : 0.0f, 10.0f, dt);
    SDL_SetRenderDrawColorFloat(renderer, 0, 0, 0, ((wallpaper ? 0.15f : 0.35f) + dim_at) * fade);
    SDL_RenderFillRect(renderer, NULL);
    gradient(0, 0, W * 0.68f, (float)H, (SDL_FColor){ 0.02f, 0.01f, 0.01f, 0.92f * fade },
             (SDL_FColor){ 0, 0, 0, 0 }, 0);
    gradient(0, H * 0.75f, (float)W, (float)H, (SDL_FColor){ 0, 0, 0, 0 },
             (SDL_FColor){ 0, 0, 0, 0.6f * fade }, 1);

    const SDL_Color white = { 255, 255, 255, 255 }, gold = { 244, 196, 92, 255 },
                    grey = { 175, 170, 160, 255 }, green = { 140, 220, 140, 255 };
    float x0 = W * 0.06f;
    // The title sits higher on the settings page, to leave the rows their room; it moves there.
    float title_want = page == PAGE_SETTINGS ? 0.12f : 0.27f;
    title_at = title_at < 0.0f ? title_want : approach(title_at, title_want, 10.0f, dt);
    float y = H * title_at;
    text(&f_kicker, x0, y, "THE LEGEND OF ZELDA", white, 0.85f * fade);
    y += H * 0.095f;
    text(&f_title, x0, y, "A LINK BETWEEN WORLDS", gold, fade);

    hits = 0;
    seg_count = 0;
    float row_h = H * 0.065f;
    if (page == PAGE_MAIN) {
        y = H * 0.56f;
        static const char *labels[MAIN_ITEMS] = { "PLAY", "SETTINGS", "MODS", "QUIT" };
        for (int i = 0; i < MAIN_ITEMS; i++) {
            float *g = &glow[i];
            *g = approach(*g, i == sel_main ? 1.0f : 0.0f, 14.0f, dt);
            const char *label = (i == ITEM_PLAY && started) ? "RESUME"
                              : (i == ITEM_PLAY && !card_product[0] && card_chooser) ? "CHOOSE CARD"
                              : (i == ITEM_PLAY && building) ? "PREPARING..."
                              : labels[i];
            int dim = i == ITEM_MODS;
            float x = x0 + *g * W * 0.012f;
            if (*g > 0.02f) {
                SDL_SetRenderDrawColorFloat(renderer, 0.96f, 0.77f, 0.36f, *g * fade);
                SDL_FRect bar = { x0 - W * 0.014f, y - row_h * 0.62f, W * 0.004f, row_h * 0.72f };
                SDL_RenderFillRect(renderer, &bar);
            }
            SDL_Color c = dim ? grey : white;
            float a = (dim ? 0.45f : 0.72f + 0.28f * *g) * fade;
            float end = text(&f_item, x, y, label, c, a);
            if (dim) text(&f_small, end + W * 0.01f, y, "coming later", grey, 0.5f * fade);
            hit[hits++] = (SDL_FRect){ x0 - W * 0.02f, y - row_h * 0.7f, W * 0.3f, row_h };
            y += row_h;
        }
    } else {
        y = H * 0.28f;
        text(&f_item, x0, y, "SETTINGS", gold, fade);
        y += row_h * 0.85f;
        const SDL_Color dark = { 105, 101, 96, 255 };
        // As far apart as the rows have room for above the line that says what a choice does.
        float small_row = (H * 0.83f - y) / (float)SETTINGS_ROWS, gap = W * 0.016f;
        if (small_row > H * 0.046f) small_row = H * 0.046f;
        // The values start a little past the longest name, as wide as the type is, not the window.
        float names_w = 0.0f;
        for (int i = 0; i < SETTINGS_ROWS; i++) {
            float nw = text_width(&f_small, rows[i].name) + H * 0.05f;
            if (nw > names_w) names_w = nw;
        }
        for (int i = 0; i < SETTINGS_ROWS; i++) {
            const struct row_def *rd = &rows[i];
            float *g = &glow[MAIN_ITEMS + i];
            *g = approach(*g, i == sel_settings ? 1.0f : 0.0f, 14.0f, dt);
            float x = x0 + *g * W * 0.008f, ty = y - small_row * 0.08f;
            if (*g > 0.02f) {
                SDL_SetRenderDrawColorFloat(renderer, 0.96f, 0.77f, 0.36f, *g * fade);
                SDL_FRect bar = { x0 - W * 0.014f, y - small_row * 0.62f, W * 0.003f, small_row * 0.7f };
                SDL_RenderFillRect(renderer, &bar);
            }
            text(&f_small, x, ty, rd->name, white, (0.7f + 0.3f * *g) * fade);
            float vx = x0 + names_w;
            int *v = row_value(i);
            if (v && rd->side_by_side) {
                for (int c = 0; c < rd->count; c++) {
                    int on = c == *v, can = offered(i, c, NULL);
                    SDL_Color col = on ? (can ? gold : dark) : (can ? grey : dark);
                    float a = (on ? 1.0f : can ? 0.72f + 0.2f * *g : 0.62f) * fade;
                    if (c == hover_choice && i == hover_row && can && !on) a = 0.95f * fade;
                    float w = text_width(&f_small, rd->choices[c]);
                    text(&f_small, vx, ty, rd->choices[c], col, a);
                    if (on) {
                        if (can) SDL_SetRenderDrawColorFloat(renderer, 0.96f, 0.77f, 0.36f, 0.9f * fade);
                        else SDL_SetRenderDrawColorFloat(renderer, 0.47f, 0.45f, 0.43f, 0.6f * fade);
                        SDL_FRect line = { vx, ty + small_row * 0.14f, w, H * 0.0025f };
                        SDL_RenderFillRect(renderer, &line);
                    }
                    if (seg_count < (int)(sizeof segs / sizeof segs[0])) {
                        segs[seg_count].r = (SDL_FRect){ vx - gap * 0.5f, y - small_row * 0.7f,
                                                         w + gap, small_row };
                        segs[seg_count].row = i;
                        segs[seg_count].choice = c;
                        seg_count++;
                    }
                    vx += w + gap;
                }
            } else if (v) {
                char shown[64];
                snprintf(shown, sizeof shown, "<  %s  >", rd->choices[*v]);
                int live = i != ROW_SHARPNESS || sharpening_applies();
                text(&f_small, vx, ty, shown, !live ? dark : i == sel_settings ? gold : grey,
                     (live ? 0.75f + 0.25f * *g : 0.45f) * fade);
            }
            // Side by side, the choices are hit one by one; else the whole row is.
            float hit_w = rd->side_by_side ? names_w + W * 0.02f : W * 0.6f;
            hit[hits++] = (SDL_FRect){ x0 - W * 0.02f, y - small_row * 0.7f, hit_w, small_row };
            y += small_row;
        }
        // What the selected choice does -- or what it would take, where it is not to be had.
        int row = hover_row >= 0 ? hover_row : sel_settings;
        int *v = row_value(row);
        int choice = hover_row >= 0 ? hover_choice : (v ? *v : 0);
        const char *about = v ? describe(row, choice) : NULL;
        if (about) text(&f_small, x0, H * 0.865f, about, grey, 0.8f * fade);
    }

    // The card, and what to press.
    float yb = H * 0.93f;
    if (build_shown && page == PAGE_MAIN) {
        // The game being made from the card, the first time: which step, how far, and that it is
        // the only time. Or, where it failed, why, and where the log is.
        float bar_y = yb - H * 0.004f, bar_w = W * 0.42f, bar_h = H * 0.007f;
        if (build.state == ALBW_BUILD_FAILED) {
            text(&f_small, x0, yb - H * 0.096f, "THE GAME COULD NOT BE MADE", gold, 0.95f * fade);
            // The reason, in lines that fit.
            char line[720];                     // "Log: " and the log's path
            const char *p = build.error;
            float y = yb - H * 0.064f;
            for (int row = 0; row < 3 && *p; row++) {
                size_t take = strlen(p);
                if (take > 96) {
                    take = 96;
                    while (take > 40 && p[take] != ' ') take--;
                }
                snprintf(line, sizeof line, "%.*s", (int)take, p);
                text(&f_small, x0, y, line, grey, 0.8f * fade);
                p += take;
                while (*p == ' ') p++;
                y += H * 0.028f;
            }
            snprintf(line, sizeof line, "Log: %s", build.log);
            text(&f_small, x0, yb + H * 0.024f, line, grey, 0.55f * fade);
        } else {
            char line[220];
            text(&f_small, x0, yb - H * 0.064f, "PREPARING THE GAME -- ONLY THIS ONCE", gold, 0.95f * fade);
            snprintf(line, sizeof line, "Step %d of %d: %s", build.stage ? build.stage : 1,
                     build.stages ? build.stages : 5, build.what[0] ? build.what : "Starting");
            text(&f_small, x0, yb - H * 0.032f, line, white, 0.9f * fade);
            SDL_SetRenderDrawColorFloat(renderer, 1.0f, 1.0f, 1.0f, 0.12f * fade);
            SDL_FRect track = { x0, bar_y, bar_w, bar_h };
            SDL_RenderFillRect(renderer, &track);
            SDL_SetRenderDrawColorFloat(renderer, 0.96f, 0.77f, 0.36f, 0.9f * fade);
            float done = (build.stage > 0 ? (float)(build.stage - 1) : 0.0f) / (float)(build.stages ? build.stages : 5);
            float share = 1.0f / (float)(build.stages ? build.stages : 5);
            if (build.fraction >= 0.0f) {
                SDL_FRect fill = { x0, bar_y, bar_w * (done + share * build.fraction), bar_h };
                SDL_RenderFillRect(renderer, &fill);
            } else {
                // A step that cannot say how far it is: what is done, and a light running along
                // the step's own share.
                SDL_FRect fill = { x0, bar_y, bar_w * done, bar_h };
                SDL_RenderFillRect(renderer, &fill);
                float run = 0.5f + 0.5f * sinf(t * 2.4f);
                SDL_FRect light = { x0 + bar_w * (done + share * 0.8f * run), bar_y, bar_w * share * 0.2f, bar_h };
                SDL_RenderFillRect(renderer, &light);
            }
            text(&f_small, x0, yb + H * 0.032f,
                 "Your cartridge's code is translated for this computer. It takes a few minutes.",
                 grey, 0.6f * fade);
        }
    }
    if (!card_product[0] && card_chooser && page == PAGE_MAIN && !build_shown) {
        // No card yet: what Play will do, or why the last one picked was not taken.
        text(&f_small, x0, yb - H * 0.032f, card_why[0] ? "That file is not a card this build plays."
                                                          : "No card image yet.",
             card_why[0] ? gold : grey, 0.9f * fade);
        text(&f_small, x0, yb, card_why[0] ? card_why
                                           : "Choose a decrypted dump of your own cartridge; it is remembered.",
             grey, 0.7f * fade);
    }
    if (card_product[0] && page == PAGE_MAIN && !build_shown) {
        char line[96];
        char region = card_product[9];
        const char *where = region == 'E' ? "USA" : region == 'P' ? "Europe"
                          : region == 'J' ? "Japan" : region == 'K' ? "Korea" : "";
        snprintf(line, sizeof line, "3DS \xc2\xb7 %s \xc2\xb7 %s", where, card_product);
        text(&f_small, x0, yb - H * 0.032f, "Card ready.", green, 0.9f * fade);
        text(&f_small, x0, yb, line, grey, 0.7f * fade);
    }
    const char *hint = page == PAGE_SETTINGS ? "Arrows change \xc2\xb7 Esc back"
                                             : (started ? "Esc resumes \xc2\xb7 F1 settings" : "");
    if (*hint) text(&f_small, W - x0 - text_width(&f_small, hint), yb - H * 0.032f, hint, grey,
                    0.6f * fade);
    const char *version = "World Across 0.1";
    text(&f_small, W - x0 - text_width(&f_small, version), yb, version, grey, 0.6f * fade);
#ifdef ALBW_HAVE_DLSS
    // NVIDIA's licence for the DLSS SDK (section 7.1 b of its RTX supplement) asks a program that
    // includes it to name the SDK on its splash screen, about box and credits; the launcher's
    // first page is all three here. Only a build with the SDK shows it.
    if (page == PAGE_MAIN) {
        const char *credit = "NVIDIA DLSS";
        text(&f_small, W - x0 - text_width(&f_small, credit), yb + H * 0.032f, credit, grey, 0.5f * fade);
    }
#endif
}
