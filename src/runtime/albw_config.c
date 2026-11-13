#include "albw_config.h"

#include "albw_cpu.h"
#include "albw_kernel.h"
#include "albw_romfs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// Region and language.

enum { REGION_JPN = 0, REGION_USA = 1, REGION_EUR = 2, REGION_CHN = 4, REGION_KOR = 5,
       REGION_TWN = 6 };

// Languages as the console numbers them.
enum { LANG_JA = 0, LANG_EN, LANG_FR, LANG_DE, LANG_IT, LANG_ES, LANG_ZH_CN, LANG_KO, LANG_NL,
       LANG_PT, LANG_RU, LANG_ZH_TW };

static const struct { const char *code; uint32_t language; } language_codes[] = {
    { "ja", LANG_JA }, { "en", LANG_EN }, { "fr", LANG_FR }, { "de", LANG_DE },
    { "it", LANG_IT }, { "es", LANG_ES }, { "zh_CN", LANG_ZH_CN }, { "ko", LANG_KO },
    { "nl", LANG_NL }, { "pt", LANG_PT }, { "ru", LANG_RU }, { "zh_TW", LANG_ZH_TW },
    { "zh", LANG_ZH_CN },
};

// Which languages a console of each region offers. A card is made for a region, and a language the
// region does not have is one the card may have no text for.
static int region_has(uint32_t region, uint32_t language) {
    switch (region) {
        case REGION_JPN: return language == LANG_JA;
        case REGION_USA: return language == LANG_EN || language == LANG_FR
                             || language == LANG_ES || language == LANG_PT;
        case REGION_EUR: return language == LANG_EN || language == LANG_FR
                             || language == LANG_DE || language == LANG_IT
                             || language == LANG_ES || language == LANG_NL
                             || language == LANG_PT || language == LANG_RU;
        case REGION_CHN: return language == LANG_ZH_CN;
        case REGION_KOR: return language == LANG_KO;
        case REGION_TWN: return language == LANG_ZH_TW;
        default: return 0;
    }
}

static uint32_t region_default_language(uint32_t region) {
    switch (region) {
        case REGION_JPN: return LANG_JA;
        case REGION_CHN: return LANG_ZH_CN;
        case REGION_KOR: return LANG_KO;
        case REGION_TWN: return LANG_ZH_TW;
        default: return LANG_EN;
    }
}

static int ready;
static uint32_t region = REGION_USA;
static uint32_t language = LANG_EN;

// The region is the card's: the icon's region lockout mask, one bit per region in the order above
// with Australia, which the console folds into Europe, at bit 3.
static uint32_t region_from_card(void) {
    uint32_t size = 0;
    const uint8_t *icon = albw_exefs_file("icon", &size);
    if (!icon || size < 0x2020 || memcmp(icon, "SMDH", 4)) {
        printf("[cfg  ] no icon on the card to read the region from; assuming the Americas\n");
        return REGION_USA;
    }
    uint32_t mask;
    memcpy(&mask, icon + 0x2018, 4);
    static const uint32_t order[] = { REGION_JPN, REGION_USA, REGION_EUR, REGION_EUR,
                                      REGION_CHN, REGION_KOR, REGION_TWN };
    for (uint32_t bit = 0; bit < 7; bit++) {
        if (mask & (1u << bit)) return order[bit];
    }
    return REGION_USA;                                   // region-free: any will do
}

// A language from a locale string or a code: "de_DE.UTF-8", "de", "zh_TW", or the number itself.
static int language_from(const char *s, uint32_t *out) {
    if (!s || !*s) return 0;
    if (isdigit((unsigned char)s[0])) {
        unsigned long n = strtoul(s, NULL, 10);
        if (n > LANG_ZH_TW) return 0;
        *out = (uint32_t)n;
        return 1;
    }
    // Longest match first, so zh_TW is not taken for zh.
    for (size_t i = 0; i < sizeof language_codes / sizeof language_codes[0]; i++) {
        size_t n = strlen(language_codes[i].code);
        if (n > 2 && !strncmp(s, language_codes[i].code, n)) {
            *out = language_codes[i].language;
            return 1;
        }
    }
    for (size_t i = 0; i < sizeof language_codes / sizeof language_codes[0]; i++) {
        size_t n = strlen(language_codes[i].code);
        if (n == 2 && !strncmp(s, language_codes[i].code, 2)
                && (s[2] == '\0' || s[2] == '_' || s[2] == '.' || s[2] == '@')) {
            *out = language_codes[i].language;
            return 1;
        }
    }
    return 0;
}

