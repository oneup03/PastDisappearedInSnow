#pragma once
#include <stdbool.h>

#include "types.h"

enum { GLYPH_ROWS = 12 };

/* One glyph, left-aligned: the top bit of each row is the leftmost pixel. */
typedef struct Glyph {
    uint8 width;
    uint16 rows[GLYPH_ROWS];
} Glyph;

/* Load a text font (see assets/fonts/latin.txt). */
bool FontLoad(const char *path);

/* The glyph for a Unicode code point, or NULL if the font lacks it. */
const Glyph *FontGlyph(uint32 codepoint);

/* Pixels a glyph takes up, with its gap. */
int FontAdvance(const Glyph *glyph);

/* Japanese, from the Shinonome font (tools/fetch_font.py): 24 bytes for one character,
 * two per row. Returns false if that font or character isn't there. */
bool FontJisLoad(const char *full_width_path, const char *ascii_path, const char *kana_path);
bool FontJisFullWidth(uint16 shift_jis, uint8 *bitmap);
bool FontJisHalfWidth(uint8 code, uint8 *bitmap);
