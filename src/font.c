/* Text fonts, drawn into the game's text windows by src/text.c. */
#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_GLYPHS = 512, ROW_ABOVE_ART = 2, LETTER_GAP = 1 };

typedef struct FontEntry {
    uint32 codepoint;
    Glyph glyph;
} FontEntry;

static FontEntry g_glyphs[MAX_GLYPHS];
static int g_glyph_count;

bool FontLoad(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[font] cannot open %s\n", path);
        return false;
    }
    g_glyph_count = 0;
    char line[128];
    FontEntry *entry = NULL;
    int row = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' && (line[1] == '#' || line[1] == '.' || line[1] == '\n' || line[1] == '\r') && !entry)
            continue;
        if (strncmp(line, "U+", 2) == 0) {
            if (g_glyph_count == MAX_GLYPHS)
                break;
            entry = &g_glyphs[g_glyph_count++];
            memset(entry, 0, sizeof(*entry));
            entry->codepoint = (uint32)strtoul(line + 2, NULL, 16);
            row = 0;
        } else if (entry && (line[0] == '#' || line[0] == '.') && row + ROW_ABOVE_ART < GLYPH_ROWS) {
            int width = 0;
            uint16 bits = 0;
            for (; line[width] == '#' || line[width] == '.'; width++) {
                if (line[width] == '#')
                    bits |= (uint16)(0x8000 >> width);
            }
            entry->glyph.rows[row + ROW_ABOVE_ART] = bits;
            if (width > entry->glyph.width)
                entry->glyph.width = (uint8)width;
            entry->glyph.advance = (uint8)(entry->glyph.width + LETTER_GAP);
            row++;
        } else {
            entry = NULL;
        }
    }
    fclose(f);
    fprintf(stderr, "[font] %d glyphs from %s\n", g_glyph_count, path);
    return g_glyph_count > 0;
}

/* ---- a language's own font ------------------------------------------------- */

static FontEntry *g_language;
static int g_language_count;
static char g_language_path[1024];

static int CompareEntries(const void *a, const void *b) {
    uint32 x = ((const FontEntry *)a)->codepoint, y = ((const FontEntry *)b)->codepoint;
    return x < y ? -1 : x > y;
}

/* A BDF's glyphs sit on its baseline: each one's box says where, so rows are placed down
 * from the font's ascent and bits across from the box's left. */
bool FontLoadLanguage(const char *path) {
    if (path && strcmp(path, g_language_path) == 0 && g_language)
        return true;
    free(g_language);
    g_language = NULL;
    g_language_count = 0;
    g_language_path[0] = 0;
    if (!path)
        return true;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[font] cannot open %s\n", path);
        return false;
    }
    int capacity = 0, ascent = GLYPH_ROWS, row = -1, top = 0, left = 0;
    char line[256];
    FontEntry *entry = NULL;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "FONT_ASCENT ", 12) == 0) {
            ascent = atoi(line + 12);
        } else if (strncmp(line, "ENCODING ", 9) == 0) {
            long code = atol(line + 9);
            entry = NULL;
            if (code < 0)
                continue;
            if (g_language_count == capacity) {
                capacity = capacity ? capacity * 2 : 4096;
                FontEntry *grown = (FontEntry *)realloc(g_language, sizeof(FontEntry) * (size_t)capacity);
                if (!grown)
                    break;
                g_language = grown;
            }
            entry = &g_language[g_language_count++];
            memset(entry, 0, sizeof(*entry));
            entry->codepoint = (uint32)code;
        } else if (entry && strncmp(line, "DWIDTH ", 7) == 0) {
            int advance = atoi(line + 7);
            entry->glyph.advance = (uint8)advance;
            entry->glyph.width = (uint8)advance;
        } else if (entry && strncmp(line, "BBX ", 4) == 0) {
            int w, h, x, y;
            if (sscanf(line + 4, "%d %d %d %d", &w, &h, &x, &y) == 4) {
                top = ascent - (y + h);
                left = x;
            }
        } else if (entry && strncmp(line, "BITMAP", 6) == 0) {
            row = 0;
        } else if (strncmp(line, "ENDCHAR", 7) == 0) {
            row = -1;
            entry = NULL;
        } else if (entry && row >= 0) {
            size_t digits = strspn(line, "0123456789abcdefABCDEF");
            unsigned long bits = strtoul(line, NULL, 16);
            int y = top + row++;
            if (digits && digits <= 4 && y >= 0 && y < GLYPH_ROWS && left >= 0)
                entry->glyph.rows[y] = (uint16)((bits << (16 - digits * 4)) >> left);
        }
    }
    fclose(f);
    qsort(g_language, (size_t)g_language_count, sizeof(FontEntry), CompareEntries);
    snprintf(g_language_path, sizeof(g_language_path), "%s", path);
    fprintf(stderr, "[font] %d glyphs from %s\n", g_language_count, path);
    return g_language_count > 0;
}

