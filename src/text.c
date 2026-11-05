/* The text path.
 *
 * Saves must keep working when the language or a translation changes, so nothing in
 * a script moves. When a part's image loads, each stretch of translated text in the
 * in-memory script becomes the game's unused opcode $0D followed by single-byte
 * spaces, which its pre-scan steps over like any text. Waits, speed changes and
 * speaker changes stay put at their own addresses, so voice sync is untouched.
 *
 * At runtime $0D lands in HleTextOpcode, which finds the string by the opcode's
 * address and draws it over the same number of ticks the Japanese took. Its progress
 * lives in WRAM, so save states and rewind can land in the middle of a line.
 *
 * Menu labels are short fixed fields, translated in place in half-width text.
 * Speaker names are copied to WRAM by the script, so there the string itself carries
 * the opcode and an index into the names table.
 */
#include "text.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "bsx_wram.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "font.h"

void BsxTraceTextShot(const char *name);  /* src/bsx_hle.c */
#include "host_paths.h"
#include "script.h"
#include "session.h"

/* Window struct, as set up by $C0:F53E. */
enum {
    WIN_BASE = 0x3400,
    WIN_SIZE = 0x80,
    WIN_COUNT = 4,
    WIN_PC = 0x04,
    WIN_BANK = 0x06,
    WIN_RETURN_PC = 0x08,
    WIN_ID = 0x10,
    WIN_PENDING = 0x14,
    WIN_DRAWN = 0x16,
    WIN_COLOR = 0x18,
    WIN_COL = 0x1E,
    WIN_ROW = 0x20,
    WIN_BASE_COL = 0x24,
    WIN_BASE_ROW = 0x26,
    WIN_CELL = 0x2E,
    WIN_COLS = 0x32,
    WIN_ROWS = 0x34,
    WIN_TYPING_SOUND = 0x42,
};

enum {
    WRAM_TILE_BASES = 0x4096,  /* words, by window id - 1; doubled, an offset into $7F */
    WRAM_DIRTY_ROWS = 0x4191,
    WRAM_SOUND_QUEUE = 0x4139,
    WRAM_BANK_7F = 0x10000,
    TILE_COLUMN_BYTES = 0x20,
    TEXT_ROW_BYTES = 0x400,
    CELL_PIXELS = 6,
    NAME_MARGIN_PX = 12,  /* the game's names start with a full-width space */
    NAME_BANK = 0x7E,
    NO_RUN = 0xFFFF,
    OP_TABLE = 0xC0FA42,  /* the interpreter's own opcodes; the same in all three packs */
    OP_STUB = 0xC0FE00,   /* unused in all three packs */
};

/* A translated stretch of a script. */
typedef struct TextRun {
    uint32 start, end;
    const char *text;
    uint16 ticks;
    uint16 glyph_count;
} TextRun;

/* Progress in one window, kept in guest memory. */
typedef struct RunProgress {
    uint16 run;      /* low word of the run's address, or a name's index */
    uint16 bank;
    uint16 offset;   /* bytes of the string drawn */
    uint16 tick;
    uint16 glyphs;   /* glyphs of the string drawn */
    uint16 px;       /* pixels across the current text row */
    uint16 row;
    uint16 margin;   /* where wrapped rows of this message start */
    uint16 wrapped;  /* nonzero if px is at a wrapped row's start */
} RunProgress;

typedef struct TranslationRow {
    uint32 addr;
    const char *text;
    bool used;
} TranslationRow;

static char g_language[32] = "ja";
static bool g_font_ready;
static char *g_file;
static TranslationRow *g_rows;
static int g_row_count;
static TextRun *g_runs;
static int g_run_count, g_run_capacity;
enum { MAX_NAMES = 255 };
static const char *g_names[MAX_NAMES];
static int g_name_count;

void TextSetLanguage(const char *language) {
    snprintf(g_language, sizeof(g_language), "%s", language);
}

static bool IsTranslating(void) { return strcmp(g_language, "ja") != 0; }

static const Glyph *GlyphFor(uint32 codepoint);
static uint32 NextCodepoint(const char **text);

/* ---- loading a language ----------------------------------------------------- */

static int CompareRows(const void *a, const void *b) {
    uint32 x = ((const TranslationRow *)a)->addr, y = ((const TranslationRow *)b)->addr;
    return x < y ? -1 : x > y;
}

static char *ReadWholeTextFile(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)size + 1);
    size_t got = text ? fread(text, 1, (size_t)size, f) : 0;
    fclose(f);
    if (text)
        text[got] = 0;
    return text;
}

/* Rows look like "zen:026EA5<tab>status<tab>text<tab>hash<tab>note". The string stays in
 * the file buffer. Rows with no text are left out, so they stay in Japanese. */
static void LoadTranslation(const char *key) {
    free(g_file);
    free(g_rows);
    g_file = NULL;
    g_rows = NULL;
    g_row_count = 0;
    char leaf[128], path[1024];
    snprintf(leaf, sizeof(leaf), "translation/%s/%s.tsv", g_language, key);
    if (!snesrecomp_exe_dir_path(leaf, path, sizeof(path)))
        snprintf(path, sizeof(path), "%s", leaf);
    g_file = ReadWholeTextFile(path);
    if (!g_file) {
        fprintf(stderr, "[text] no %s\n", path);
        return;
    }
    int capacity = 1;
    for (const char *p = g_file; *p; p++)
        capacity += *p == '\n';
    g_rows = (TranslationRow *)malloc(sizeof(TranslationRow) * (size_t)capacity);
    for (char *line = g_file; line && *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        char *colon = strchr(line, ':');
        char *status = strchr(line, '\t');
        char *text = status ? strchr(status + 1, '\t') : NULL;
        if (colon && status && text && colon < status) {
            char *after = strchr(text + 1, '\t');
            if (after)
                *after = 0;
            size_t length = strlen(text + 1);
            if (length && text[length] == '\r')
                text[length] = 0;
            if (text[1]) {
                g_rows[g_row_count].addr = (uint32)strtoul(colon + 1, NULL, 16);
                g_rows[g_row_count].text = text + 1;
                g_rows[g_row_count].used = false;
                g_row_count++;
            }
        }
        line = end ? end + 1 : NULL;
    }
    qsort(g_rows, (size_t)g_row_count, sizeof(TranslationRow), CompareRows);
    fprintf(stderr, "[text] %s: %d translated rows for %s\n", g_language, g_row_count, key);
}

