/* A title screen, which the broadcast never had: the game went straight from checking the
 * time into the story. The port draws it over the picture, holding the game where it is
 * (still waiting for its first time signal) until any key, button or click. It then stays up
 * while the game checks the time, and fades away as the story begins. The logo follows the
 * 1997 one: chunky pale-blue letters, dark outline, an italic line under it. In Japanese
 * it says what the original logo did, and Chinese says the same.
 *
 * Everything is drawn here from the port's own font, so there is no art to ship.
 */
#include "title.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsx_wram.h"
#include "desktop/sdl_compat.h"
#include "font.h"
#include "options.h"
#include "session.h"

enum {
    WIDTH = 256,
    HEIGHT = 224,
    PAD_BUTTONS = 0xFFF0,   /* the low four bits say what kind of controller it is */
    CLOCK_MINUTE_ROLLED = 0x40,
    FRAMES_HELD = 2,
    /* After the game joins, its time check fades out and the screen stays dark a moment. */
    COVER_FRAMES_AFTER_JOIN = 100,
    FADE_FRAMES = 24,
    BLINK_FRAMES = 40,
    SNOWFLAKES = 90,
    LOGO_SCALE = 3,
};

static bool g_waiting, g_covering, g_fading;
static int g_fade, g_cover;
static unsigned g_frame;

void TitleOnImageLoaded(int part, bool first) {
    (void)part;
    g_waiting = first;
    g_covering = g_fading = false;
    g_fade = g_cover = 0;
}

bool TitleIsWaiting(void) {
    return g_waiting;
}

void TitleCancel(void) {
    g_waiting = g_covering = g_fading = false;
}

static bool AnyKeyboardKey(void) {
#if SNESRECOMP_SDL3
    int count = 0;
    const bool *keys = SDL_GetKeyboardState(&count);
#else
    int count = 0;
    const Uint8 *keys = SDL_GetKeyboardState(&count);
#endif
    for (int k = 0; keys && k < count; k++) {
        if (keys[k])
            return true;
    }
    return false;
}

/* Runs after the game reads the pads. A press has to last two frames, and the game doesn't
 * see it, so it can't carry on into whatever comes next. */
void TitleOnFrame(uint8 *ram) {
    static int held_for;
    if (g_covering) {
        ram[WRAM_PAD_PRESSED] = ram[WRAM_PAD_PRESSED + 1] = 0;
        if (ram[WRAM_CLOCK_FLAGS] & CLOCK_MINUTE_ROLLED || g_cover > 0) {
            if (++g_cover >= COVER_FRAMES_AFTER_JOIN) {
                g_covering = false;
                g_fading = true;
                g_fade = FADE_FRAMES;
            }
        }
        return;
    }
    if (!g_waiting)
        return;
    bool held = (WramRead16(ram, WRAM_PAD_HELD) & PAD_BUTTONS) || AnyKeyboardKey();
    held_for = held ? held_for + 1 : 0;
    ram[WRAM_PAD_PRESSED] = ram[WRAM_PAD_PRESSED + 1] = 0;
    ram[WRAM_PAD1_PRESSED] = ram[WRAM_PAD1_PRESSED + 1] = 0;
    if (held_for >= FRAMES_HELD) {
        g_waiting = false;
        g_covering = true;
        g_cover = 0;
    }
}

/* ── drawing ─────────────────────────────────────────────────────────── */

typedef struct Rgb {
    float r, g, b;
} Rgb;

static uint8 *g_field;

static void Put(int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT)
        return;
    uint8 *p = g_field + (y * WIDTH + x) * 4;
    p[0] = (uint8)fminf(255.0f, c.b);
    p[1] = (uint8)fminf(255.0f, c.g);
    p[2] = (uint8)fminf(255.0f, c.r);
    p[3] = 255;
}