bool FontHasLanguage(void) { return g_language_count > 0; }

const Glyph *FontGlyph(uint32 codepoint) {
    FontEntry key = {.codepoint = codepoint};
    const FontEntry *own = g_language_count
        ? (const FontEntry *)bsearch(&key, g_language, (size_t)g_language_count, sizeof(FontEntry), CompareEntries)
        : NULL;
    if (own)
        return &own->glyph;
    for (int i = 0; i < g_glyph_count; i++) {
        if (g_glyphs[i].codepoint == codepoint)
            return &g_glyphs[i].glyph;
    }
    return NULL;
}

int FontAdvance(const Glyph *glyph) { return glyph->advance; }

/* ---- Japanese ---------------------------------------------------------------- */

enum { JIS_ROWS = 94, JIS_BYTES = 24, JIS_FIRST = 0x21 };

static uint8 (*g_jis)[JIS_ROWS][JIS_BYTES];
static uint8 *g_jis_present;
static uint8 g_half[256][JIS_BYTES];
static uint8 g_half_present[256];

/* Calls store(encoding, bitmap) for each character of a BDF font with 12 rows. */
static bool ReadBdf(const char *path, int code_offset, void (*store)(int encoding, const uint8 *bitmap)) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[font] cannot open %s\n", path);
        return false;
    }
    char line[128];
    int encoding = -1, row = -1;
    uint8 bitmap[JIS_BYTES];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "ENCODING ", 9) == 0) {
            encoding = atoi(line + 9) + code_offset;
        } else if (strncmp(line, "BITMAP", 6) == 0) {
            row = 0;
            memset(bitmap, 0, sizeof(bitmap));
        } else if (strncmp(line, "ENDCHAR", 7) == 0) {
            if (encoding >= 0)
                store(encoding, bitmap);
            row = -1;
        } else if (row >= 0 && row < GLYPH_ROWS) {
            unsigned long bits = strtoul(line, NULL, 16);
            size_t digits = strspn(line, "0123456789abcdefABCDEF");
            if (digits <= 2) {
                bitmap[row * 2] = (uint8)bits;
            } else {
                bitmap[row * 2] = (uint8)(bits >> 8);
                bitmap[row * 2 + 1] = (uint8)bits;
            }
            row++;
        }
    }
    fclose(f);
    return true;
}

static void StoreFullWidth(int encoding, const uint8 *bitmap) {
    int row = (encoding >> 8) - JIS_FIRST, cell = (encoding & 0xFF) - JIS_FIRST;
    if (row >= 0 && row < JIS_ROWS && cell >= 0 && cell < JIS_ROWS) {
        memcpy(g_jis[row][cell], bitmap, JIS_BYTES);
        g_jis_present[row * JIS_ROWS + cell] = 1;
    }
}

static void StoreHalfWidth(int encoding, const uint8 *bitmap) {
    if (encoding >= 0 && encoding < 256) {
        memcpy(g_half[encoding], bitmap, JIS_BYTES);
        g_half_present[encoding] = 1;
    }
}

bool FontJisLoad(const char *full_width_path, const char *ascii_path, const char *kana_path) {
    if (g_jis)
        return true;
    g_jis = malloc(sizeof(uint8[JIS_ROWS][JIS_ROWS][JIS_BYTES]));
    g_jis_present = calloc(JIS_ROWS * JIS_ROWS, 1);
    if (!g_jis || !g_jis_present || !ReadBdf(full_width_path, 0, StoreFullWidth)) {
        free(g_jis);
        free(g_jis_present);
        g_jis = NULL;
        g_jis_present = NULL;
        return false;
    }
    ReadBdf(ascii_path, 0, StoreHalfWidth);
    /* The kana font is encoded from $21; those are bytes $A1 and up in Shift-JIS. */
    ReadBdf(kana_path, 0x80, StoreHalfWidth);
    return true;
}

bool FontJisFullWidth(uint16 shift_jis, uint8 *bitmap) {
    if (!g_jis)
        return false;
    int lead = shift_jis >> 8, trail = shift_jis & 0xFF;
    int row = (lead < 0xA0 ? lead - 0x81 : lead - 0xC1) * 2;
    int cell;
    if (trail >= 0x9F) {
        row += 1;
        cell = trail - 0x9F;
    } else {
        cell = trail - 0x40 - (trail > 0x7F ? 1 : 0);
    }
    if (row < 0 || row >= JIS_ROWS || cell < 0 || cell >= JIS_ROWS || !g_jis_present[row * JIS_ROWS + cell])
        return false;
    memcpy(bitmap, g_jis[row][cell], JIS_BYTES);
    return true;
}

bool FontJisHalfWidth(uint8 code, uint8 *bitmap) {
    if (!g_half_present[code])
        return false;
    memcpy(bitmap, g_half[code], JIS_BYTES);
    return true;
}
