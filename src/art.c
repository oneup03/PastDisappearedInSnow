/* English over the Japanese in the game's pictures: signs, the poster, the logo.
 *
 * Pictures are packed in the ROM, so this works on what they unpack to in VRAM. A rule in
 * translation/<language>/art.txt names a rectangle of the screen and a hash of how it
 * looks with the Japanese in it. Whenever a frame shows exactly that, the text is drawn
 * over it. A hash is only a number, so no ROM art is in the repo, and since VRAM is
 * in save states, a loaded state either has the text already or gets it again.
 *
 * Tiles used in more than one place would change everywhere, which is why a rule's hash
 * covers only the rectangle it means to change.
 */
#include "art.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_rtl.h"
#include "cpu_state.h"
#include "font.h"
#include "snes/ppu.h"
#include "text.h"

enum {
    MAX_RULES = 64,
    MAX_TEXT = 96,
    TILE_PIXELS = 8,
    WORDS_PER_PLANE_PAIR = 8,
    LINE_GAP = 1,
    /* A picture arrives over several frames: wait until it has stopped changing. */
    STEADY_FRAMES = 8,
    TINY_SPACE = 2,
};

typedef struct Rule {
    int layer, x, y, w, h;
    uint32 hash;
    bool tiny;
    bool big;   /* the text font at twice the size */
    bool map;   /* x and y are in the layer's own space, so a picture that pans keeps its rule */
    bool fill;  /* cover the Japanese first: off where the picture carries on behind it */
    char text[MAX_TEXT];
} Rule;

static Rule g_rules[MAX_RULES];
static int g_steady[MAX_RULES];

/* What a drawn rule took, so that it can tell when the game writes into one of its tiles. */
enum { MAX_CELLS = 26 * 19, MAX_TILE_WORDS = 32 };
typedef struct Drawn {
    bool active;
    int paper, ink;
    int cells;
    uint16 entry_at[MAX_CELLS];     /* map word of each cell */
    uint16 tile[MAX_CELLS];         /* the tile it was given */
    uint16 words[MAX_CELLS][MAX_TILE_WORDS];  /* what was left in it */
    int ox, oy, ow, oh;             /* the cells' area on screen, and the picture there before */
    uint16 *before;
} Drawn;
static Drawn g_drawn[MAX_RULES];
static bool g_banned[1024];         /* tiles the game turned out to use */
static int g_rule_count;

typedef struct TileSpot {
    uint16 base;     /* first word of the tile */
    int px, py;
    int planes;
    int palette;
} TileSpot;

static int LayerBpp(int layer) {
    static const uint8 kBpp[8][4] = {{2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0},
                                     {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0}, {8, 0, 0, 0}};
    return kBpp[g_ppu->bgmode & 7][layer];
}

static TileSpot SpotAt(int layer, int x, int y) {
    const Ppu *ppu = g_ppu;
    int sc = ppu->bgXsc[layer];
    int bx = (x + ppu->hScroll[layer]) & (sc & 1 ? 511 : 255);
    int by = (y + ppu->vScroll[layer]) & (sc & 2 ? 511 : 255);
    uint32 map = (uint32)(sc & 0xFC) << 8;
    if (bx >= 256)
        map += 0x400;
    if (by >= 256)
        map += sc & 1 ? 0x800 : 0x400;
    uint16 entry = ppu->vram[(map + (by >> 3 & 31) * 32 + (bx >> 3 & 31)) & 0x7FFF];
    int bpp = LayerBpp(layer);
    uint32 tiles = (uint32)(ppu->bgTileAdr >> (layer * 4) & 15) << 12;
    TileSpot spot;
    spot.base = (uint16)((tiles + (entry & 0x3FF) * (uint32)(bpp * 4)) & 0x7FFF);
    spot.px = entry & 0x4000 ? 7 - (bx & 7) : bx & 7;
    spot.py = entry & 0x8000 ? 7 - (by & 7) : by & 7;
    spot.planes = bpp;
    spot.palette = entry >> 10 & 7;
    return spot;
}

static int GetPixel(int layer, int x, int y) {
    TileSpot s = SpotAt(layer, x, y);
    int value = 0;
    for (int plane = 0; plane < s.planes; plane += 2) {
        uint16 word = g_ppu->vram[(s.base + s.py + plane / 2 * WORDS_PER_PLANE_PAIR) & 0x7FFF];
        value |= (word >> (7 - s.px) & 1) << plane;
        value |= (word >> (15 - s.px) & 1) << (plane + 1);
    }
    return value;
}