static const char *TranslationFor(uint32 file_offset) {
    TranslationRow key = {file_offset, NULL, false};
    TranslationRow *row =
        (TranslationRow *)bsearch(&key, g_rows, (size_t)g_row_count, sizeof(TranslationRow), CompareRows);
    if (!row)
        return NULL;
    row->used = true;
    return row->text;
}

/* ---- applying it to the image ----------------------------------------------- */

static uint32 FileOffset(uint32 addr) { return addr & 0x0FFFFF; }

static uint32 NextCodepoint(const char **text) {
    const uint8 *p = (const uint8 *)*text;
    uint32 c = *p++;
    if (c >= 0xF0) {
        c = (c & 7) << 18 | (p[0] & 0x3Fu) << 12 | (p[1] & 0x3Fu) << 6 | (p[2] & 0x3Fu);
        p += 3;
    } else if (c >= 0xE0) {
        c = (c & 15) << 12 | (p[0] & 0x3Fu) << 6 | (p[1] & 0x3Fu);
        p += 2;
    } else if (c >= 0xC0) {
        c = (c & 31) << 6 | (p[0] & 0x3Fu);
        p += 1;
    }
    *text = (const char *)p;
    return c;
}

/* A translation's own tags, three bytes each: a line break, and a clear of the window for
 * text that needs pages the Japanese made by overwriting itself. */
static bool IsOwnTag(const char *text) { return strncmp(text, "{n}", 3) == 0 || strncmp(text, "{c}", 3) == 0; }

/* A tag that stays in the script, which each translation must repeat in order. Returns
 * its length, or 0. */
static int KeptTagLength(const char *text) {
    if (text[0] != '{' || IsOwnTag(text))
        return 0;
    const char *end = strchr(text, '}');
    return end ? (int)(end - text) + 1 : 0;
}

static void AddRun(uint32 start, uint32 end, const char *text, size_t length, int ticks) {
    if (g_run_count == g_run_capacity) {
        g_run_capacity = g_run_capacity ? g_run_capacity * 2 : 1024;
        g_runs = (TextRun *)realloc(g_runs, sizeof(TextRun) * (size_t)g_run_capacity);
    }
    char *copy = (char *)malloc(length + 1);
    memcpy(copy, text, length);
    copy[length] = 0;
    int glyphs = 0;
    for (const char *p = copy; *p;) {
        if (IsOwnTag(p)) {
            p += 3;
        } else {
            NextCodepoint(&p);
        }
        glyphs++;
    }
    g_runs[g_run_count++] = (TextRun){start, end, copy, (uint16)ticks, (uint16)glyphs};
    uint8 *bytes = RomPtr(start);
    memset(bytes, ' ', end - start);
    bytes[0] = OP_TEXT_HOOK;
}

static int CompareRuns(const void *a, const void *b) {
    uint32 x = ((const TextRun *)a)->start, y = ((const TextRun *)b)->start;
    return x < y ? -1 : x > y;
}

/* One message's contiguous stretches of text, and what separates them. */
enum { MAX_PIECES = 128, MAX_TAG_TEXT = 1024 };

typedef struct Piece {
    uint32 start, end;
    int ticks;
    int gap;  /* how many kept tags come before this */
} Piece;

typedef struct Message {
    uint32 key;
    Piece pieces[MAX_PIECES];
    int count;
    char tags[MAX_TAG_TEXT];
    size_t tags_length;
    int gap;
    bool in_piece;
} Message;

static void AppendTag(Message *m, const char *tag) {
    size_t length = strlen(tag);
    if (m->tags_length + length + 1 < MAX_TAG_TEXT) {
        memcpy(m->tags + m->tags_length, tag, length + 1);
        m->tags_length += length;
    }
    m->gap++;
    m->in_piece = false;
}

static void ApplyMessage(Message *m) {
    if (!m->key)
        return;
    const char *text = TranslationFor(FileOffset(m->key));
    if (!text)
        return;
    /* The translation must keep the same tags in the same order. */
    char seen[MAX_TAG_TEXT];
    size_t seen_length = 0;
    for (const char *p = text; *p; p++) {
        int tag = KeptTagLength(p);
        if (tag && seen_length + (size_t)tag + 1 < sizeof(seen)) {
            memcpy(seen + seen_length, p, (size_t)tag);
            seen_length += (size_t)tag;
            p += tag - 1;
        }
    }
    seen[seen_length] = 0;
    m->tags[m->tags_length] = 0;
    if (strcmp(seen, m->tags) != 0) {
        fprintf(stderr, "[text] %06X keeps Japanese: tags should be %s\n", FileOffset(m->key), m->tags);
        return;
    }
    const char *chunk = text;
    int gap = 0, piece = 0;
    while (piece < m->count) {
        const char *end = chunk;
        while (*end && !KeptTagLength(end))
            end++;
        bool first_in_gap = true;
        while (piece < m->count && m->pieces[piece].gap == gap) {
            const Piece *at = &m->pieces[piece++];
            if (first_in_gap)
                AddRun(at->start, at->end, chunk, (size_t)(end - chunk), at->ticks);
            else
                AddRun(at->start, at->end, "", 0, at->ticks);
            first_in_gap = false;
        }
        if (*end == 0)
            break;
        chunk = end + KeptTagLength(end);
        gap++;
    }
    /* A translation with no text after its last tag leaves nothing to draw. */
    while (piece < m->count) {
        const Piece *at = &m->pieces[piece++];
        AddRun(at->start, at->end, "", 0, at->ticks);
    }
}

static void ResetMessage(Message *m) {
    m->key = 0;
    m->count = 0;
    m->tags_length = 0;
    m->gap = 0;
    m->in_piece = false;
}

static void AddToPiece(Message *m, const ScriptItem *item) {
    if (!m->in_piece) {
        if (m->count == MAX_PIECES)
            return;
        m->pieces[m->count++] = (Piece){item->addr, item->addr, 0, m->gap};
        m->in_piece = true;
    }
    Piece *piece = &m->pieces[m->count - 1];
    piece->end = item->next;
    piece->ticks++;
}

/* Speaker names: "<$0D> <number> <spaces> <NUL>", the same size as the original. The
 * number starts at 1 so that it can't end the string early. */
