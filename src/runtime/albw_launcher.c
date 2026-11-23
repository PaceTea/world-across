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
#if __has_include(<stb/stb_image_write.h>)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include <stb/stb_image_write.h>
#pragma GCC diagnostic pop
#define HAVE_STB_WRITE 1
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
    .frame_rate = ALBW_RATE_CONSOLE, .sound = 1, .gamepads = 1,
    .touch_controls = ALBW_TOUCH_ALL,
};

// The words the settings file uses for each choice, in the order of the enums.
static const char *const layout_keys[] = { "stacked", "side", "top" };
static const char *const upscale_keys[ALBW_UPSCALERS] = {
    "off", "bilinear", "fsr1", "fsr3", "dlss", "software",
};
static const char *const rate_keys[ALBW_RATES] = { "console", "display", "uncapped" };
static const char *const renderer_keys[ALBW_RENDERERS] = { "gpu", "software" };
static const char *const resolution_keys[ALBW_RESOLUTIONS] = {
    "auto", "1", "2", "3", "4", "5", "6", "7", "8",
};
static const char *const aa_keys[ALBW_AAS] = { "off", "2", "4", "8" };
static const char *const touch_keys[ALBW_TOUCHES] = { "mouse", "touchpad", "gyro", "stick", "all" };

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
               "sharpness=%d\nframerate=%s\ninteger=%d\nsound=%d\ngamepads=%d\n"
               "touch=%s\nlanguage=%s\n",
            settings.fullscreen, layout_keys[settings.layout], renderer_keys[settings.renderer],
            resolution_keys[settings.resolution], aa_keys[settings.antialias],
            upscale_keys[settings.upscale],
            settings.sharpness, rate_keys[settings.frame_rate],
            settings.integer_scale, settings.sound, settings.gamepads,
            touch_keys[settings.touch_controls], languages[settings.language].code);
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
        else if (!strcmp(line, "integer")) settings.integer_scale = atoi(v) != 0;
        else if (!strcmp(line, "sound")) settings.sound = atoi(v) != 0;
        else if (!strcmp(line, "gamepads")) settings.gamepads = atoi(v) != 0;
        else if (!strcmp(line, "touch"))                 // "both" was touchpad and gyro, before the stick
            settings.touch_controls = !strcmp(v, "both") ? ALBW_TOUCH_ALL
                                    : key_index(touch_keys, ALBW_TOUCHES, v, ALBW_TOUCH_ALL);
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
static int wallpaper_is_scene;                           // the kept backdrop, not the player's picture
static int wallpaper_is_art;                             // the launcher's own picture, credited below

// The launcher's picture, made for World Across by teethegrinder (third_party/art). Shown with a
// button in the corner that names the artist, and a second press opens the artist's page.
#define ARTIST_NAME "teethegrinder"
#define ARTIST_URL "https://www.instagram.com/teethegrinder"
static int artist_focus, artist_open;                   // selected (keys, pad, mouse); name shown
static float artist_glow, artist_w;
static SDL_FRect artist_hit;
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

// A file the packages bring for the menu, in `dir` (fonts/, art/): beside the program, where an
// AppImage keeps it, or in the checkout it was built in. 1 and its path if it is there.
static int bundled_path(const char *dir, const char *file, char *out, size_t n) {
    char exe[600];
    albw_exe_dir(exe, sizeof exe);
    const char *const layouts[] = { "%s/%s/%s", "%s/../share/world-across/%s/%s",
                                    "%s/../third_party/%s/%s" };
    for (size_t i = 0; i < sizeof layouts / sizeof layouts[0]; i++) {
        snprintf(out, n, layouts[i], exe, dir, file);
        FILE *f = fopen(out, "rb");
        if (f) {
            fclose(f);
            return 1;
        }
    }
    return 0;
}