static Rgb Mix(Rgb a, Rgb b, float t) {
    return (Rgb){a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

/* A small fixed hash, so the snow and mountains look the same every time. */
static float Noise(unsigned n) {
    n = (n << 13) ^ n;
    return (float)((n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7FFFFFFFu) / 2147483648.0f;
}

static void DrawSky(void) {
    const Rgb top = {6, 10, 34}, low = {38, 52, 96};
    for (int y = 0; y < HEIGHT; y++) {
        Rgb c = Mix(top, low, (float)y / HEIGHT);
        for (int x = 0; x < WIDTH; x++)
            Put(x, y, c);
    }
    /* Two ridges of peaks, the far one paler, with snow on the upper slopes. */
    for (int layer = 0; layer < 2; layer++) {
        const Rgb rock = layer ? (Rgb){12, 16, 32} : (Rgb){28, 36, 66};
        const Rgb snow = layer ? (Rgb){170, 182, 215} : (Rgb){120, 135, 178};
        const float base = layer ? 226.0f : 210.0f;
        const int peaks = layer ? 9 : 7;
        for (int x = 0; x < WIDTH; x++) {
            /* The highest of a few triangular peaks, roughened a little. */
            float top = HEIGHT;
            for (int k = 0; k < peaks; k++) {
                float cx = (k + 0.5f) * WIDTH / peaks + 30.0f * (Noise((unsigned)(k * 31 + layer * 7)) - 0.5f);
                float height = (layer ? 26.0f : 34.0f) + 22.0f * Noise((unsigned)(k * 17 + layer * 5 + 3));
                float slope = 0.55f + 0.6f * Noise((unsigned)(k * 3 + layer * 11 + (x > cx)));
                float y = base - height + fabsf(x - cx) * slope;
                if (y < top)
                    top = y;
            }
            top += 1.5f * Noise((unsigned)(x * 13 + layer)) + 2.0f * sinf(x * 0.3f + layer);
            float snow_line = top + (layer ? 4.0f : 10.0f) + 5.0f * Noise((unsigned)(x / 3 * 5 + layer * 3));
            for (int y = (int)top; y < HEIGHT; y++)
                Put(x, y, y < snow_line && y < base - 8 ? snow : rock);
        }
    }
}

static void DrawSnow(unsigned frame) {
    for (int i = 0; i < SNOWFLAKES; i++) {
        float speed = 0.25f + 0.5f * Noise((unsigned)i * 3u + 1u);
        float drift = 6.0f * sinf(frame * 0.02f + i);
        int x = (int)(Noise((unsigned)i * 7u) * WIDTH + drift) % WIDTH;
        int y = (int)(Noise((unsigned)i * 11u) * HEIGHT + frame * speed) % HEIGHT;
        Rgb flake = {235, 240, 255};
        Put(x, y, flake);
        if (speed > 0.55f)
            Put(x + 1, y, Mix(flake, (Rgb){40, 50, 90}, 0.4f));
    }
}

/* A line of text as glyphs: Shift-JIS from the Japanese font, or else UTF-8 from the
 * language's own font and the port's Latin one. */
typedef struct Cell {
    int width, advance;
    uint16 rows[GLYPH_ROWS];
} Cell;

enum { MAX_CELLS = 64 };

static uint32 NextUtf8(const uint8 **text) {
    const uint8 *p = *text;
    uint32 c = *p++;
    int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= more == 3 ? 7 : more == 2 ? 15 : more == 1 ? 31 : 0x7F;
    for (; more && (*p & 0xC0) == 0x80; more--)
        c = c << 6 | (*p++ & 0x3Fu);
    *text = p;
    return c;
}

static int Layout(const char *text, Cell *cells) {
    const bool shift_jis = strcmp(OptionsLanguage(), "ja") == 0;
    int count = 0;
    for (const uint8 *p = (const uint8 *)text; *p && count < MAX_CELLS;) {
        Cell *cell = &cells[count];
        memset(cell, 0, sizeof(*cell));
        uint8 bitmap[24];
        if (!shift_jis) {
            const Glyph *glyph = FontGlyph(NextUtf8(&p));
            if (!glyph)
                continue;
            cell->width = glyph->width;
            cell->advance = FontAdvance(glyph);
            memcpy(cell->rows, glyph->rows, sizeof(cell->rows));
        } else if (*p >= 0x81 && (*p < 0xA0 || *p >= 0xE0) && p[1]) {
            bool ok = FontJisFullWidth((uint16)(p[0] << 8 | p[1]), bitmap);
            p += 2;
            cell->width = cell->advance = 12;
            for (int y = 0; ok && y < GLYPH_ROWS; y++)
                cell->rows[y] = (uint16)(bitmap[2 * y] << 8 | bitmap[2 * y + 1]);
        } else if (*p >= 0x80) {
            bool ok = FontJisHalfWidth(*p++, bitmap);
            cell->width = cell->advance = 6;
            for (int y = 0; ok && y < GLYPH_ROWS; y++)
                cell->rows[y] = (uint16)(bitmap[2 * y] << 8 | bitmap[2 * y + 1]);
        } else {
            const Glyph *glyph = FontGlyph(*p++);
            if (!glyph)
                continue;
            cell->width = glyph->width;
            cell->advance = FontAdvance(glyph);
            memcpy(cell->rows, glyph->rows, sizeof(cell->rows));
        }
        count++;
    }
    return count;
}

static int TextWidth(const char *text, int scale) {
    Cell cells[MAX_CELLS];
    int count = Layout(text, cells), width = 0;
    for (int i = 0; i < count; i++)
        width += cells[i].advance * scale;
    return width;
}

static bool CellPixel(const Cell *cell, int x, int y) {
    return x >= 0 && y >= 0 && x < cell->width && y < GLYPH_ROWS && (cell->rows[y] & (0x8000 >> x));
}

/* Text into a mask, for the logo's outline and shading. */
enum { MASK_W = WIDTH, MASK_H = 64 };
static uint8 g_mask[MASK_H][MASK_W];

static void MaskText(const char *text, int scale, int left, int top) {
    memset(g_mask, 0, sizeof(g_mask));
    Cell cells[MAX_CELLS];
    int count = Layout(text, cells), pen = left;
    for (int c = 0; c < count; c++) {
        for (int y = 0; y < GLYPH_ROWS * scale; y++) {
            for (int x = 0; x < cells[c].width * scale; x++) {
                if (CellPixel(&cells[c], x / scale, y / scale) && top + y < MASK_H && pen + x >= 0 && pen + x < MASK_W)
                    g_mask[top + y][pen + x] = 1;
            }
        }
        pen += cells[c].advance * scale;
    }
}

static bool MaskAt(int x, int y) {
    return x >= 0 && y >= 0 && x < MASK_W && y < MASK_H && g_mask[y][x];
}

/* Pale blue, lighter at the top with a white band, inside a dark navy outline and shadow. */
static void DrawLogo(const char *text, int y0, int scale) {
    const int top = 4;
    int width = TextWidth(text, scale);
    MaskText(text, scale, (WIDTH - width) / 2, top);
    const Rgb outline = {10, 20, 70}, shadow = {4, 6, 20};
    const Rgb light = {250, 252, 255}, mid = {150, 200, 255}, deep = {50, 110, 230};
    for (int y = 0; y < MASK_H; y++) {
        for (int x = 0; x < MASK_W; x++) {
            if (MaskAt(x, y))
                continue;
            if (MaskAt(x - 3, y - 3) || MaskAt(x - 2, y - 3) || MaskAt(x - 3, y - 2))
                Put(x, y0 + y, shadow);
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    if (dx * dx + dy * dy <= 5 && MaskAt(x + dx, y + dy)) {
                        Put(x, y0 + y, outline);
                        dy = dx = 3;
                    }
                }
            }
        }
    }
    /* The fill runs over the letters' own height, wherever in the mask they sit. */
    int first = MASK_H, last = 0;
    for (int y = 0; y < MASK_H; y++) {
        for (int x = 0; x < MASK_W; x++) {
            if (MaskAt(x, y)) {
                first = y < first ? y : first;
                last = y;
                break;
            }
        }
    }
    for (int y = first; y <= last; y++) {
        float t = (float)(y - first) / (float)(last - first + 1);
        Rgb fill = t < 0.18f ? light : t < 0.55f ? Mix(light, mid, (t - 0.18f) / 0.37f) : Mix(mid, deep, (t - 0.55f) / 0.45f);
        for (int x = 0; x < MASK_W; x++) {
            if (!MaskAt(x, y))
                continue;
            /* A thin dark line inside each letter's right and bottom edge gives it depth. */
            Rgb c = !MaskAt(x + 1, y) || !MaskAt(x, y + 1) ? Mix(fill, deep, 0.6f) : fill;
            Put(x, y0 + y, c);
        }
    }
}

/* Text at `pen`, leaning right by one pixel every four rows if asked, with a soft shadow. */
static void DrawTextAt(const char *text, int pen, int y0, Rgb colour, bool lean) {
    Cell cells[MAX_CELLS];
    int count = Layout(text, cells);
    for (int c = 0; c < count; c++) {
        for (int y = 0; y < GLYPH_ROWS; y++) {
            int shift = lean ? (GLYPH_ROWS - y) / 4 : 0;
            for (int x = 0; x < cells[c].width; x++) {
                if (!CellPixel(&cells[c], x, y))
                    continue;
                Put(pen + x + shift + 1, y0 + y + 1, (Rgb){8, 10, 30});
                Put(pen + x + shift, y0 + y, colour);
            }
        }
        pen += cells[c].advance;
    }
}

/* Text with a dark outline all round, to read over the snowy ridges. */
static void DrawOutlined(const char *text, int y0, Rgb colour) {
    Cell cells[MAX_CELLS];
    int count = Layout(text, cells), pen = (WIDTH - TextWidth(text, 1)) / 2;
    const Rgb edge = {8, 12, 34};
    for (int pass = 0; pass < 2; pass++) {
        int x0 = pen;
        for (int c = 0; c < count; c++) {
            for (int y = 0; y < GLYPH_ROWS; y++) {
                for (int x = 0; x < cells[c].width; x++) {
                    if (!CellPixel(&cells[c], x, y))
                        continue;
                    if (pass == 1) {
                        Put(x0 + x, y0 + y, colour);
                        continue;
                    }
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++)
                            Put(x0 + x + dx, y0 + y + dy, edge);
                    }
                }
            }
            x0 += cells[c].advance;
        }
    }
}