static void ApplyName(const ScriptItem *item) {
    uint32 string = item->addr + 4;
    const char *name = TranslationFor(FileOffset(string));
    uint32 length = item->next - 1 - string;
    if (!name || length < 2 || g_name_count == MAX_NAMES)
        return;
    uint8 *bytes = RomPtr(string);
    memset(bytes, ' ', length);
    bytes[0] = OP_TEXT_HOOK;
    g_names[g_name_count++] = name;
    bytes[1] = (uint8)g_name_count;
}

static void ApplyMenu(const ScriptItem *item) {
    int count = *RomPtr(item->addr + 1);
    for (int i = 0; i < count; i++) {
        uint32 label = item->addr + 2 + (uint32)i * MENU_ENTRY_SIZE + MENU_LABEL_OFFSET;
        const char *text = TranslationFor(FileOffset(label));
        if (!text)
            continue;
        uint8 *bytes = RomPtr(label);
        size_t length = strlen(text);
        if (length > MENU_LABEL_SIZE)
            length = MENU_LABEL_SIZE;
        memset(bytes, ' ', MENU_LABEL_SIZE);
        memcpy(bytes, text, length);
    }
}

static void ApplyToScene(uint32 addr) {
    static Message message;
    ResetMessage(&message);
    ScriptItem item = {.next = addr};
    do {
        ScriptStep(item.next, &item);
        if (item.op == -1) {
            if (!message.key)
                message.key = item.addr;
            AddToPiece(&message, &item);
        } else if (item.op == OP_NEWLINE) {
            if (message.key)
                AddToPiece(&message, &item);
        } else if (item.op == OP_WAIT || item.op == OP_SPEED || item.op == OP_SOUND || item.op == OP_1A) {
            if (message.key) {
                const uint8 *args = RomPtr(item.addr + 1);
                char tag[32];
                uint16 value = (uint16)(args[0] | args[1] << 8);
                if (item.op == OP_WAIT && value == WAIT_FOR_BUTTON)
                    snprintf(tag, sizeof(tag), "{w}");
                else if (item.op == OP_WAIT)
                    snprintf(tag, sizeof(tag), "{w:%u}", value);
                else if (item.op == OP_SPEED)
                    snprintf(tag, sizeof(tag), "{s:%u}", value);
                else if (item.op == OP_SOUND)
                    snprintf(tag, sizeof(tag), "{se:%u}", args[0]);
                else
                    snprintf(tag, sizeof(tag), "{x:%02x%02x%02x%02x}", args[0], args[1], args[2], args[3]);
                AppendTag(&message, tag);
            }
        } else if (item.op == OP_SPEAKER) {
            message.in_piece = false;
        } else {
            ApplyMessage(&message);
            ResetMessage(&message);
            if (item.op == OP_STRING)
                ApplyName(&item);
            else if (item.op == OP_MENU)
                ApplyMenu(&item);
        }
    } while (item.op != OP_END);
    ApplyMessage(&message);
}

void AssetPath(const char *leaf, char *path, size_t size) {
    if (!snesrecomp_exe_dir_path(leaf, path, size))
        snprintf(path, size, "%s", leaf);
}

/* Japanese is needed in every language, for anything not yet translated. */
static void LoadFonts(void) {
    if (g_font_ready)
        return;
    g_font_ready = true;
    char latin[1024], full[1024], ascii[1024], kana[1024];
    AssetPath("assets/fonts/latin.txt", latin, sizeof(latin));
    FontLoad(latin);
    AssetPath("assets/fonts/shnmk12.bdf", full, sizeof(full));
    AssetPath("assets/fonts/shnm6x12a.bdf", ascii, sizeof(ascii));
    AssetPath("assets/fonts/shnm6x12r.bdf", kana, sizeof(kana));
    if (!FontJisLoad(full, ascii, kana))
        fprintf(stderr, "[text] no Japanese font: run tools/fetch_font.py\n");
}

/* ---- labels drawn into the game's graphics ----------------------------------- */

enum {
    ICON_ROW_TILES = 16,   /* the icon sheet's tile rows */
    TILE_BYTES = 32,
    ICON_WIDTH = 31,
    LABEL_LEFT = 2,
    LABEL_TOP = 1,
    OLD_LABEL_RIGHT = 17,  /* where the game's own label ends */
    OLD_LABEL_ROWS = 11,
    TEXT_ROWS = 9,         /* a label's text, with its descenders */
    LABEL_BACK = 1,        /* colours the game's own labels use */
    LABEL_INK = 15,
    ITEM_WIDTH = 48,
    ITEM_HEIGHT = 16,
    ITEM_OLD_LABEL_LEFT = 22,
    ITEM_TEXT_LEFT = 19,
    ITEM_TEXT_TOP = 8,
    ITEM_TEXT_ROWS = 7,
    TINY_COLUMNS = 3,
    TINY_ROWS = 5,
    NAME_BOX_WIDTH = 12,
};

static uint8 *TilePixelByte(uint32 sheet, int x, int y, int plane) {
    uint32 tile = (uint32)(y / 8 * ICON_ROW_TILES + x / 8);
    return RomPtr(0xC00000 | (sheet + tile * TILE_BYTES)) + (plane >= 2 ? 16 : 0) + y % 8 * 2 + (plane & 1);
}

static void PutTilePixel(uint32 sheet, int x, int y, int color) {
    int bit = 7 - x % 8;
    for (int plane = 0; plane < 4; plane++) {
        uint8 *byte = TilePixelByte(sheet, x, y, plane);
        *byte = (uint8)((*byte & ~(1 << bit)) | ((color >> plane & 1) << bit));
    }
}

static int GetTilePixel(uint32 sheet, int x, int y) {
    int bit = 7 - x % 8, color = 0;
    for (int plane = 0; plane < 4; plane++)
        color |= (*TilePixelByte(sheet, x, y, plane) >> bit & 1) << plane;
    return color;
}