static void PutPixel(int layer, int x, int y, int value) {
    TileSpot s = SpotAt(layer, x, y);
    for (int plane = 0; plane < s.planes; plane += 2) {
        uint16 *word = &g_ppu->vram[(s.base + s.py + plane / 2 * WORDS_PER_PLANE_PAIR) & 0x7FFF];
        uint16 low = (uint16)(1 << (7 - s.px)), high = (uint16)(1 << (15 - s.px));
        *word = (uint16)((*word & ~(low | high)) | (value >> plane & 1 ? low : 0) | (value >> (plane + 1) & 1 ? high : 0));
    }
}

static uint32 HashRect(int layer, int x, int y, int w, int h) {
    uint32 hash = 2166136261u;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++)
            hash = (hash ^ (uint32)GetPixel(layer, x + i, y + j)) * 16777619u;
    }
    return hash;
}

static int Luminance(int colour) {
    int r = colour & 31, g = colour >> 5 & 31, b = colour >> 10 & 31;
    return 2 * r + 5 * g + b;
}

static int ColourAt(int layer, int x, int y) {
    TileSpot spot = SpotAt(layer, x, y);
    int base = spot.planes == 8 ? 0 : spot.palette << spot.planes;
    return g_ppu->cgram[(base + GetPixel(layer, x, y)) & 255] & 0x7FFF;
}

/* The commonest colour in the rectangle is the sign, and the colour that stands out from it
 * most, of those that take up a real share of the rectangle, is what the Japanese was in.
 * Colours, not colour numbers: neighbouring tiles can use different palettes. */
static void SignColours(const Rule *rule, int *paper, int *ink) {
    enum { MAX_SEEN = 64 };
    int colours[MAX_SEEN], counts[MAX_SEEN], seen = 0;
    for (int j = 0; j < rule->h; j++) {
        for (int i = 0; i < rule->w; i++) {
            int c = ColourAt(rule->layer, rule->x + i, rule->y + j), k = 0;
            while (k < seen && colours[k] != c)
                k++;
            if (k == seen) {
                if (seen == MAX_SEEN)
                    continue;
                colours[seen] = c;
                counts[seen++] = 0;
            }
            counts[k]++;
        }
    }
    int p = 0;
    for (int k = 1; k < seen; k++) {
        if (counts[k] > counts[p])
            p = k;
    }
    *paper = seen ? colours[p] : 0;
    /* The commonest of the colours that stand well apart from the paper. */
    int widest = 0;
    for (int k = 0; k < seen; k++) {
        int contrast = abs(Luminance(colours[k]) - Luminance(*paper));
        if (contrast > widest)
            widest = contrast;
    }
    *ink = *paper;
    int most = 0;
    for (int k = 0; k < seen; k++) {
        if (k != p && abs(Luminance(colours[k]) - Luminance(*paper)) * 2 >= widest && counts[k] > most) {
            most = counts[k];
            *ink = colours[k];
        }
    }
}

static int Distance(int a, int b) {
    int dr = (a & 31) - (b & 31), dg = (a >> 5 & 31) - (b >> 5 & 31), db = (a >> 10 & 31) - (b >> 10 & 31);
    return dr * dr + dg * dg + db * db;
}

/* How well a palette can show a colour: the distance to its nearest entry. */
static int PaletteMiss(int base, int count, int colour) {
    int best = 1 << 30;
    for (int v = 1; v < count; v++) {
        int d = Distance(g_ppu->cgram[(base + v) & 255], colour);
        if (d < best)
            best = d;
    }
    return best;
}

/* Puts the colour nearest to `colour` that this pixel's tile can show. */
static void PutColour(int layer, int x, int y, int colour) {
    TileSpot spot = SpotAt(layer, x, y);
    int count = 1 << spot.planes, base = spot.planes == 8 ? 0 : spot.palette << spot.planes;
    int best = 0, best_distance = 1 << 30;
    for (int v = 1; v < count; v++) {
        int c = g_ppu->cgram[(base + v) & 255];
        int dr = (c & 31) - (colour & 31), dg = (c >> 5 & 31) - (colour >> 5 & 31), db = (c >> 10 & 31) - (colour >> 10 & 31);
        int distance = dr * dr + dg * dg + db * db;
        if (distance < best_distance) {
            best_distance = distance;
            best = v;
        }
    }
    PutPixel(layer, x, y, best);
}