// The menu's own type, Fira Sans Condensed (SIL Open Font License).
static unsigned char *bundled_font(const char *file) {
    char path[800];
    return bundled_path("fonts", file, path, sizeof path) ? read_file(path) : NULL;
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
       ROW_SHARPNESS, ROW_FRAME_RATE, ROW_INTEGER, ROW_SOUND, ROW_GAMEPADS,
       ROW_TOUCH, ROW_LANGUAGE, ROW_BACK, SETTINGS_ROWS };

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
static const char *const renderer_names[ALBW_RENDERERS] = { "GPU", "Software" };
static const char *const aa_names[ALBW_AAS] = { "Off", "2x MSAA", "4x MSAA", "8x MSAA" };
static const char *const touch_names[ALBW_TOUCHES] = { "Mouse", "Touchpad", "Gyro", "Stick", "All" };
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
    [ROW_INTEGER] = { "Whole-pixel scaling", on_off, 2, 1, -1 },
    [ROW_SOUND] = { "Sound", on_off, 2, 1, -1 },
    [ROW_GAMEPADS] = { "Gamepads", on_off, 2, 1, -1 },
    [ROW_TOUCH] = { "Touch Screen Controls", touch_names, ALBW_TOUCHES, 1, -1 },
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
        case ROW_INTEGER: return &settings.integer_scale;
        case ROW_SOUND: return &settings.sound;
        case ROW_GAMEPADS: return &settings.gamepads;
        case ROW_TOUCH: return &settings.touch_controls;
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
        case ROW_TOUCH: {
            static const char *const text[ALBW_TOUCHES] = {
                "The mouse on the bottom screen is the stylus; gamepads leave the touch screen alone.",
                "A finger on the touchpad shows a ring on the touch screen; press the pad in to touch.",
                "Right stick in: a ring you aim by tilting the gamepad. In again to touch, held to drag.",
                "The right stick moves a ring over the touch screen; pressed in, it touches there.",
                "Touchpad, right stick and gyroscope, whichever the gamepad has. The mouse works too.",
            };
            return text[choice];
        }
        case ROW_LANGUAGE: return "The game's language, from the next start.";
        default: return NULL;
    }
}

static int want_visible = 1, page = PAGE_MAIN, sel_main, sel_settings, started;
static float fade = 0.0f, glow[SETTINGS_ROWS + MAIN_ITEMS], title_at = -1.0f, dim_at;
static uint64_t last_ns;
static char card_product[17], card_path[1024];
static SDL_FRect hit[SETTINGS_ROWS];                    // where each row was drawn, for the mouse
static int hits;
// Where each choice of a side-by-side row was drawn, to be clicked.
static struct { SDL_FRect r; int row, choice; } segs[64];
static int seg_count, hover_row = -1, hover_choice = -1;
static int stick_dir;

void albw_launcher_set_card(const char *product, const char *path) {
    snprintf(card_product, sizeof card_product, "%s", product ? product : "");
    snprintf(card_path, sizeof card_path, "%s", product && product[0] && path ? path : "");
}

// The card, when none is known yet -- started from a desktop's menu, the first time: Play opens
// SDL's file dialog (which goes through the desktop's own), and what is picked is checked and
// remembered by the chooser main() hands over. The dialog answers on a thread of its own, so its
// answer waits here until the next frame takes it. The chooser moves the card into the program's
// card folder (albw_card.h), which across file systems is a copy of a gigabyte, so it runs on a
// thread too, and the launcher shows how far it is.
static int (*card_chooser)(const char *path, char product[17], char *where, size_t where_n,
                           char *why, size_t n, volatile float *progress);
static SDL_Mutex *card_mutex;
static char card_picked_path[1024], card_why[240];
// 0 closed, 1 open, 2 a path waiting, 3 the chooser at it, 4 its answer waiting.
static int card_dialog;
static SDL_Thread *card_thread;
static volatile float card_progress = -1.0f;            // the move's, while it copies
static int card_taken, card_busy;
static char card_taken_product[17], card_taken_path[1024], card_taken_why[240];