static void DrawLabel(uint32 sheet, const char *label) {
    int width = 0;
    for (const char *p = label; *p;) {
        const Glyph *glyph = GlyphFor(NextCodepoint(&p));
        width += glyph ? FontAdvance(glyph) : 0;
    }
    int right = LABEL_LEFT + width < ICON_WIDTH ? LABEL_LEFT + width : ICON_WIDTH;
    /* Cover the original label, and however far this one runs past it. */
    for (int y = LABEL_TOP; y < LABEL_TOP + OLD_LABEL_ROWS; y++) {
        for (int x = 1; x <= (y < LABEL_TOP + TEXT_ROWS && right > OLD_LABEL_RIGHT ? right : OLD_LABEL_RIGHT); x++)
            PutTilePixel(sheet, x, y, LABEL_BACK);
    }
    int pen = LABEL_LEFT;
    for (const char *p = label; *p;) {
        const Glyph *glyph = GlyphFor(NextCodepoint(&p));
        if (!glyph)
            continue;
        for (int y = 0; y < TEXT_ROWS; y++) {
            uint16 bits = glyph->rows[y + 2];
            for (int x = 0; x < glyph->width && pen + x < ICON_WIDTH; x++) {
                if (bits & (0x8000 >> x))
                    PutTilePixel(sheet, pen + x, LABEL_TOP + y, LABEL_INK);
            }
        }
        pen += FontAdvance(glyph);
    }
}

/* An item's sprite is 48 by 16, with its picture on the left and its label on the right.
 * A word in the bottom row reaches furthest left, under the picture's narrow top. */
static void DrawItemLabel(uint32 sheet, const char *label) {
    for (int y = 0; y < ITEM_HEIGHT; y++) {
        for (int x = y < ITEM_TEXT_TOP ? ITEM_OLD_LABEL_LEFT : ITEM_TEXT_LEFT; x < ITEM_WIDTH; x++)
            PutTilePixel(sheet, x, y, 0);
    }
    int pen = ITEM_TEXT_LEFT;
    for (const char *p = label; *p;) {
        const Glyph *glyph = GlyphFor(NextCodepoint(&p));
        if (!glyph)
            continue;
        for (int y = 0; y < ITEM_TEXT_ROWS; y++) {
            for (int x = 0; x < glyph->width; x++) {
                if (!(glyph->rows[y + 2] & (0x8000 >> x)) || pen + x >= ITEM_WIDTH)
                    continue;
                PutTilePixel(sheet, pen + x, ITEM_TEXT_TOP + y, LABEL_INK);
                /* The game's labels have a dark outline, to read against any scene. */
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int ox = pen + x + dx, oy = ITEM_TEXT_TOP + y + dy;
                        if (ox >= ITEM_TEXT_LEFT && ox < ITEM_WIDTH && oy < ITEM_HEIGHT && !GetTilePixel(sheet, ox, oy))
                            PutTilePixel(sheet, ox, oy, LABEL_BACK);
                    }
                }
            }
        }
        pen += FontAdvance(glyph);
    }
}

/* The people chart's cells hold names written downwards in a box 12 pixels wide. Stacking
 * short rows keeps that look, in small capitals, which are all that fit. */
static const uint8 kTinyLetters[26][TINY_ROWS] = {
    {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7}, {7, 4, 6, 4, 4},
    {3, 4, 5, 5, 3}, {5, 5, 7, 5, 5}, {7, 2, 2, 2, 7}, {1, 1, 1, 5, 2}, {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7},
    {5, 7, 7, 5, 5}, {6, 5, 5, 5, 5}, {2, 5, 5, 5, 2}, {6, 5, 6, 4, 4}, {2, 5, 5, 6, 3}, {6, 5, 6, 5, 5},
    {3, 4, 2, 1, 6}, {7, 2, 2, 2, 2}, {5, 5, 5, 5, 7}, {5, 5, 5, 2, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5},
    {5, 5, 2, 2, 2}, {7, 1, 2, 4, 7},
};

typedef struct ChartCell {
    uint32 sheet;
    int tile, width;  /* in tiles: a cell's tiles run across and then down the sheet */
} ChartCell;

static void PutCellPixel(const ChartCell *cell, int x, int y, int color) {
    int bit = 7 - x % 8;
    uint8 *tile = RomPtr(0xC00000 | (cell->sheet + (uint32)(cell->tile + y / 8 * cell->width + x / 8) * TILE_BYTES));
    for (int plane = 0; plane < 4; plane++) {
        uint8 *byte = tile + (plane >= 2 ? 16 : 0) + y % 8 * 2 + (plane & 1);
        *byte = (uint8)((*byte & ~(1 << bit)) | ((color >> plane & 1) << bit));
    }
}

/* Slash-separated rows of capitals, centred in a box, over whatever was there. */
static void DrawCellText(const ChartCell *cell, int left, int top, int right, int bottom, const char *text) {
    for (int y = top; y <= bottom; y++) {
        for (int x = left; x <= right; x++)
            PutCellPixel(cell, x, y, LABEL_BACK);
    }
    int rows = 1;
    for (const char *p = text; *p; p++)
        rows += *p == '/';
    int y = top + (bottom - top + 1 - (rows * (TINY_ROWS + 1) - 1)) / 2;
    for (const char *p = text; *p; y += TINY_ROWS + 1) {
        size_t length = strcspn(p, "/");
        int x = left + (right - left + 1 - ((int)length * (TINY_COLUMNS + 1) - 1)) / 2;
        for (size_t i = 0; i < length; i++, x += TINY_COLUMNS + 1) {
            char c = p[i];
            c = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
            if (c < 'A' || c > 'Z')
                continue;
            for (int gy = 0; gy < TINY_ROWS; gy++) {
                for (int gx = 0; gx < TINY_COLUMNS; gx++) {
                    if (kTinyLetters[c - 'A'][gy] >> (TINY_COLUMNS - 1 - gx) & 1)
                        PutCellPixel(cell, x + gx, y + gy, LABEL_INK);
                }
            }
        }
        p += length + (p[length] == '/');
    }
}

static void DrawChartName(uint32 sheet, const char *line) {
    int tile, width, height, used = 0;
    if (sscanf(line, "%x %d %d %n", (unsigned *)&tile, &width, &height, &used) != 3)
        return;
    ChartCell cell = {sheet, tile, width};
    /* A box is 12 wide inside its border, centred in the cell. */
    int left = (width * 8 - NAME_BOX_WIDTH) / 2;
    DrawCellText(&cell, left, 1, left + NAME_BOX_WIDTH - 1, height * 8 - 2, line + used);
}