static bool TileIsBlank(uint32 base, int words) {
    for (int w = 0; w < words; w++) {
        if (g_ppu->vram[(base + (uint32)w) & 0x7FFF])
            return false;
    }
    return true;
}

static bool MakeTilesPrivate(const Rule *rule, Drawn *drawn) {
    enum { TILE_COUNT = 1024, MAP_SCREEN_WORDS = 0x400, TILE_BANK_WORDS = 0x1000 };
    const int layer = rule->layer, bpp = LayerBpp(layer), tile_words = bpp * 4;
    const uint32 tiles = (uint32)(g_ppu->bgTileAdr >> (layer * 4) & 15) << 12;
    static bool taken[TILE_COUNT];
    memset(taken, 0, sizeof(taken));
    /* Whatever any layer's map names, in any screen of it. */
    for (int l = 0; l < 3; l++) {
        if (!LayerBpp(l))
            continue;
        int sc = g_ppu->bgXsc[l];
        uint32 map = (uint32)(sc & 0xFC) << 8;
        int words = MAP_SCREEN_WORDS * (sc & 1 ? 2 : 1) * (sc & 2 ? 2 : 1);
        int other_bpp = LayerBpp(l);
        uint32 other = (uint32)(g_ppu->bgTileAdr >> (l * 4) & 15) << 12;
        for (int w = 0; w < words; w++) {
            uint16 entry = g_ppu->vram[(map + (uint32)w) & 0x7FFF];
            uint32 start = other + (uint32)(entry & 0x3FF) * (uint32)(other_bpp * 4);
            for (int part = 0; part < other_bpp * 4; part += tile_words) {
                uint32 at = (start + (uint32)part - tiles) & 0x7FFF;
                if (at / (uint32)tile_words < TILE_COUNT)
                    taken[at / (uint32)tile_words] = true;
            }
        }
        /* The text layer draws its letters into its tiles as it goes, so all of them are spoken for. */
        if (l == 2 && l != layer) {
            for (uint32 w = 0; w < 0x2000; w += (uint32)tile_words) {
                uint32 at = (other + w - tiles) & 0x7FFF;
                if (at / (uint32)tile_words < TILE_COUNT)
                    taken[at / (uint32)tile_words] = true;
            }
        }
        /* The map itself can sit in this layer's tile range. */
        for (int w = 0; w < words; w += tile_words) {
            uint32 at = (map + (uint32)w - tiles) & 0x7FFF;
            if (at / (uint32)tile_words < TILE_COUNT)
                taken[at / (uint32)tile_words] = true;
        }
    }
    /* Sprites' tiles. */
    for (int page = 0; page < 2; page++) {
        uint32 obj = (uint32)(page ? PPU_objTileAdr2(g_ppu) : PPU_objTileAdr1(g_ppu));
        for (int w = 0; w < TILE_BANK_WORDS; w += tile_words) {
            uint32 at = (obj + (uint32)w - tiles) & 0x7FFF;
            if (at / (uint32)tile_words < TILE_COUNT)
                taken[at / (uint32)tile_words] = true;
        }
    }
    for (int t = 0; t < TILE_COUNT; t++)
        taken[t] |= g_banned[t];
    /* Of the palettes the rectangle's tiles use, the one that shows paper and ink best. */
    int best_palette = 0, best_miss = 1 << 30;
    if (bpp < 8) {
        for (int j = 0; j < rule->h; j += 4) {
            for (int i = 0; i < rule->w; i += 4) {
                int palette = SpotAt(layer, rule->x + i, rule->y + j).palette;
                int base = palette << bpp, count = 1 << bpp;
                int miss = PaletteMiss(base, count, drawn->paper) + PaletteMiss(base, count, drawn->ink);
                if (miss < best_miss) {
                    best_miss = miss;
                    best_palette = palette;
                }
            }
        }
    }
    drawn->cells = 0;
    int next;
    /* Walk the layer's own tile grid: a scrolled layer's tiles don't line up with the screen's. */
    const int hs = g_ppu->hScroll[layer], vs = g_ppu->vScroll[layer];
    for (int ty = (rule->y + vs) >> 3; ty <= (rule->y + rule->h - 1 + vs) >> 3; ty++) {
        for (int tx = (rule->x + hs) >> 3; tx <= (rule->x + rule->w - 1 + hs) >> 3; tx++) {
            const int sx = tx * 8 - hs, sy = ty * 8 - vs;  /* the tile's corner on screen */
            /* An empty tile first: one with old picture in it may be about to be used again. */
            next = -1;
            for (int pass = 0; pass < 2 && next < 0; pass++) {
                for (int t = TILE_COUNT - 1; t >= 0; t--) {
                    if (!taken[t] && (pass || TileIsBlank(tiles + (uint32)t * (uint32)tile_words, tile_words))) {
                        next = t;
                        break;
                    }
                }
            }
            if (next < 0)
                return false;
            taken[next] = true;
            int sc = g_ppu->bgXsc[layer];
            int bx = (tx * 8) & (sc & 1 ? 511 : 255);
            int by = (ty * 8) & (sc & 2 ? 511 : 255);
            uint32 map = (uint32)(sc & 0xFC) << 8;
            if (bx >= 256)
                map += 0x400;
            if (by >= 256)
                map += sc & 1 ? 0x800 : 0x400;
            uint16 *entry = &g_ppu->vram[(map + (by >> 3 & 31) * 32 + (bx >> 3 & 31)) & 0x7FFF];
            /* Keep its picture: the rectangle's edges may cut through this tile. */
            int pixels[8][8];
            for (int j = 0; j < 8; j++) {
                for (int i = 0; i < 8; i++)
                    pixels[j][i] = ColourAt(layer, sx + i, sy + j);
            }
            int keep = *entry & 0x3C00;
            bool inside = sx >= rule->x && sy >= rule->y && sx + 8 <= rule->x + rule->w && sy + 8 <= rule->y + rule->h;
            if (inside && bpp < 8)
                keep = (keep & 0x2000) | best_palette << 10;
            *entry = (uint16)(keep | next);
            if (drawn->cells < MAX_CELLS) {
                drawn->entry_at[drawn->cells] = (uint16)(entry - g_ppu->vram);
                drawn->tile[drawn->cells] = (uint16)next;
                drawn->cells++;
            }
            for (int j = 0; j < 8; j++) {
                for (int i = 0; i < 8; i++)
                    PutColour(layer, sx + i, sy + j, pixels[j][i]);
            }
        }
    }
    return true;
}