static void DrawText(const char *text, int y0, Rgb colour) {
    DrawTextAt(text, (WIDTH - TextWidth(text, 1)) / 2, y0, colour, false);
}

static void DrawItalic(const char *text, int y0, Rgb colour) {
    DrawTextAt(text, (WIDTH - TextWidth(text, 1)) / 2, y0, colour, true);
}

/* The font has no copyright sign. */
static void DrawCopyright(int x, int y, Rgb colour) {
    static const char *const kSign[7] = {".####.", "#....#", "#.##.#", "#.#..#", "#.##.#", "#....#", ".####."};
    for (int j = 0; j < 7; j++) {
        for (int i = 0; kSign[j][i]; i++) {
            if (kSign[j][i] == '#')
                Put(x + i, y + j, colour);
        }
    }
}


/* FileStatus in Chinese, in UTF-8. */
static void ZhFileStatus(const SessionFiles *files, int found, int missing, const char *const *parts,
                         char *first, char *second, size_t size) {
    if (strcmp(OptionsAudioSource(), "rom") == 0) {
        /* 声音：游戏自带的音效, 可在启动器的Mods页更改 */
        snprintf(first, size, "\xe5\xa3\xb0\xe9\x9f\xb3\xef\xbc\x9a\xe6\xb8\xb8\xe6\x88\x8f\xe8\x87\xaa"
                              "\xe5\xb8\xa6\xe7\x9a\x84\xe9\x9f\xb3\xe6\x95\x88");
        snprintf(second, size, "\xe5\x8f\xaf\xe5\x9c\xa8\xe5\x90\xaf\xe5\x8a\xa8\xe5\x99\xa8\xe7\x9a\x84"
                               "Mods\xe9\xa1\xb5\xe6\x9b\xb4\xe6\x94\xb9");
    } else if (missing == 0) {
        /* 三篇的广播音频齐全 */
        snprintf(first, size, "\xe4\xb8\x89\xe7\xaf\x87\xe7\x9a\x84\xe5\xb9\xbf\xe6\x92\xad\xe9\x9f\xb3"
                              "\xe9\xa2\x91\xe9\xbd\x90\xe5\x85\xa8");
    } else if (found == 0) {
        /* 找不到录像, 只有游戏自带的声音 */
        snprintf(first, size, "\xe6\x89\xbe\xe4\xb8\x8d\xe5\x88\xb0\xe5\xbd\x95\xe5\x83\x8f");
        snprintf(second, size, "\xe5\x8f\xaa\xe6\x9c\x89\xe6\xb8\xb8\xe6\x88\x8f\xe8\x87\xaa\xe5\xb8\xa6"
                               "\xe7\x9a\x84\xe5\xa3\xb0\xe9\x9f\xb3");
    } else {
        /* 没有<parts>的录像, 该篇只有游戏自带的声音 */
        snprintf(first, size, "\xe6\xb2\xa1\xe6\x9c\x89");
        int listed = 0;
        for (int part = 0; part < PART_COUNT; part++) {
            if (files->recording[part])
                continue;
            snprintf(first + strlen(first), size - strlen(first), "%s%s", listed ? "\xe3\x80\x81" : "", parts[part]);
            listed++;
        }
        snprintf(first + strlen(first), size - strlen(first), "\xe7\x9a\x84\xe5\xbd\x95\xe5\x83\x8f");
        snprintf(second, size, "\xe8\xaf\xa5\xe7\xaf\x87\xe5\x8f\xaa\xe6\x9c\x89\xe6\xb8\xb8\xe6\x88\x8f"
                               "\xe8\x87\xaa\xe5\xb8\xa6\xe7\x9a\x84\xe5\xa3\xb0\xe9\x9f\xb3");
    }
}