static void DrawChartHeading(uint32 sheet, const char *line) {
    int tile, width, left, top, right, bottom, used = 0;
    if (sscanf(line, "%x %d %d %d %d %d %n", (unsigned *)&tile, &width, &left, &top, &right, &bottom, &used) != 6)
        return;
    ChartCell cell = {sheet, tile, width};
    DrawCellText(&cell, left, top, right, bottom, line + used);
}

bool TextTinyPixel(char letter, int x, int y) {
    if (letter < 'A' || letter > 'Z')
        return false;
    return kTinyLetters[letter - 'A'][y] >> (TINY_COLUMNS - 1 - x) & 1;
}

static void ApplyIconLabels(const char *part_key) {
    char leaf[128], path[1024], line[160];
    snprintf(leaf, sizeof(leaf), "translation/%s/icons.txt", g_language);
    AssetPath(leaf, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char kind[8], parts[32];
        unsigned long sheet;
        int consumed = 0;
        if (line[0] == '#' || sscanf(line, "%7s %31s %lx %n", kind, parts, &sheet, &consumed) != 3)
            continue;
        if (!strstr(parts, part_key))
            continue;
        char *text = line + consumed;
        text[strcspn(text, "\r\n")] = 0;
        if (strcmp(kind, "icon") == 0)
            DrawLabel((uint32)sheet, text);
        else if (strcmp(kind, "item") == 0)
            DrawItemLabel((uint32)sheet, text);
        else if (strcmp(kind, "name") == 0)
            DrawChartName((uint32)sheet, text);
        else if (strcmp(kind, "heading") == 0)
            DrawChartHeading((uint32)sheet, text);
    }
    fclose(f);
}

/* The script interpreter dispatches opcodes with an RTS. If it's running interpreted
 * rather than compiled, that would run the ROM bytes at $C0:FC7F and never reach the
 * hook, because only calls are intercepted. So the opcode's table entry points at a stub
 * in unused ROM that calls it. */
static void InstallOpcodeStub(void) {
    static const uint8 kStub[] = {0x20, 0x7F, 0xFC, 0x60};  /* JSR $FC7F, RTS */
    memcpy(RomPtr(OP_STUB), kStub, sizeof(kStub));
    uint8 *entry = RomPtr(OP_TABLE + OP_TEXT_HOOK * 2);
    entry[0] = (uint8)OP_STUB;
    entry[1] = (uint8)(OP_STUB >> 8);
}

/* Screens outside the scene scripts (status, time check, credits): these rows have no
 * scene to find them through, and code may poke digits into them by offset, so they're
 * rewritten in place, in half-width text, without changing a string's size. */
static void ApplyInPlace(uint32 addr, const char *text) {
    uint8 *bytes = RomPtr(0xC00000 | addr);
    size_t size = strlen((const char *)bytes);
    size_t at = 0;
    while (*text && at < size) {
        if (strncmp(text, "{n}", 3) == 0) {
            bytes[at++] = OP_NEWLINE;
            text += 3;
        } else if (strncmp(text, "{x:", 3) == 0) {
            /* Bytes to leave as the game has them, such as a logo's tiles. */
            unsigned value;
            for (text += 3; at < size && sscanf(text, "%2x", &value) == 1; text += 2)
                bytes[at++] = (uint8)value;
            if (*text == '}')
                text++;
        } else {
            uint32 c = NextCodepoint(&text);
            bytes[at++] = c < 0x80 ? (uint8)c : '?';
        }
    }
    if (at < size)
        memset(bytes + at, ' ', size - at);
}

void TextOnImageLoaded(int part) {
    static const char *const kKeys[PART_COUNT] = {"zen", "chu", "kou"};
    g_run_count = 0;
    g_name_count = 0;
    LoadFonts();
    if (!IsTranslating())
        return;
    LoadTranslation(kKeys[part]);
    ApplyIconLabels(kKeys[part]);
    if (!g_row_count)
        return;
    InstallOpcodeStub();
    for (int scene = 0;; scene++) {
        uint32 addr = ScriptScene(scene);
        if (!addr)
            break;
        ApplyToScene(addr);
    }
    int in_place = 0;
    for (int i = 0; i < g_row_count; i++) {
        if (!g_rows[i].used) {
            ApplyInPlace(g_rows[i].addr, g_rows[i].text);
            in_place++;
        }
    }
    qsort(g_runs, (size_t)g_run_count, sizeof(TextRun), CompareRuns);
    fprintf(stderr, "[text] %d runs, %d names and %d screen strings applied\n", g_run_count, g_name_count, in_place);
}

/* ---- drawing ---------------------------------------------------------------- */

static uint16 Read16(const uint8 *ram, uint32 addr) { return (uint16)(ram[addr] | ram[addr + 1] << 8); }

static void Write16(uint8 *ram, uint32 addr, uint16 value) {
    ram[addr] = (uint8)value;
    ram[addr + 1] = (uint8)(value >> 8);
}

static void MarkRowDirty(uint8 *ram, int row) {
    Write16(ram, WRAM_DIRTY_ROWS, (uint16)(Read16(ram, WRAM_DIRTY_ROWS) | 1u << (row & 15)));
}

/* Strings draw from the script tick, but the game wipes its dirty rows before each
 * pass over the windows, so they're kept here until HleTextDrawPending. */
static void MarkRowDirtyLater(uint8 *ram, int row) {
    Write16(ram, WRAM_PVS_DIRTY_ROWS, (uint16)(Read16(ram, WRAM_PVS_DIRTY_ROWS) | 1u << (row & 15)));
}

/* Pixels across a text window's tile buffer: 2bpp columns of 8 x 16, with one row of
 * blank at the top of each glyph, and a shadow one row down and one pixel right. */
static void DrawGlyphAt(uint8 *ram, int window, int row, int x, const Glyph *glyph, int color) {
    uint16 base = (uint16)(Read16(ram, WRAM_TILE_BASES + (uint32)(window - 1) * 2) * 2 +
                           row * TEXT_ROW_BYTES + TILE_COLUMN_BYTES);
    int column = x / 8, shift = x % 8, plane = color ? 1 : 0;
    MarkRowDirtyLater(ram, row);
    for (int y = GLYPH_ROWS - 1; y >= 0; y--) {
        uint32 bits = (uint32)glyph->rows[y] << 8 >> shift;
        uint32 shadow = bits >> 1;
        for (int i = 0; i < 3; i++) {
            uint16 at = (uint16)(base + (column + i) * TILE_COLUMN_BYTES + (y + 1) * 2);
            uint8 *pixels = ram + WRAM_BANK_7F + at;
            uint8 lit = (uint8)(bits >> (16 - i * 8));
            uint8 shade = (uint8)(shadow >> (16 - i * 8));
            pixels[plane] |= lit;
            pixels[plane ^ 1] &= (uint8)~lit;
            uint8 below = (uint8)(shade & ~(pixels[2] | pixels[3]));
            pixels[2] |= below;
            pixels[3] |= below;
        }
    }
}