void albw_launcher_set_card_chooser(int (*chooser)(const char *path, char product[17], char *where,
                                                   size_t where_n, char *why, size_t n,
                                                   volatile float *progress)) {
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

// The chooser, on its thread: the dialog cannot open again meanwhile, so the path stays as it is.
static int SDLCALL card_take_thread(void *unused) {
    (void)unused;
    char product[17] = "", where[1024] = "", why[240] = "";
    int ok = card_chooser(card_picked_path, product, where, sizeof where, why, sizeof why,
                          &card_progress);
    SDL_LockMutex(card_mutex);
    card_taken = ok;
    memcpy(card_taken_product, product, sizeof product);
    snprintf(card_taken_path, sizeof card_taken_path, "%s", where);
    snprintf(card_taken_why, sizeof card_taken_why, "%s", why);
    card_dialog = 4;
    SDL_UnlockMutex(card_mutex);
    return 0;
}

// A path the dialog picked: checked and moved on a thread, and taken when that is done. Called each
// frame the launcher is drawn.
static void card_take(void) {
    if (!card_mutex) return;
    SDL_LockMutex(card_mutex);
    int state = card_dialog;
    if (state == 2) card_dialog = card_chooser ? 3 : 0;
    if (state == 4) card_dialog = 0;
    SDL_UnlockMutex(card_mutex);
    card_busy = state == 2 || state == 3;
    if (state == 2 && card_chooser) {
        card_progress = -1.0f;
        card_thread = SDL_CreateThread(card_take_thread, "card", NULL);
        if (!card_thread) card_take_thread(NULL);
        return;
    }
    if (state != 4) return;
    card_busy = 0;
    if (card_thread) SDL_WaitThread(card_thread, NULL);
    card_thread = NULL;
    if (card_taken) {
        snprintf(card_product, sizeof card_product, "%s", card_taken_product);
        snprintf(card_path, sizeof card_path, "%s", card_taken_path);
        card_why[0] = '\0';
        // The launcher's build: a card chosen is a game to make, and then to play.
        if (game_ready && !game_ready()) game_start_build();
    } else {
        snprintf(card_why, sizeof card_why, "%s", card_taken_why);
    }
}

static SDL_Mutex *prompt_mutex;                         // the software keyboard's, below

int albw_launcher_init(SDL_Window *w, SDL_Renderer *r) {
    window = w;
    renderer = r;
    if (!prompt_mutex) prompt_mutex = SDL_CreateMutex();
    for (int i = 0; i < LANGUAGES; i++) language_names[i] = languages[i].name;
    // A condensed sans for the menu, as launchers of this kind have it: Fira Sans Condensed, the
    // copy the package brings or else one installed, else whatever sans the system has -- on
    // Windows Segoe UI, which every release since Vista has.
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
    serif_data = bundled_font("FiraSansCondensed-SemiBold.ttf");
    sans_data = bundled_font("FiraSansCondensed-Medium.ttf");
    small_data = bundled_font("FiraSansCondensed-Regular.ttf");
    if (!serif_data) serif_data = find_font_of("Fira Sans Condensed:semibold", "Fira Sans Condensed", title);
    if (!sans_data) sans_data = find_font_of("Fira Sans Condensed:medium", "Fira Sans Condensed", item);
    if (!small_data) small_data = find_font_of("Fira Sans Condensed", "Fira Sans Condensed", small);
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
    // settings. Without one, the launcher's own (art/, which the packages bring); without that, the
    // scene the game was left in last time (albw_launcher_keep_backdrop), from the player's card.
#if HAVE_STB_IMAGE
    static const char *const names[] = { "background.png", "background.jpg", "background.jpeg",
                                         "launcher-background.jpg", "backdrop.jpg" };
    for (size_t i = 0; i < sizeof names / sizeof names[0] && !wallpaper; i++) {
        char path[800];
        if (i < 3) {
            albw_config_path(path, sizeof path, names[i]);
        } else if (i == 3) {
            if (!bundled_path("art", names[i], path, sizeof path)) continue;
        } else {
            char root[500];
            albw_cache_root(root, sizeof root);
            snprintf(path, sizeof path, "%s/%s", root, names[i]);
        }
        int iw, ih, n;
        unsigned char *px = stbi_load(path, &iw, &ih, &n, 4);
        if (!px) continue;
        wallpaper = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, iw, ih);
        if (wallpaper) {
            SDL_UpdateTexture(wallpaper, NULL, px, iw * 4);
            SDL_SetTextureScaleMode(wallpaper, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(wallpaper, SDL_BLENDMODE_BLEND);
            wallpaper_is_art = i == 3;
            wallpaper_is_scene = i == 4;
            printf("[launch] background %s, %dx%d\n", path, iw, ih);
        }
        stbi_image_free(px);
    }
#endif
    const char *e = getenv("ALBW_LAUNCHER");
    if (e && !strcmp(e, "0")) want_visible = 0;
    if (e && !strcmp(e, "settings")) page = PAGE_SETTINGS;   // for screenshots of that page
    if (e && !strcmp(e, "artist")) artist_focus = artist_open = 1;
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

// The artist's button: the first press shows the name, the next opens the artist's page.
static void artist_press(void) {
    if (!artist_open) {
        artist_open = 1;
        return;
    }
    if (!SDL_OpenURL(ARTIST_URL)) printf("[launch] %s does not open: %s\n", ARTIST_URL, SDL_GetError());
}

static enum albw_launcher_action confirm(void) {
    if (page == PAGE_MAIN && artist_focus) {
        artist_press();
        return ALBW_LAUNCHER_NONE;
    }
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
            if (card_busy) return ALBW_LAUNCHER_NONE;     // the card picked is being moved
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
    if (page == PAGE_MAIN && (artist_focus || artist_open)) {
        artist_focus = artist_open = 0;
        return ALBW_LAUNCHER_NONE;
    }
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
    else if (artist_focus) artist_focus = 0;            // up or down: back to the menu
    else sel_main = (sel_main + MAIN_ITEMS + dir) % MAIN_ITEMS;
    hover_row = hover_choice = -1;
}

static enum albw_launcher_action sideways(int dir) {
    if (page == PAGE_MAIN && wallpaper_is_art) {        // right to the artist's button, left back
        artist_focus = dir > 0;
        if (!artist_focus) artist_open = 0;
        return ALBW_LAUNCHER_NONE;
    }
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
            if (page == PAGE_MAIN && wallpaper_is_art) {
                const SDL_FRect *r = &artist_hit;
                if (x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h) {
                    artist_focus = 1;
                    if (click) artist_press();
                    return ALBW_LAUNCHER_NONE;
                }
                if (click) artist_open = 0;              // a click anywhere else folds it away
            }
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
                else {
                    sel_main = i;
                    artist_focus = 0;
                }
                if (click) return confirm();
                break;
            }
            break;
        }
        default: break;
    }
    return ALBW_LAUNCHER_NONE;
}