static void setup(void) {
    if (ready) return;
    ready = 1;
    region = region_from_card();
    language = region_default_language(region);

    // An explicit choice wins, then the desktop's language, then the region's own default -- and
    // whichever it is has to be one the region offers.
    const char *sources[] = { getenv("ALBW_LANGUAGE"), getenv("LC_ALL"), getenv("LC_MESSAGES"),
                              getenv("LANG") };
    for (size_t i = 0; i < sizeof sources / sizeof sources[0]; i++) {
        uint32_t wanted;
        if (!language_from(sources[i], &wanted)) continue;
        if (region_has(region, wanted)) {
            language = wanted;
            break;
        }
        if (i == 0) {
            printf("[cfg  ] ALBW_LANGUAGE asks for a language this card's region does not offer; "
                   "keeping %u\n", language);
        }
    }
    printf("[cfg  ] region %u, language %u\n", region, language);
}

uint32_t albw_config_region(void) {
    setup();
    return region;
}

// ---------------------------------------------------------------------------------------------
// The blocks.

static void put_bytes(uint32_t dest, uint32_t size, const void *data, uint32_t len) {
    const uint8_t *p = data;
    for (uint32_t i = 0; i < size; i++) albw_w8(dest + i, i < len ? p[i] : 0);
}

// The 3D camera parameters a console ships with: the distance between the viewer's eyes, the
// distance to the screen, the screen's width and height, then the limits the stereo depth is held to.
// The width and height are not tuning: the top screen is 400 by 240 pixels at 0.192 mm each, which is
// 76.8 by 46.08 mm.
static const float stereo_camera[8] = {
    62.0f, 289.0f, 76.80f, 46.08f, 10.0f, 5.0f, 55.58f, 21.57f,
};

// Country codes, as the console numbers them, for the one country each region falls back to.
static uint8_t region_country(void) {
    switch (region) {
        case REGION_JPN: return 1;                       // Japan
        case REGION_EUR: return 110;                     // the United Kingdom
        default: return 49;                              // the United States
    }
}

// The name the console was given, which the game offers as the player's. ALBW_USERNAME if set,
// otherwise the login name, kept to the ten characters and the plain letters the block can carry.
void albw_config_username(char out[11]) {
    const char *name = getenv("ALBW_USERNAME");
    if (!name || !*name) name = getenv("USER");
    if (!name || !*name) name = "Link";
    size_t w = 0;
    for (const char *p = name; *p && w < 10; p++) {
        unsigned char ch = (unsigned char)*p;
        if (isalnum(ch) || ch == ' ') out[w++] = (char)ch;
    }
    if (!w) {
        strcpy(out, "Link");
        return;
    }
    out[w] = '\0';
    if (!getenv("ALBW_USERNAME") && islower((unsigned char)out[0])) {
        out[0] = (char)toupper((unsigned char)out[0]);
    }
}

uint32_t albw_config_block(uint32_t block, uint32_t size, uint32_t dest) {
    setup();
    if (!albw_guest_ok(dest, size)) return 0xD8A103F9u;  // the buffer is not the guest's

    switch (block) {
        case 0x00050005:                                 // stereo camera
            put_bytes(dest, size, stereo_camera, sizeof stereo_camera);
            return 0;
        case 0x00070001: {                               // sound output: stereo
            uint8_t mode = 1;
            put_bytes(dest, size, &mode, 1);
            return 0;
        }
        case 0x000A0000: {                               // the user's name, UTF-16, then two words
            char name[11];
            albw_config_username(name);
            uint8_t raw[0x1C] = { 0 };
            for (size_t i = 0; name[i]; i++) raw[i * 2] = (uint8_t)name[i];
            put_bytes(dest, size, raw, sizeof raw);
            return 0;
        }
        case 0x000A0002: {
            uint8_t lang = (uint8_t)language;
            put_bytes(dest, size, &lang, 1);
            return 0;
        }
        case 0x000B0000: {                               // two unknown bytes, state, country
            uint8_t info[4] = { 0, 0, 0, region_country() };
            put_bytes(dest, size, info, sizeof info);
            return 0;
        }
        case 0x000D0000: {
            // The EULA version agreed to, minor then major. The value a console reports once its
            // agreements are accepted; zero means none have been, which a game may treat as a
            // reason to keep its network features off -- harmless here, but not the truth.
            uint8_t eula[4] = { 0x7F, 0x7F, 0, 0 };
            put_bytes(dest, size, eula, sizeof eula);
            return 0;
        }
        case 0x000B0001:                                 // country name, sixteen languages
        case 0x000B0002:                                 // state name, likewise
        case 0x000C0000:                                 // parental controls: none set up
            put_bytes(dest, size, NULL, 0);
            return 0;
        default:
            // Blocks nobody here has identified. Zeros, which is what a console that never wrote
            // the block would hold, and a line in the log, so that the next black screen that turns
            // out to be a missing setting is found by reading the log rather than by a backtrace.
            printf("[cfg  ] block %08x (%u bytes) is not one this knows; answering zeros\n",
                   block, size);
            put_bytes(dest, size, NULL, 0);
            return 0;
    }
}