static const Glyph *GlyphFor(uint32 codepoint) {
    const Glyph *glyph = FontGlyph(codepoint);
    return glyph ? glyph : FontGlyph('?');
}

static int WordWidth(const char *text) {
    int width = 0;
    while (*text && *text != ' ' && !IsOwnTag(text)) {
        const Glyph *glyph = GlyphFor(NextCodepoint(&text));
        width += glyph ? FontAdvance(glyph) : 0;
    }
    return width;
}

static void NextTextRow(uint8 *ram, uint32 win, RunProgress *p, const char *text) {
    int rows = Read16(ram, win + WIN_ROWS);
    int row = Read16(ram, win + WIN_ROW) + 1;
    if (row >= rows) {
        /* Longer than the window. The linter assumes three rows, and a few windows have two,
         * so PVS_TRACE_TEXT=1 reports it. */
        static int trace = -1;
        if (trace < 0)
            trace = getenv("PVS_TRACE_TEXT") != NULL;
        if (trace)
            fprintf(stderr, "[text] too long for %d rows: %.60s\n", rows, text);
        row = rows - 1;
    }
    Write16(ram, win + WIN_ROW, (uint16)row);
    Write16(ram, win + WIN_COL, 0);
    p->px = p->margin;
    p->row = (uint16)row;
    p->wrapped = 1;
}

/* Blank a window's text and go back to its top. */
static void ClearWindow(uint8 *ram, uint32 win, RunProgress *p) {
    int window = Read16(ram, win + WIN_ID);
    int first = Read16(ram, win + WIN_BASE_ROW), rows = Read16(ram, win + WIN_ROWS);
    int cols = Read16(ram, win + WIN_BASE_COL) + Read16(ram, win + WIN_COLS);
    uint16 base = (uint16)(Read16(ram, WRAM_TILE_BASES + (uint32)(window - 1) * 2) * 2 + TILE_COLUMN_BYTES);
    int tile_columns = (cols * CELL_PIXELS + 7) / 8;
    for (int row = first; row < first + rows; row++) {
        for (int column = Read16(ram, win + WIN_BASE_COL) * CELL_PIXELS / 8; column < tile_columns; column++)
            memset(ram + WRAM_BANK_7F + (uint16)(base + row * TEXT_ROW_BYTES + column * TILE_COLUMN_BYTES), 0, TILE_COLUMN_BYTES);
        MarkRowDirtyLater(ram, row);
    }
    Write16(ram, win + WIN_ROW, 0);
    Write16(ram, win + WIN_COL, 0);
    p->row = 0;
    p->px = p->margin;
    p->wrapped = 1;
}

/* Draw what's next in a string. Returns whether a visible glyph was drawn. */
static bool DrawNextGlyph(uint8 *ram, uint32 win, RunProgress *p, const char *text) {
    const char *at = text + p->offset;
    int width = Read16(ram, win + WIN_COLS) * CELL_PIXELS;
    if (strncmp(at, "{n}", 3) == 0) {
        NextTextRow(ram, win, p, text);
        p->offset += 3;
        return false;
    }
    if (strncmp(at, "{c}", 3) == 0) {
        ClearWindow(ram, win, p);
        p->offset += 3;
        return false;
    }
    if (at[0] == ' ' && p->wrapped) {
        /* A space that a wrap has just made pointless. */
        p->offset++;
        return false;
    }
    bool at_word_start = at == text || at[-1] == ' ' || (at - text >= 3 && IsOwnTag(at - 3));
    if (at_word_start && at[0] != ' ' && !p->wrapped && p->px + WordWidth(at) > width)
        NextTextRow(ram, win, p, text);
    const char *next = at;
    const Glyph *glyph = GlyphFor(NextCodepoint(&next));
    p->offset = (uint16)(next - text);
    if (!glyph)
        return false;
    if (*at == ' ' && p->px + glyph->width > width) {
        /* A space that doesn't fit ends the row instead of starting the next one. */
        NextTextRow(ram, win, p, text);
        return false;
    }
    if (p->px + glyph->width > width)
        NextTextRow(ram, win, p, text);
    int row = Read16(ram, win + WIN_BASE_ROW) + Read16(ram, win + WIN_ROW);
    int x = Read16(ram, win + WIN_BASE_COL) * CELL_PIXELS + p->px;
    DrawGlyphAt(ram, Read16(ram, win + WIN_ID), row, x, glyph, Read16(ram, win + WIN_COLOR));
    p->px = (uint16)(p->px + FontAdvance(glyph));
    p->wrapped = 0;
    int cells = (p->px + CELL_PIXELS - 1) / CELL_PIXELS;
    int cols = Read16(ram, win + WIN_COLS);
    Write16(ram, win + WIN_COL, (uint16)(cells < cols ? cells : cols - 1));
    return *at != ' ';
}

static const TextRun *FindRun(uint32 start) {
    TextRun key = {.start = start};
    return (const TextRun *)bsearch(&key, g_runs, (size_t)g_run_count, sizeof(TextRun), CompareRuns);
}

static RunProgress *ProgressFor(uint8 *ram, uint32 win) {
    return (RunProgress *)(ram + WRAM_PVS_TEXT + (win - WIN_BASE) / WIN_SIZE * sizeof(RunProgress));
}

/* Where to carry on drawing when a new string starts in this window. Within a line the
 * game's position is only in six-pixel cells, so keep ours unless it has moved on. */
