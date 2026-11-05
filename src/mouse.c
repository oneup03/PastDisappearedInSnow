/* The game supports the SNES Mouse: left is A, right is B, and two objects read each
 * frame's motion through $C0:D279 (X) and $C0:D289 (Y). The pointer adds it to its
 * position. A menu takes a move of 3 or more in a frame as one step.
 *
 * A real SNES Mouse only reports motion, so its hand would drift away from the desktop's
 * pointer, and the game puts the hand back at its default spot whenever text closes. So
 * each read answers with whatever puts the hand under the pointer, until the d-pad is
 * used, and a menu gets one step per row's distance moved. Nothing here is saved: a save
 * state holds only what the game made of it.
 */
#include "mouse.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "bsx_wram.h"
#include "common_rtl.h"
#include "desktop/sdl_compat.h"
#include "session.h"

enum {
    PAD_A = 0x0080,
    PAD_B = 0x8000,
    PAD_SELECT = 0x2000,
    PAD_DIRECTIONS = 0x0F00,
    /* The pointer object's position, in the object the game keeps at direct page $0C. */
    OBJECT_POINTER_REG = 0x0C,
    OBJECT_X = 0x1E,
    OBJECT_Y = 0x20,
    /* A menu's threshold for one step, and how far to move for one. Rows are 16 pixels. */
    MENU_STEP_MOTION = 4,
    MENU_STEP_PIXELS = 14,
    MAX_SCRIPT = 4096,
};

/* Where each part's reads of $D279 and $D289 return to: the code differs by part. */
static const uint16 kPointerX[PART_COUNT] = {0x7862, 0x7722, 0x78D5};
static const uint16 kPointerY[PART_COUNT] = {0x7889, 0x7749, 0x78FC};

typedef struct ScriptStep {
    int frame, x, y, buttons;
} ScriptStep;

static ScriptStep g_script[MAX_SCRIPT];
static int g_script_count, g_script_next, g_frame;
static int g_trace = -1;

static int g_frame_w = 256, g_frame_h = 224;
static SnesDisplayViewport g_viewport;

static bool g_have_position;
static bool g_in_control;  /* moved since the d-pad last was */
static float g_x, g_y;       /* picture pixels */
static float g_step_x, g_step_y;  /* picture pixels moved and not yet a menu step */
static int g_buttons, g_held;

void MouseSetViewport(int frame_w, int frame_h, const SnesDisplayViewport *viewport) {
    g_frame_w = frame_w;
    g_frame_h = frame_h;
    g_viewport = *viewport;
}

/* Development aid: PVS_TEST_MOUSE=<file> of "<frame> <x> <y> <buttons>" lines, in picture
 * pixels and pad bits (A=128, B=32768), stands in for a real mouse. */
static void LoadScript(void) {
    const char *path = getenv("PVS_TEST_MOUSE");
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f)
        return;
    ScriptStep s;
    while (g_script_count < MAX_SCRIPT && fscanf(f, "%d %d %d %d", &s.frame, &s.x, &s.y, &s.buttons) == 4)
        g_script[g_script_count++] = s;
    fclose(f);
}

static void MoveTo(float x, float y) {
    if (g_have_position && x == g_x && y == g_y)
        return;
    if (g_have_position) {
        g_step_x += x - g_x;
        g_step_y += y - g_y;
        g_in_control = true;
    }
    g_have_position = true;
    g_x = x;
    g_y = y;
}

static void ReadScript(void) {
    while (g_script_next < g_script_count && g_script[g_script_next].frame <= g_frame) {
        const ScriptStep *s = &g_script[g_script_next++];
        MoveTo((float)s->x, (float)s->y);
        g_buttons = s->buttons;
    }
}