// Type sized by the window's height, as the rows that hold it are; only the title is held to the
// width as well, so that it fits across a tall, narrow window.
static void fonts_for(int W, int H) {
    if (abs(H - baked_for_h) <= H / 20) return;
    bake(&f_title, serif_data, H * 0.085f > W * 0.075f ? W * 0.075f : H * 0.085f);
    bake(&f_kicker, small_data, H * 0.030f);
    bake(&f_item, sans_data, H * 0.045f);
    bake(&f_small, small_data, H * 0.023f);
    baked_for_h = H;
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
    fonts_for(W, H);
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
    SDL_SetRenderDrawColorFloat(renderer, 0, 0, 0,
                                ((wallpaper && !wallpaper_is_scene ? 0.15f : 0.35f) + dim_at) * fade);
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
            *g = approach(*g, i == sel_main && !artist_focus ? 1.0f : 0.0f, 14.0f, dt);
            const char *label = (i == ITEM_PLAY && started) ? "RESUME"
                              : (i == ITEM_PLAY && (building || card_busy)) ? "PREPARING..."
                              : (i == ITEM_PLAY && !card_product[0] && card_chooser) ? "CHOOSE CARD"
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
    if (card_busy && page == PAGE_MAIN && !build_shown) {
        // The card picked, on its way into the card folder: a rename, or a copy that says how far.
        text(&f_small, x0, yb - H * 0.032f, "Moving the card into the card folder...", white, 0.9f * fade);
        float p = card_progress;
        if (p >= 0.0f) {
            float bar_y = yb - H * 0.004f, bar_w = W * 0.42f, bar_h = H * 0.007f;
            SDL_SetRenderDrawColorFloat(renderer, 1.0f, 1.0f, 1.0f, 0.12f * fade);
            SDL_FRect track = { x0, bar_y, bar_w, bar_h };
            SDL_RenderFillRect(renderer, &track);
            SDL_SetRenderDrawColorFloat(renderer, 0.96f, 0.77f, 0.36f, 0.9f * fade);
            SDL_FRect fill = { x0, bar_y, bar_w * (p > 1.0f ? 1.0f : p), bar_h };
            SDL_RenderFillRect(renderer, &fill);
        }
        text(&f_small, x0, yb + H * 0.032f, "It stays there, beside the program, and is played from there.",
             grey, 0.6f * fade);
    }
    if (!card_product[0] && card_chooser && page == PAGE_MAIN && !build_shown && !card_busy) {
        // No card yet: what Play will do, or why the last one picked was not taken.
        text(&f_small, x0, yb - H * 0.032f, card_why[0] ? "That file is not a card this build plays."
                                                          : "No card image yet.",
             card_why[0] ? gold : grey, 0.9f * fade);
        text(&f_small, x0, yb, card_why[0] ? card_why
                                           : "Choose a decrypted dump of your own cartridge; it moves into the card folder.",
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
        // Where it is: the card folder, normally, under the home directory as ~.
        if (card_path[0]) {
            char dir[1024], place[1100];
            snprintf(dir, sizeof dir, "%s", card_path);
            char *a = strrchr(dir, '/'), *b = strrchr(dir, '\\');
            if (a > b ? a : b) *(a > b ? a : b) = '\0';
            const char *home = getenv("HOME");
            size_t hn = home ? strlen(home) : 0;
            if (hn > 1 && !strncmp(dir, home, hn) && (dir[hn] == '/' || !dir[hn]))
                snprintf(place, sizeof place, "In ~%s", dir + hn);
            else
                snprintf(place, sizeof place, "In %s", dir);
            text(&f_small, x0, yb + H * 0.032f, place, grey, 0.5f * fade);
        }
    }
    // The hint above the corner, higher where the artist's button sits in it; the button's own
    // hint, once the name is shown, in its place.
    int artist_shown = page == PAGE_MAIN && wallpaper_is_art;
    const char *hint = page == PAGE_SETTINGS ? "Arrows change \xc2\xb7 Esc back"
                     : artist_shown && artist_open && artist_focus ? "Opens the artist's Instagram"
                     : (started ? "Esc resumes \xc2\xb7 F1 settings" : "");
    float hint_y = yb - H * (artist_shown ? 0.062f : 0.032f);
    if (*hint) text(&f_small, W - x0 - text_width(&f_small, hint), hint_y, hint, grey, 0.6f * fade);
#ifndef ALBW_VERSION
#define ALBW_VERSION "0.1"
#endif
    const char *version = "World Across " ALBW_VERSION;
    float version_right = W - x0;
    if (artist_shown) {
        // The artist's button, in the corner: ARTIST, then the name, which opens the artist's page.
        // It widens to the name smoothly; the version line keeps to its left.
        const char *label = artist_open ? ARTIST_NAME : "ARTIST";
        float pad = H * 0.018f, bh = H * 0.042f;
        float want = text_width(&f_small, label) + 2.0f * pad;
        artist_w = artist_w <= 0.0f ? want : approach(artist_w, want, 16.0f, dt);
        artist_glow = approach(artist_glow, artist_focus ? 1.0f : 0.0f, 14.0f, dt);
        SDL_FRect box = { W - x0 - artist_w, yb - bh * 0.72f, artist_w, bh };
        artist_hit = box;
        SDL_SetRenderDrawColorFloat(renderer, 0.0f, 0.0f, 0.0f, (0.35f + 0.2f * artist_glow) * fade);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColorFloat(renderer, 1.0f + (0.96f - 1.0f) * artist_glow,
                                    1.0f + (0.77f - 1.0f) * artist_glow,
                                    1.0f + (0.36f - 1.0f) * artist_glow, (0.3f + 0.6f * artist_glow) * fade);
        SDL_RenderRect(renderer, &box);
        float tw = text_width(&f_small, label);
        text(&f_small, box.x + (box.w - tw) * 0.5f, yb, label, artist_glow > 0.5f ? gold : white,
             (0.75f + 0.25f * artist_glow) * fade);
        version_right = box.x - W * 0.012f;
    }
    text(&f_small, version_right - text_width(&f_small, version), yb, version, grey, 0.6f * fade);
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

// ---------------------------------------------------------------------------------------------
// The software keyboard. When the game asks for text -- the player's name -- a field over the
// window takes it: typed on the keyboard, or picked from keys drawn below it with a gamepad (A
// types, B deletes, X shifts, Y is a space, Start confirms) or the mouse. The game waits for it as
// it waits for the console's keyboard. The kernel asks from the game's thread, the window answers
// from its own, so what passes between them is under a lock.

static SDL_Mutex *prompt_mutex;
static int prompt_asked, prompt_answered;               // under prompt_mutex
static unsigned prompt_max, prompt_n, prompt_offered_n, prompt_answer_n;
static uint16_t prompt_text[65], prompt_offered[65], prompt_answer[65];
static int prompt_open;                                  // the window's: the field is up
static int prompt_row = 1, prompt_col, prompt_shift = 1, prompt_stick_x, prompt_stick_y;
static uint64_t prompt_since;
static const char *const prompt_keys[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM.'!" };
enum { PROMPT_SHIFT, PROMPT_SPACE, PROMPT_DELETE, PROMPT_OK, PROMPT_SPECIALS };
static const char *const prompt_special_names[PROMPT_SPECIALS] = { "Shift", "Space", "Delete", "OK" };
static struct { SDL_FRect r; int row, col; } prompt_hits[48];
static int prompt_hit_count;

int albw_prompt_ask(const uint16_t *text, unsigned n, unsigned max_len) {
    if (!prompt_mutex || !small_data || !sans_data) return 0;    // nothing to set the field in
    if (max_len > 64) max_len = 64;
    if (n > max_len) n = max_len;
    SDL_LockMutex(prompt_mutex);
    prompt_max = max_len;
    prompt_n = prompt_offered_n = n;
    memcpy(prompt_text, text, n * sizeof *text);
    memcpy(prompt_offered, text, n * sizeof *text);
    prompt_asked = 1;
    prompt_answered = 0;
    SDL_UnlockMutex(prompt_mutex);
    return 1;
}

int albw_prompt_answer(uint16_t *text, unsigned *n) {
    if (!prompt_mutex) return 0;
    SDL_LockMutex(prompt_mutex);
    int done = prompt_answered;
    if (done) {
        memcpy(text, prompt_answer, prompt_answer_n * sizeof *text);
        *n = prompt_answer_n;
        prompt_answered = 0;
    }
    SDL_UnlockMutex(prompt_mutex);
    return done;
}

static void prompt_finish(int keep_offered);
static void prompt_type(unsigned cp);

int albw_prompt_visible(void) {
    if (!prompt_mutex) return 0;
    SDL_LockMutex(prompt_mutex);
    int asked = prompt_asked;
    SDL_UnlockMutex(prompt_mutex);
    if (asked && !prompt_open) {
        prompt_open = 1;
        prompt_row = 1;
        prompt_col = 0;
        prompt_since = SDL_GetTicksNS();
        SDL_StartTextInput(window);
        // ALBW_PROMPT_TYPE=<text> types that in place of the offered text and confirms it, for
        // checking the way back to the game without a hand on the keyboard.
        const char *typed = getenv("ALBW_PROMPT_TYPE");
        if (typed && *typed) {
            prompt_n = 0;
            while (*typed) prompt_type(next_cp(&typed));
            prompt_finish(0);
            return 0;
        }
    }
    return asked;
}

static void prompt_finish(int keep_offered) {
    SDL_LockMutex(prompt_mutex);
    if (keep_offered || !prompt_n) {
        memcpy(prompt_answer, prompt_offered, prompt_offered_n * sizeof *prompt_answer);
        prompt_answer_n = prompt_offered_n;
    } else {
        memcpy(prompt_answer, prompt_text, prompt_n * sizeof *prompt_answer);
        prompt_answer_n = prompt_n;
    }
    prompt_asked = 0;
    prompt_answered = 1;
    SDL_UnlockMutex(prompt_mutex);
    prompt_open = 0;
    SDL_StopTextInput(window);
}

// A character the field takes: what the menu's type can show, which is what a name needs.
static void prompt_type(unsigned cp) {
    if (cp < 32 || cp == 127 || (cp >= 128 && cp < 160) || cp >= FIRST_CHAR + CHARS) return;
    if (prompt_n >= prompt_max) return;
    prompt_text[prompt_n++] = (uint16_t)cp;
    // After a capital the keys go small, as a name goes on.
    if (prompt_shift == 1 && cp != ' ') prompt_shift = 0;
}

static void prompt_press(int row, int col) {
    if (row < 4) {
        char ch = prompt_keys[row][col];
        if (ch >= 'A' && ch <= 'Z' && !prompt_shift) ch = (char)(ch - 'A' + 'a');
        prompt_type((unsigned char)ch);
        return;
    }
    switch (col) {
        case PROMPT_SHIFT: prompt_shift = !prompt_shift; break;
        case PROMPT_SPACE: prompt_type(' '); break;
        case PROMPT_DELETE: if (prompt_n) prompt_n--; break;
        case PROMPT_OK: if (prompt_n) prompt_finish(0); break;
    }
}

static void prompt_move(int dx, int dy) {
    int rows = 5;
    prompt_row = (prompt_row + dy + rows) % rows;
    int cols = prompt_row < 4 ? 10 : PROMPT_SPECIALS;
    if (dy) {
        // Between the letters and the wide keys below, keep to the same part of the board.
        if (prompt_row == 4 && dy) prompt_col = prompt_col * PROMPT_SPECIALS / 10;
        else if (prompt_row == 0 && dy > 0) prompt_col = prompt_col * 10 / PROMPT_SPECIALS;
        else if (prompt_row == 3 && dy < 0) prompt_col = prompt_col * 10 / PROMPT_SPECIALS;
    }
    if (prompt_col >= cols) prompt_col = cols - 1;
    prompt_col = (prompt_col + dx + cols) % cols;
}

void albw_prompt_event(const SDL_Event *e) {
    switch (e->type) {
        case SDL_EVENT_TEXT_INPUT: {
            const char *p = e->text.text;
            while (*p) prompt_type(next_cp(&p));
            break;
        }
        case SDL_EVENT_KEY_DOWN:
            switch (e->key.scancode) {
                case SDL_SCANCODE_BACKSPACE: if (prompt_n) prompt_n--; break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
                    if (!e->key.repeat && prompt_n) prompt_finish(0);
                    break;
                case SDL_SCANCODE_ESCAPE: if (!e->key.repeat) prompt_finish(1); break;
                case SDL_SCANCODE_UP: prompt_move(0, -1); break;
                case SDL_SCANCODE_DOWN: prompt_move(0, 1); break;
                case SDL_SCANCODE_LEFT: prompt_move(-1, 0); break;
                case SDL_SCANCODE_RIGHT: prompt_move(1, 0); break;
                default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            switch (e->gbutton.button) {
                case SDL_GAMEPAD_BUTTON_DPAD_UP: prompt_move(0, -1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN: prompt_move(0, 1); break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT: prompt_move(-1, 0); break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: prompt_move(1, 0); break;
                case SDL_GAMEPAD_BUTTON_EAST: prompt_press(prompt_row, prompt_col); break;   // A
                case SDL_GAMEPAD_BUTTON_SOUTH: if (prompt_n) prompt_n--; break;              // B
                case SDL_GAMEPAD_BUTTON_NORTH: prompt_shift = !prompt_shift; break;          // X
                case SDL_GAMEPAD_BUTTON_WEST: prompt_type(' '); break;                       // Y
                case SDL_GAMEPAD_BUTTON_START: if (prompt_n) prompt_finish(0); break;
                default: break;
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            int *last = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX ? &prompt_stick_x
                      : e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY ? &prompt_stick_y : NULL;
            if (!last) break;
            int dir = e->gaxis.value > 20000 ? 1 : e->gaxis.value < -20000 ? -1 : 0;
            if (dir && dir != *last) {
                if (last == &prompt_stick_x) prompt_move(dir, 0);
                else prompt_move(0, dir);
            }
            *last = dir;
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            float x = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.x : e->button.x;
            float y = e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.y : e->button.y;
            float scale = SDL_GetWindowPixelDensity(window);
            x *= scale;
            y *= scale;
            for (int i = 0; i < prompt_hit_count; i++) {
                const SDL_FRect *r = &prompt_hits[i].r;
                if (x < r->x || x > r->x + r->w || y < r->y || y > r->y + r->h) continue;
                prompt_row = prompt_hits[i].row;
                prompt_col = prompt_hits[i].col;
                if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT)
                    prompt_press(prompt_row, prompt_col);
                break;
            }
            break;
        }
        default: break;
    }
}

// UTF-16 to UTF-8, for the field.
static void prompt_utf8(const uint16_t *t, unsigned n, char *out, size_t size) {
    size_t w = 0;
    for (unsigned i = 0; i < n && w + 4 < size; i++) {
        unsigned c = t[i];
        if (c < 0x80) out[w++] = (char)c;
        else if (c < 0x800) {
            out[w++] = (char)(0xC0 | (c >> 6));
            out[w++] = (char)(0x80 | (c & 0x3F));
        } else out[w++] = '?';
    }
    out[w] = '\0';
}

static void prompt_box(float x, float y, float w, float h, float r, float g, float b, float a) {
    SDL_SetRenderDrawColorFloat(renderer, r, g, b, a);
    SDL_FRect rect = { x, y, w, h };
    SDL_RenderFillRect(renderer, &rect);
}

void albw_prompt_draw(void) {
    if (!prompt_open) return;
    int W, H;
    SDL_GetCurrentRenderOutputSize(renderer, &W, &H);
    fonts_for(W, H);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const SDL_Color white = { 255, 255, 255, 255 }, gold = { 244, 196, 92, 255 },
                    grey = { 175, 170, 160, 255 }, dark = { 30, 22, 14, 255 };
    prompt_box(0, 0, (float)W, (float)H, 0.03f, 0.02f, 0.02f, 0.88f);

    float pw = fminf(W * 0.92f, H * 1.05f), x0 = (W - pw) / 2.0f;
    float y = H * 0.22f;
    text(&f_kicker, x0, y, "YOUR NAME", gold, 1.0f);
    char count[32];
    snprintf(count, sizeof count, "%u / %u", prompt_n, prompt_max);
    text(&f_small, x0 + pw - text_width(&f_small, count), y, count, grey, 0.8f);

    // The field, with a caret that blinks.
    float fy = y + H * 0.025f, fh = H * 0.085f;
    prompt_box(x0, fy, pw, fh, 1, 1, 1, 0.10f);
    prompt_box(x0, fy + fh - 2.0f, pw, 2.0f, 0.96f, 0.77f, 0.36f, 0.9f);
    char field[200];
    prompt_utf8(prompt_text, prompt_n, field, sizeof field);
    float end = text(&f_item, x0 + H * 0.02f, fy + fh * 0.70f, field, white, 1.0f);
    if (((SDL_GetTicksNS() - prompt_since) / 500000000u) % 2 == 0)
        prompt_box(end + 2.0f, fy + fh * 0.22f, 2.0f, fh * 0.56f, 1, 1, 1, 0.9f);

    // The keys: four rows of ten, and the wide ones below.
    float ky = fy + fh + H * 0.04f, gap = pw * 0.012f;
    float kw = (pw - gap * 9) / 10.0f, kh = fminf(kw * 0.8f, H * 0.075f);
    prompt_hit_count = 0;
    for (int row = 0; row < 5; row++) {
        int cols = row < 4 ? 10 : PROMPT_SPECIALS;
        float w = row < 4 ? kw : (pw - gap * (PROMPT_SPECIALS - 1)) / PROMPT_SPECIALS;
        for (int col = 0; col < cols; col++) {
            float kx = x0 + col * (w + gap), yy = ky + row * (kh + gap);
            int sel = row == prompt_row && col == prompt_col;
            int lit = row == 4 && col == PROMPT_SHIFT && prompt_shift;
            if (sel) prompt_box(kx, yy, w, kh, 0.96f, 0.77f, 0.36f, 0.95f);
            else prompt_box(kx, yy, w, kh, 1, 1, 1, lit ? 0.22f : 0.08f);
            char label[8];
            if (row < 4) {
                char ch = prompt_keys[row][col];
                if (ch >= 'A' && ch <= 'Z' && !prompt_shift) ch = (char)(ch - 'A' + 'a');
                label[0] = ch;
                label[1] = '\0';
            } else {
                snprintf(label, sizeof label, "%s", prompt_special_names[col]);
            }
            const struct font *f = row < 4 ? &f_item : &f_small;
            float tw = text_width(f, label);
            text(f, kx + (w - tw) / 2.0f, yy + kh * (row < 4 ? 0.72f : 0.64f), label,
                 sel ? dark : white, row == 4 && col == PROMPT_OK && !prompt_n ? 0.4f : 1.0f);
            if (prompt_hit_count < (int)(sizeof prompt_hits / sizeof prompt_hits[0])) {
                prompt_hits[prompt_hit_count].r = (SDL_FRect){ kx, yy, w, kh };
                prompt_hits[prompt_hit_count].row = row;
                prompt_hits[prompt_hit_count].col = col;
                prompt_hit_count++;
            }
        }
    }
    char hint[200], offered[100];
    prompt_utf8(prompt_offered, prompt_offered_n, offered, sizeof offered);
    float hy = ky + 5 * (kh + gap) + H * 0.03f;
    text(&f_small, x0, hy, "Type it, or pick keys: A types, B deletes, X shifts, Y is a space.", grey, 0.85f);
    snprintf(hint, sizeof hint, "Enter or Start confirms. Esc keeps \"%s\".", offered);
    text(&f_small, x0, hy + H * 0.032f, hint, grey, 0.85f);
}

// ---------------------------------------------------------------------------------------------
// The backdrop: the top screen as the game was left, kept in the cache for the menu's next start.
// Made from the player's own game, on their machine, and kept there.
void albw_launcher_keep_backdrop(SDL_Surface *shot) {
#if HAVE_STB_WRITE
    if (!shot) return;
    SDL_Surface *rgb = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_RGB24);
    if (!rgb) return;
    unsigned char *packed = malloc((size_t)rgb->w * (size_t)rgb->h * 3u);
    if (packed) {
        for (int y = 0; y < rgb->h; y++)
            memcpy(packed + (size_t)y * rgb->w * 3, (unsigned char *)rgb->pixels + (size_t)y * rgb->pitch,
                   (size_t)rgb->w * 3);
        char root[500], path[600];
        albw_cache_root(root, sizeof root);
        albw_mkdir(root);
        snprintf(path, sizeof path, "%s/backdrop.jpg", root);
        char tmp[620];
        snprintf(tmp, sizeof tmp, "%s.new", path);
        if (stbi_write_jpg(tmp, rgb->w, rgb->h, 3, packed, 90) && albw_rename(tmp, path) == 0)
            printf("[launch] kept the scene as the menu's backdrop (%dx%d)\n", rgb->w, rgb->h);
        free(packed);
    }
    SDL_DestroySurface(rgb);
#else
    (void)shot;
#endif
}