static int Advance(const Rule *rule, char c) {
    if (rule->tiny)
        return c == ' ' ? TINY_SPACE : TEXT_TINY_COLUMNS + 1;
    const Glyph *glyph = FontGlyph((uint8)c);
    return glyph ? FontAdvance(glyph) : 0;
}

static uint32 TileBase(int layer, int tile) {
    uint32 tiles = (uint32)(g_ppu->bgTileAdr >> (layer * 4) & 15) << 12;
    return (tiles + (uint32)tile * (uint32)(LayerBpp(layer) * 4)) & 0x7FFF;
}

static void Remember(const Rule *rule, Drawn *drawn) {
    for (int c = 0; c < drawn->cells; c++) {
        for (int w = 0; w < LayerBpp(rule->layer) * 4; w++)
            drawn->words[c][w] = g_ppu->vram[(TileBase(rule->layer, drawn->tile[c]) + (uint32)w) & 0x7FFF];
    }
    drawn->active = true;
}

/* first: the picture as the game drew it, which is kept to draw again from if needed. */
static void DrawRule(const Rule *rule, Drawn *drawn, bool first) {
    const int layer = rule->layer, hs = g_ppu->hScroll[layer], vs = g_ppu->vScroll[layer];
    if (first) {
        SignColours(rule, &drawn->paper, &drawn->ink);
        if (getenv("PVS_TRACE_ART"))
            fprintf(stderr, "[art] draw '%s' paper %04x ink %04x\n", rule->text, drawn->paper, drawn->ink);
        if (drawn->paper == drawn->ink || !drawn->paper && !drawn->ink)
            return;
        drawn->ox = ((rule->x + hs) & ~7) - hs;
        drawn->oy = ((rule->y + vs) & ~7) - vs;
        drawn->ow = ((rule->x + rule->w - 1 + hs) | 7) - hs + 1 - drawn->ox;
        drawn->oh = ((rule->y + rule->h - 1 + vs) | 7) - vs + 1 - drawn->oy;
        free(drawn->before);
        drawn->before = malloc((size_t)(drawn->ow * drawn->oh) * sizeof(uint16));
        if (!drawn->before)
            return;
        for (int j = 0; j < drawn->oh; j++) {
            for (int i = 0; i < drawn->ow; i++)
                drawn->before[j * drawn->ow + i] = (uint16)ColourAt(layer, drawn->ox + i, drawn->oy + j);
        }
    }
    if (!MakeTilesPrivate(rule, drawn)) {
        fprintf(stderr, "[art] no free tiles for: %s\n", rule->text);
        drawn->active = false;
        return;
    }
    for (int j = 0; j < drawn->oh; j++) {
        for (int i = 0; i < drawn->ow; i++)
            PutColour(layer, drawn->ox + i, drawn->oy + j, drawn->before[j * drawn->ow + i]);
    }
    int paper = drawn->paper, ink = drawn->ink;
    if (rule->fill) {
        for (int j = 0; j < rule->h; j++) {
            for (int i = 0; i < rule->w; i++)
                PutColour(rule->layer, rule->x + i, rule->y + j, paper);
        }
    }
    /* Rows are split by "|", and centred both ways. */
    char text[MAX_TEXT];
    snprintf(text, sizeof(text), "%s", rule->text);
    const char *rows[8];
    int row_count = 0;
    for (char *part = strtok(text, "|"); part && row_count < 8; part = strtok(NULL, "|"))
        rows[row_count++] = part;
    int scale = rule->big ? 2 : 1;
    int row_height = (rule->tiny ? TEXT_TINY_ROWS : GLYPH_ROWS) * scale + LINE_GAP;
    int top = rule->y + (rule->h - row_count * row_height + LINE_GAP) / 2;
    for (int r = 0; r < row_count; r++) {
        int width = 0;
        for (const char *p = rows[r]; *p; p++)
            width += Advance(rule, *p) * scale;
        int pen = rule->x + (rule->w - width) / 2;
        for (const char *p = rows[r]; *p; p++) {
            const Glyph *glyph = rule->tiny ? NULL : FontGlyph((uint8)*p);
            int columns = rule->tiny ? TEXT_TINY_COLUMNS : glyph ? glyph->width : 0;
            int lines = rule->tiny ? TEXT_TINY_ROWS : GLYPH_ROWS;
            for (int j = 0; j < lines; j++) {
                for (int i = 0; i < columns; i++) {
                    bool on = rule->tiny ? TextTinyPixel(*p, i, j) : (glyph->rows[j] & (0x8000 >> i)) != 0;
                    for (int sy = 0; on && sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++)
                            PutColour(rule->layer, pen + i * scale + sx, top + r * row_height + j * scale + sy, ink);
                    }
                }
            }
            pen += Advance(rule, *p) * scale;
        }
    }
    Remember(rule, drawn);
}