static void ReadSdl(void) {
    SDL_Window *window = SDL_GetMouseFocus();
    if (!window || g_viewport.width <= 0 || g_viewport.height <= 0) {
        g_buttons = 0;
        return;
    }
    int window_w = 1, window_h = 1, drawable_w = 1, drawable_h = 1;
    SDL_GetWindowSize(window, &window_w, &window_h);
    SDL_GetWindowSizeInPixels(window, &drawable_w, &drawable_h);
#if SNESRECOMP_SDL3
    float x = 0, y = 0;
#else
    int x = 0, y = 0;
#endif
    uint32 state = SDL_GetMouseState(&x, &y);
    float px = (float)x * drawable_w / window_w, py = (float)y * drawable_h / window_h;
    MoveTo((px - g_viewport.x) * g_frame_w / g_viewport.width,
           (py - g_viewport.y) * g_frame_h / g_viewport.height);
    g_buttons = ((state & SDL_BUTTON_LMASK) ? PAD_A : 0) | ((state & SDL_BUTTON_RMASK) ? PAD_B : 0) |
                ((state & SDL_BUTTON_MMASK) ? PAD_SELECT : 0);
}

/* The game draws its own, and the launcher's window shows it again by itself. */
static void HideDesktopPointer(void) {
#if SNESRECOMP_SDL3
    bool visible = SDL_CursorVisible();
#else
    bool visible = SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE;
#endif
    if (visible)
        snesrecomp_sdl_show_cursor(false);
}

static void WramOr16(uint8 *ram, uint32 addr, int bits) {
    ram[addr] |= (uint8)bits;
    ram[addr + 1] |= (uint8)(bits >> 8);
}

void MouseOnFrame(uint8 *ram) {
    if (g_trace < 0) {
        g_trace = getenv("PVS_TRACE_MOUSE") != NULL;
        LoadScript();
    }
    g_frame++;
    if (g_script_count) {
        ReadScript();
    } else {
        ReadSdl();
        HideDesktopPointer();
    }
    if (WramRead16(ram, WRAM_PAD_HELD) & PAD_DIRECTIONS)
        g_in_control = false;
    int pressed = g_buttons & ~g_held;
    WramOr16(ram, WRAM_PAD_HELD, g_buttons);
    WramOr16(ram, WRAM_PAD_PRESSED, pressed);
    WramOr16(ram, WRAM_PAD_RELEASED, g_held & ~g_buttons);
    /* The pause and hold logic reads the first pad's presses. */
    WramOr16(ram, WRAM_PAD1_PRESSED, pressed);
    g_held = g_buttons;
}

/* One menu step's worth of motion, if the mouse has moved far enough. */
static int MenuStep(float *moved) {
    if (*moved >= MENU_STEP_PIXELS) {
        *moved -= MENU_STEP_PIXELS;
        return MENU_STEP_MOTION;
    }
    if (*moved <= -MENU_STEP_PIXELS) {
        *moved += MENU_STEP_PIXELS;
        return -MENU_STEP_MOTION;
    }
    return 0;
}

/* Stack on entry: return, saved S, which pad. Returns as the original would, with the
 * doubled pad number left in X. */
static RecompReturn ReadMotion(CpuState *cpu, const uint16 *pointer_site, float position, int object_field,
                               float *moved) {
    uint8 *ram = cpu->ram;
    uint16 site = WramRead16(ram, cpu->S + 1);
    int motion;
    if (site == pointer_site[ram[WRAM_PVS_PART]]) {
        uint16 object = WramRead16(ram, (uint16)(cpu->D + OBJECT_POINTER_REG));
        motion = g_in_control ? (int)position - WramRead16(ram, (uint16)(object + object_field)) : 0;
        *moved = 0;
    } else {
        motion = MenuStep(moved);
        if (g_trace > 0 && motion)
            fprintf(stderr, "[mouse] frame %d menu site %04X %d\n", g_frame, site, motion);
    }
    cpu->X = (uint16)(WramRead16(ram, cpu->S + 5) * 2);
    cpu->A = (uint16)motion;
    cpu->S += 2;
    return RECOMP_RETURN_NORMAL;
}

/* $C0:D279. */
RecompReturn HleMouseX(CpuState *cpu) {
    return ReadMotion(cpu, kPointerX, g_x, OBJECT_X, &g_step_x);
}

/* $C0:D289. */
RecompReturn HleMouseY(CpuState *cpu) {
    return ReadMotion(cpu, kPointerY, g_y, OBJECT_Y, &g_step_y);
}