/* Two lines under the prompt: which recordings were found, and what that means. */
static void FileStatus(char *first, char *second, size_t size) {
    static const char *const kJaParts[PART_COUNT] = {"\x91\x4f\x95\xd2", "\x92\x86\x95\xd2", "\x8c\xe3\x95\xd2"};
    /* 前篇, 中篇, 后篇 */
    static const char *const kZhParts[PART_COUNT] = {"\xe5\x89\x8d\xe7\xaf\x87", "\xe4\xb8\xad\xe7\xaf\x87",
                                                     "\xe5\x90\x8e\xe7\xaf\x87"};
    const bool ja = strcmp(OptionsLanguage(), "ja") == 0;
    const bool zh = strncmp(OptionsLanguage(), "zh", 2) == 0;
    SessionFiles files;
    SessionCheckFiles(&files);
    int found = 0, missing = 0;
    for (int part = 0; part < PART_COUNT; part++)
        found += files.recording[part];
    missing = PART_COUNT - found;
    first[0] = second[0] = 0;
    if (zh) {
        ZhFileStatus(&files, found, missing, kZhParts, first, second, size);
    } else if (strcmp(OptionsAudioSource(), "rom") == 0) {
        /* 音声：ゲーム本体の音, ランチャーのModsで変更できます */
        snprintf(first, size, "%s", ja ? "\x89\xb9\x90\xba\x81\x46\x83\x51\x81\x5b\x83\x80\x96\x7b\x91\xcc\x82\xcc\x89\xb9"
                                       : "Sound: the game's own, not the recordings");
        snprintf(second, size, "%s", ja ? "\x83\x89\x83\x93\x83\x60\x83\x83\x81\x5b\x82\xcc" "Mods"
                                          "\x82\xc5\x95\xcf\x8d\x58\x82\xc5\x82\xab\x82\xdc\x82\xb7"
                                        : "Change it on the launcher's Mods page");
    } else if (missing == 0) {
        /* 全３話の放送音声あり */
        snprintf(first, size, "%s", ja ? "\x91\x53\x82\x52\x98\x62\x82\xcc\x95\xfa\x91\x97\x89\xb9\x90\xba\x82\xa0\x82\xe8"
                                       : "Broadcast audio for all three parts");
    } else if (found == 0) {
        /* 録画が見つかりません, ゲーム本体の音のみ */
        snprintf(first, size, "%s", ja ? "\x98\x5e\x89\xe6\x82\xaa\x8c\xa9\x82\xc2\x82\xa9\x82\xe8\x82\xdc\x82\xb9\x82\xf1"
                                       : "No recordings found");
        snprintf(second, size, "%s", ja ? "\x83\x51\x81\x5b\x83\x80\x96\x7b\x91\xcc\x82\xcc\x89\xb9\x82\xcc\x82\xdd"
                                        : "The game's own sounds only");
    } else {
        /* <parts>の録画がありません, その回はゲーム本体の音のみ */
        int listed = 0;
        for (int part = 0; part < PART_COUNT; part++) {
            if (files.recording[part])
                continue;
            const char *joint = listed == 0 ? "" : ja ? "\x81\x45" : listed == missing - 1 ? " or " : ", ";
            snprintf(first + strlen(first), size - strlen(first), "%s%s", joint,
                     ja ? kJaParts[part] : SessionPartName(part));
            listed++;
        }
        char parts[64];
        snprintf(parts, sizeof(parts), "%s", first);
        if (ja)
            snprintf(first, size, "%s\x82\xcc\x98\x5e\x89\xe6\x82\xaa\x82\xa0\x82\xe8\x82\xdc\x82\xb9\x82\xf1", parts);
        else
            snprintf(first, size, "No recording for %s", parts);
        snprintf(second, size, "%s", ja ? "\x82\xbb\x82\xcc\x89\xf1\x82\xcd\x83\x51\x81\x5b\x83\x80\x96\x7b\x91\xcc\x82\xcc\x89\xb9\x82\xcc\x82\xdd"
                                        : missing == 1 ? "That part uses the game's own sounds"
                                                       : "Those parts use the game's own sounds");
    }
}