void ArtLoad(const char *language, int part) {
    static const char *const kParts[] = {"zen", "chu", "kou"};
    g_rule_count = 0;
    memset(g_steady, 0, sizeof(g_steady));
    memset(g_banned, 0, sizeof(g_banned));
    for (int i = 0; i < MAX_RULES; i++)
        g_drawn[i].active = false;
    char leaf[128], path[1024];
    snprintf(leaf, sizeof(leaf), "translation/%s/art.txt", language);
    AssetPath(leaf, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    while (fgets(line, sizeof(line), f) && g_rule_count < MAX_RULES) {
        line[strcspn(line, "\r\n")] = 0;
        char parts[32], style[16];
        Rule rule = {0};
        int used = 0;
        if (sscanf(line, "art %31s %d %d %d %d %d %x %15s %n", parts, &rule.layer, &rule.x, &rule.y, &rule.w, &rule.h,
                   &rule.hash, style, &used) < 8 || !used)
            continue;
        if (!strstr(parts, kParts[part]))
            continue;
        rule.tiny = strstr(style, "tiny") != NULL;
        rule.big = strstr(style, "big") != NULL;
        rule.map = strstr(style, "map") != NULL;
        rule.fill = strstr(style, "over") == NULL;
        snprintf(rule.text, sizeof(rule.text), "%s", line + used);
        g_rules[g_rule_count++] = rule;
    }
    fclose(f);
}

/* Development aid: PVS_TRACE_ART=<file> of "<name> <layer> <x> <y> <w> <h>" lines prints each
 * rectangle's hash when it changes, with the clock, for writing a rule. */
static void TraceArt(void) {
    enum { MAX_TRACE = 64 };
    static struct {
        char name[24];
        int layer, x, y, w, h;
        uint32 last;
    } rects[MAX_TRACE];
    static int count = -1;
    if (count < 0) {
        count = 0;
        const char *path = getenv("PVS_TRACE_ART");
        FILE *f = path ? fopen(path, "r") : NULL;
        while (f && count < MAX_TRACE && fscanf(f, "%23s %d %d %d %d %d", rects[count].name, &rects[count].layer, &rects[count].x,
                                                &rects[count].y, &rects[count].w, &rects[count].h) == 6)
            count++;
        if (f)
            fclose(f);
    }
    const char *thumbs = getenv("PVS_TRACE_ART_DIR");
    const bool map = getenv("PVS_TRACE_ART_MAP") != NULL;
    for (int i = 0; i < count; i++) {
        if (!LayerBpp(rects[i].layer))
            continue;
        int sx = rects[i].x - (map ? g_ppu->hScroll[rects[i].layer] & 511 : 0);
        int sy = rects[i].y - (map ? g_ppu->vScroll[rects[i].layer] & 511 : 0);
        uint32 hash = HashRect(rects[i].layer, sx, sy, rects[i].w, rects[i].h);
        if (hash != rects[i].last && thumbs) {
            char name[256];
            snprintf(name, sizeof(name), "%s/%s_%d_%08x.ppm", thumbs, rects[i].name, rects[i].layer, hash);
            FILE *t = fopen(name, "wb");
            if (t) {
                fprintf(t, "P6 %d %d 255\n", rects[i].w, rects[i].h);
                for (int y = 0; y < rects[i].h; y++) {
                    for (int x = 0; x < rects[i].w; x++) {
                        TileSpot spot = SpotAt(rects[i].layer, sx + x, sy + y);
                        int base = spot.planes == 8 ? 0 : spot.palette << spot.planes;
                        int c = g_ppu->cgram[(base + GetPixel(rects[i].layer, sx + x, sy + y)) & 255];
                        uint8 rgb[3] = {(uint8)((c & 31) * 8), (uint8)((c >> 5 & 31) * 8), (uint8)((c >> 10 & 31) * 8)};
                        fwrite(rgb, 1, 3, t);
                    }
                }
                fclose(t);
            }
        }
        if (hash != rects[i].last)
            fprintf(stderr, "[art] %s layer %d hash %08x mode %d scroll %d,%d clock %d:%d.%d\n", rects[i].name,
                    rects[i].layer, hash, g_ppu->bgmode, g_ppu->hScroll[rects[i].layer] & 511, g_ppu->vScroll[rects[i].layer] & 511, g_cpu.ram[0x4183], g_cpu.ram[0x4184], g_cpu.ram[0x4185]);
        rects[i].last = hash;
    }
}

/* Whether the palette held still since the last frame: pictures fade in by changing it. */
static bool PaletteSteady(void) {
    static uint32 last;
    uint32 hash = 2166136261u;
    for (int c = 0; c < 256; c++)
        hash = (hash ^ g_ppu->cgram[c]) * 16777619u;
    bool steady = hash == last;
    last = hash;
    return steady;
}

void ArtOnFrame(void) {
    if (!g_ppu)
        return;
    TraceArt();
    const bool steady = PaletteSteady();
    for (int i = 0; i < g_rule_count; i++) {
        Rule screen = g_rules[i];
        if (screen.map) {
            screen.x -= g_ppu->hScroll[screen.layer] & 511;
            screen.y -= g_ppu->vScroll[screen.layer] & 511;
            if (screen.x < 0 || screen.y < 0 || screen.x + screen.w > 256 || screen.y + screen.h > 224)
                continue;
        }
        const Rule *rule = &screen;
        if (!LayerBpp(rule->layer))
            continue;
        Drawn *drawn = &g_drawn[i];
        if (drawn->active) {
            /* Still this picture? Then check the tiles it was given are as it left them. */
            bool ours = true, intact = true;
            for (int c = 0; c < drawn->cells && ours; c++) {
                ours = (g_ppu->vram[drawn->entry_at[c]] & 0x3FF) == drawn->tile[c];
                for (int w = 0; ours && w < LayerBpp(rule->layer) * 4; w++) {
                    if (g_ppu->vram[(TileBase(rule->layer, drawn->tile[c]) + (uint32)w) & 0x7FFF] != drawn->words[c][w]) {
                        g_banned[drawn->tile[c]] = true;
                        intact = false;
                    }
                }
            }
            if (!ours)
                drawn->active = false;
            else if (!intact && steady)
                DrawRule(rule, drawn, false);
            continue;
        }
        if (steady && HashRect(rule->layer, rule->x, rule->y, rule->w, rule->h) == rule->hash) {
            if (++g_steady[i] == STEADY_FRAMES)
                DrawRule(rule, drawn, true);
        } else {
            g_steady[i] = 0;
        }
    }
}