static void StartString(uint8 *ram, uint32 win, RunProgress *p, uint16 run, uint16 bank, uint16 start_px) {
    int col = Read16(ram, win + WIN_COL), row = Read16(ram, win + WIN_ROW);
    int cells = (p->px + CELL_PIXELS - 1) / CELL_PIXELS;
    int cols = Read16(ram, win + WIN_COLS);
    if (cells >= cols)
        cells = cols - 1;  /* as DrawNextGlyph reports it */
    if (p->row != row || (!p->wrapped && (col == 0 || cells != col))) {
        p->px = (uint16)(col ? col * CELL_PIXELS : start_px);
        p->margin = p->px;
        p->wrapped = 0;
    }
    p->row = (uint16)row;
    p->run = run;
    p->bank = bank;
    p->offset = 0;
    p->tick = 0;
    p->glyphs = 0;
}

static void PlayTypingSound(uint8 *ram, uint32 win) {
    uint16 sound = Read16(ram, win + WIN_TYPING_SOUND);
    if (Read16(ram, win + WIN_RETURN_PC) != 0xFFFF || sound == 0 || sound == 0xFFFF)
        return;
    if (AudioRecordingCoversEffects(ram))
        return;
    uint8 queued = ram[WRAM_SOUND_QUEUE];
    if (queued == 0 || queued >= (uint8)sound)
        ram[WRAM_SOUND_QUEUE] = (uint8)sound;
}

/* Spread a string's glyphs evenly over the ticks the Japanese took, so that the waits
 * after it, and the voices they're timed to, still line up. */
static bool AdvanceString(uint8 *ram, uint32 win, RunProgress *p, const char *text, int glyph_count, int ticks) {
    p->tick++;
    int due = ticks ? glyph_count * p->tick / ticks : glyph_count;
    bool drew = false;
    while (p->glyphs < due && text[p->offset]) {
        drew |= DrawNextGlyph(ram, win, p, text);
        p->glyphs++;
    }
    if (drew)
        PlayTypingSound(ram, win);
    return p->tick >= ticks;
}

/* $C0:FC7F, the game's handler for opcode $0D, reached through OP_STUB. The script
 * interpreter at $C0:FA82 has already stepped past the opcode. Stack: return to the stub,
 * return to the interpreter, saved S, window, operand pointer. */
RecompReturn HleTextOpcode(CpuState *cpu) {
    uint8 *ram = cpu->ram;
    uint32 win = Read16(ram, (uint32)cpu->S + 7);
    uint16 pc = Read16(ram, win + WIN_PC);
    uint16 bank = Read16(ram, win + WIN_BANK);
    RunProgress *p = ProgressFor(ram, win);
    if (bank == NAME_BANK) {
        /* A speaker name: pc is on its number, in the WRAM copy of the string. */
        int index = ram[pc];
        uint16 terminator = pc;
        while (ram[terminator] != 0)
            terminator++;
        if (p->run != (uint16)index || p->bank != NAME_BANK)
            StartString(ram, win, p, (uint16)index, NAME_BANK, NAME_MARGIN_PX);
        const char *name = index >= 1 && index <= g_name_count ? g_names[index - 1] : "";
        int glyphs = 0;
        for (const char *q = name; *q; glyphs++)
            NextCodepoint(&q);
        int ticks = (terminator - pc + 1) / 2;
        if (AdvanceString(ram, win, p, name, glyphs, ticks ? ticks : 1)) {
            p->run = NO_RUN;
            Write16(ram, win + WIN_PC, terminator);
        } else {
            Write16(ram, win + WIN_PC, (uint16)(pc - 1));
        }
    } else {
        uint32 start = (uint32)bank << 16 | (uint16)(pc - 1);
        const TextRun *run = FindRun(start);
        if (!run) {
            /* Only reachable if the language changed under a save state. */
            Write16(ram, win + WIN_PC, pc);
        } else {
            if (p->run != (uint16)start || p->bank != bank)
                StartString(ram, win, p, (uint16)start, bank, 0);
            if (AdvanceString(ram, win, p, run->text, run->glyph_count, run->ticks)) {
                p->run = NO_RUN;
                Write16(ram, win + WIN_PC, (uint16)run->end);
                char name[32];
                static const char *const kKeys[PART_COUNT] = {"zen", "chu", "kou"};
                snprintf(name, sizeof(name), "%s_%06X", kKeys[ram[WRAM_PVS_PART] % PART_COUNT], (unsigned)(start & 0x3FFFFF));
                BsxTraceTextShot(name);
            } else {
                Write16(ram, win + WIN_PC, (uint16)start);
            }
        }
    }
    cpu->A = 0xFFFF;
    cpu->S += 2;
    return RECOMP_RETURN_NORMAL;
}

/* ---- the game's own glyph routine ------------------------------------------- */

static const uint8 kFullWidthMasks[4][3] = {{0x00, 0x0F, 0xFF}, {0xFC, 0x00, 0x3F}, {0xF0, 0x00, 0xFF}, {0xC0, 0x03, 0xFF}};
static const uint8 kHalfWidthMasks[4][3] = {{0x03, 0xFF, 0xFF}, {0xFC, 0x0F, 0xFF}, {0xF0, 0x3F, 0xFF}, {0xC0, 0xFF, 0xFF}};
static const uint8 kCellShift[4] = {0, 6, 4, 2};
static const uint16 kCellColumnBytes[4] = {0x00, 0x00, 0x20, 0x40};

enum {
    GLYPH_BYTES = 24,
    FONT_TABLE = 0xC0F733,
    FONT_TABLE_LAST = 0x56,
    FONT_ENTRY_SIZE = 6,
    HALF_WIDTH_BANK = 0x8000,
    FIRST_PACK_BANK = 0xC0,
    GAME_SYMBOLS = 0xF000,
};

/* Find a character's bitmap through the game's font table, as $C0:F857 does. Only the
 * game's own symbols are in the pack; the rest of the table points into the BIOS, which
 * only a development image (--dev-bios-font) has. */