/* SnesDesktopHostGame.publish_native_field: the title, over whatever the game drew. */
void TitlePublishField(uint8_t *field, unsigned number) {
    (void)number;
    if (!g_waiting && !g_covering && !g_fading)
        return;
    g_field = field;
    g_frame++;
    DrawSky();
    DrawSnow(g_frame);
    const Rgb pale = {230, 236, 255}, gold = {255, 230, 120};
    if (strcmp(OptionsLanguage(), "ja") == 0) {
        /* BS探偵倶楽部, 〜雪に消えた過去〜, なにかキーを押してください */
        DrawLogo("BS\x92\x54\x92\xe3\x8b\xe4\x8a\x79\x95\x94", 44, LOGO_SCALE);
        DrawItalic("\x81\x60\x90\xe1\x82\xc9\x8f\xc1\x82\xa6\x82\xbd\x89\xdf\x8b\x8e\x81\x60", 106, pale);
        if (g_waiting && g_frame / BLINK_FRAMES % 2 == 0)
            DrawText("\x82\xc8\x82\xc9\x82\xa9\x83\x4c\x81\x5b\x82\xf0\x89\x9f\x82\xb5\x82\xc4"
                     "\x82\xad\x82\xbe\x82\xb3\x82\xa2", 150, gold);
    } else if (strncmp(OptionsLanguage(), "zh", 2) == 0) {
        /* BS侦探俱乐部, ～消失在雪中的过去～, 请按任意键 */
        DrawLogo("BS\xe4\xbe\xa6\xe6\x8e\xa2\xe4\xbf\xb1\xe4\xb9\x90\xe9\x83\xa8", 44, LOGO_SCALE);
        DrawItalic("\xef\xbd\x9e\xe6\xb6\x88\xe5\xa4\xb1\xe5\x9c\xa8\xe9\x9b\xaa\xe4\xb8\xad\xe7\x9a\x84"
                   "\xe8\xbf\x87\xe5\x8e\xbb\xef\xbd\x9e", 106, pale);
        if (g_waiting && g_frame / BLINK_FRAMES % 2 == 0)
            DrawText("\xe8\xaf\xb7\xe6\x8c\x89\xe4\xbb\xbb\xe6\x84\x8f\xe9\x94\xae", 150, gold);
    } else {
        DrawLogo("BS", 30, LOGO_SCALE);
        DrawLogo("Detective Club", 66, LOGO_SCALE);
        DrawItalic("~ The Past That Disappeared in the Snow ~", 112, pale);
        if (g_waiting && g_frame / BLINK_FRAMES % 2 == 0)
            DrawText("Press any key", 150, gold);
    }
    static char status[2][96];
    static bool status_known;
    if (!status_known) {
        FileStatus(status[0], status[1], sizeof(status[0]));
        status_known = true;
    }
    const Rgb muted = {200, 208, 232};
    DrawOutlined(status[0], 172, muted);
    DrawOutlined(status[1], 186, muted);
    const char *line = "1988, 1989, 1997 Nintendo";
    int left = (WIDTH - (TextWidth(line, 1) + 9)) / 2;
    DrawCopyright(left, 211 + 2, (Rgb){200, 205, 225});
    DrawTextAt(line, left + 9, 211, (Rgb){200, 205, 225}, false);
    if (g_fading) {
        /* Fade the title to black; the game's own fade-in follows. */
        float keep = (float)g_fade / FADE_FRAMES;
        for (int i = 0; i < WIDTH * HEIGHT; i++) {
            for (int c = 0; c < 3; c++)
                field[i * 4 + c] = (uint8)(field[i * 4 + c] * keep);
        }
        if (--g_fade <= 0)
            g_fading = false;
    }
}

/* The logo and the line under it, for the game's own title card (src/art.c): drawn into a
 * cleared 256x224 field, centred across it, letters at the top. */
void TitleDrawCard(uint8_t *field, const char *logo, const char *subtitle) {
    g_field = field;
    const int scale = 2;
    DrawLogo(logo, -2, scale);
    DrawItalic(subtitle, GLYPH_ROWS * scale + 3, (Rgb){230, 236, 255});
}