static void RomGlyph(uint16 code, uint8 *out, bool *half_width) {
    uint16 sjis = code;
    if (code < 0x8000)
        sjis = (uint16)((code >> 8) + 0x851F);
    else if (code >= 0xA000 && code < 0xE000)
        sjis = (uint16)((code >> 8) + 0x84FE);
    int entry = FONT_TABLE_LAST;
    while (entry > 0 && sjis < (RomPtr(FONT_TABLE + (uint32)entry)[0] | RomPtr(FONT_TABLE + (uint32)entry)[1] << 8))
        entry -= FONT_ENTRY_SIZE;
    const uint8 *row = RomPtr(FONT_TABLE + (uint32)entry);
    uint16 start = (uint16)(row[0] | row[1] << 8), pointer = (uint16)(row[2] | row[3] << 8);
    uint16 bank = (uint16)(row[4] | row[5] << 8);
    uint32 source = (uint32)(bank & 0xFF) << 16 | (uint16)(pointer + (sjis - start) * GLYPH_BYTES);
    *half_width = (bank & HALF_WIDTH_BANK) != 0;
    if ((bank & 0xFF) >= FIRST_PACK_BANK)
        memcpy(out, RomPtr(source), GLYPH_BYTES);
    else
        memset(out, 0, GLYPH_BYTES);
}

static bool IsHalfWidthCode(uint16 code) { return code < 0x8000 || (code >= 0xA000 && code < 0xE000); }

/* Japanese from the Shinonome font, when it's there. Symbols at $F000 up are the game's. */
static bool JisGlyph(uint16 code, uint8 *out, bool *half_width) {
    if (code >= GAME_SYMBOLS)
        return false;
    *half_width = IsHalfWidthCode(code);
    return *half_width ? FontJisHalfWidth((uint8)(code >> 8), out) : FontJisFullWidth(code, out);
}

/* A half-width character from our own font, in the game's bitmap layout. */
static bool LatinGlyph(uint16 code, uint8 *out) {
    if (!IsTranslating() || code >= 0x8000)
        return false;
    const Glyph *glyph = FontGlyph(code >> 8);
    if (!glyph)
        return false;
    /* These screens have a fixed cell per letter, so narrow letters go in the middle of it. */
    int shift = glyph->width < CELL_PIXELS ? (CELL_PIXELS - glyph->width) / 2 : 0;
    for (int y = 0; y < GLYPH_ROWS; y++) {
        uint16 row = (uint16)(glyph->rows[y] >> shift);
        out[y * 2] = (uint8)(row >> 8);
        out[y * 2 + 1] = (uint8)row;
    }
    return true;
}

/* Draw one character at a window's cell, from the font table, combining it with the
 * tile buffer as $C0:F7B1 does. The only change is where the bitmap comes from. */
static void DrawCell(uint8 *ram, int window, int cell, uint16 code, int color) {
    uint8 bitmap[GLYPH_BYTES];
    bool half_width = false;
    if (LatinGlyph(code, bitmap))
        half_width = true;
    else if (!JisGlyph(code, bitmap, &half_width))
        RomGlyph(code, bitmap, &half_width);

    int sub = cell & 3;
    MarkRowDirty(ram, cell >> 6);
    uint16 x = (uint16)(Read16(ram, WRAM_TILE_BASES + (uint32)(window - 1) * 2) * 2 + 0x38 +
                        ((cell & 0x3C0) << 4) + (cell & 0x3C) * 24 + kCellColumnBytes[sub]);
    const uint8 *masks = half_width ? kHalfWidthMasks[sub] : kFullWidthMasks[sub];
    /* The third byte of each shifted row is kept between rows, as in the original. */
    uint8 lit[3] = {0};
    uint8 *bank = ram + WRAM_BANK_7F;
#define AT(offset) bank[(uint16)(x + (offset))]
    /* The shadow's row below the glyph, once: the loop goes bottom up. */
    for (int i = 0; i < 3; i++) {
        AT(i * TILE_COLUMN_BYTES + 2) &= masks[i];
        AT(i * TILE_COLUMN_BYTES + 3) &= masks[i];
    }
    for (int y = GLYPH_ROWS - 1; y >= 0; y--) {
        lit[0] = bitmap[y * 2];
        lit[1] = bitmap[y * 2 + 1];
        for (int shift = kCellShift[sub]; shift > 0; shift--) {
            lit[2] = (uint8)(lit[2] >> 1 | lit[1] << 7);
            lit[1] = (uint8)(lit[1] >> 1 | lit[0] << 7);
            lit[0] >>= 1;
        }
        uint8 shadow[3];
        shadow[0] = lit[0] >> 1;
        shadow[1] = (uint8)(lit[1] >> 1 | lit[0] << 7);
        shadow[2] = (uint8)(lit[2] >> 1 | lit[1] << 7);
        for (int i = 0; i < 3; i++) {
            int column = i * TILE_COLUMN_BYTES;
            AT(column + 0) = (uint8)((AT(column + 0) & masks[i]) | (color ? 0 : lit[i]));
            AT(column + 1) = (uint8)((AT(column + 1) & masks[i]) | (color ? lit[i] : 0));
            uint8 below = (uint8)(shadow[i] & ~(AT(column + 2) | AT(column + 3)));
            AT(column + 2) |= below;
            AT(column + 3) |= below;
        }
        x = (uint16)(x - 2);
    }
#undef AT
}

/* $C0:F7B1. Stack: return, saved S, window, cell, character, colour. */
RecompReturn HleTextDrawGlyph(CpuState *cpu) {
    uint8 *ram = cpu->ram;
    uint32 s = cpu->S;
    DrawCell(ram, Read16(ram, s + 5), Read16(ram, s + 7), Read16(ram, s + 9), Read16(ram, s + 11));
    cpu->S += 2;
    return RECOMP_RETURN_NORMAL;
}

/* $C0:F65B draws each window's pending character, once per pass. */
RecompReturn HleTextDrawPending(CpuState *cpu) {
    uint8 *ram = cpu->ram;
    for (int i = 0; i < WIN_COUNT; i++) {
        uint32 win = WIN_BASE + (uint32)i * WIN_SIZE;
        uint16 code = Read16(ram, win + WIN_PENDING);
        if (Read16(ram, win) == 0xFFFF || code == 0xFFFF)
            continue;
        Write16(ram, win + WIN_DRAWN, code);
        Write16(ram, win + WIN_PENDING, 0xFFFF);
        DrawCell(ram, Read16(ram, win + WIN_ID), Read16(ram, win + WIN_CELL), code, Read16(ram, win + WIN_COLOR));
    }
    Write16(ram, WRAM_DIRTY_ROWS, Read16(ram, WRAM_DIRTY_ROWS) | Read16(ram, WRAM_PVS_DIRTY_ROWS));
    Write16(ram, WRAM_PVS_DIRTY_ROWS, 0);
    cpu->S += 2;
    return RECOMP_RETURN_NORMAL;
}
