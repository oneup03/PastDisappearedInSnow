/* Pacing modes: how the broadcast clock treats the player.
 *
 * Real Time runs the clock without stopping, as in 1997: timed events and the
 * scene's deadline interrupt whatever the player was doing.
 *
 * Unlimited stops the clock one second before each timed event or deadline in a
 * scene where the player can act, until they choose to move on. Everything is
 * read from what the game itself armed when the scene started ($C0:8444), so no
 * part of this is specific to a scene. The clock still runs through cutscenes,
 * which stay in sync with their voices.
 *
 * What this adds to the machine lives in WRAM so that save states and rewind
 * capture it. The rest is recomputed each frame.
 */
#include "pacing.h"

#include <stdio.h>

#include "bsx_wram.h"
#include "common_rtl.h"
#include "script.h"
#include "session.h"
#include "snes_osd.h"

enum {
    /* Each scene opens with events at +0s and +2s that start it. */
    LAST_OPENING_EVENT_SECOND = 2,
    /* The game's script variable for "a timed event is playing out". */
    EVENT_RUNNING = 0xFF,
    /* Select isn't used by the game. High byte of the pad word. */
    PAD_HIGH_SELECT = 0x20,
    HOLD_NOTICE_FRAMES = 60 * 20,
    HOLD_NOTICE_MS = 5000,
    MAX_SCENES = 32,
};

static PacingMode g_mode;
static bool g_holding;
static int g_notice_frames;

void PacingSetMode(PacingMode mode) { g_mode = mode; }
PacingMode PacingGetMode(void) { return g_mode; }
bool PacingIsHolding(void) { return g_holding; }

/* True if the scene defines any actions for the player. Depends only on the ROM. */
static bool SceneHasActions(int part, int scene) {
    static int8 parts[PART_COUNT][MAX_SCENES];  /* 0 unknown, 1 yes, -1 no */
    if (scene <= 0 || scene >= MAX_SCENES)
        return false;
    int8 *cache = parts[part];
    if (!cache[scene]) {
        cache[scene] = -1;
        ScriptItem item = {.next = ScriptScene(scene)};
        do {
            ScriptStep(item.next, &item);
            if (item.op == OP_LABEL)
                cache[scene] = 1;
        } while (item.op != OP_END);
    }
    return cache[scene] > 0;
}

bool PacingSceneHasActions(const uint8 *ram) {
    return SceneHasActions(ram[WRAM_PVS_PART], WramRead16(ram, WRAM_SCENE_NUMBER));
}

/* The soonest second, on the scene counter, at which anything is armed to happen.
 * -1 if nothing is. */
static int NextTimedSecond(const uint8 *ram) {
    int next = -1;
    for (int i = 0; i < EVENT_SLOT_COUNT; i++) {
        if (WramRead16(ram, WRAM_SLOT_SCRIPT + i * 2) == 0xFFFF)
            continue;
        int when = WramRead16(ram, WRAM_SLOT_TIME + i * 2);
        if (next < 0 || when < next)
            next = when;
    }
    if (WramRead16(ram, WRAM_DEADLINE_SCRIPT) != 0xFFFF) {
        int when = WramRead16(ram, WRAM_DEADLINE_TIME);
        if (next < 0 || when < next)
            next = when;
    }
    return next;
}

/* An event's scene can run past the next event's time. Take the clocks back so the
 * player still gets to act before it fires. */
static void TakeClocksBack(uint8 *ram, int seconds) {
    int scene = (int16)WramRead16(ram, WRAM_SCENE_SECONDS) - seconds;
    ram[WRAM_SCENE_SECONDS] = (uint8)scene;
    ram[WRAM_SCENE_SECONDS + 1] = (uint8)(scene >> 8);
    int total = ram[WRAM_MINUTE] * 60 + ram[WRAM_SECOND] - seconds;
    if (total >= 0) {
        ram[WRAM_MINUTE] = (uint8)(total / 60);
        ram[WRAM_SECOND] = (uint8)(total % 60);
    }
}

static void Log(const uint8 *ram, const char *what, int next) {
    fprintf(stderr, "[pacing] %s: clock %02d:%02d scene %d +%ds, next at +%ds\n", what,
            ram[WRAM_MINUTE], ram[WRAM_SECOND], WramRead16(ram, WRAM_SCENE_NUMBER),
            (int16)WramRead16(ram, WRAM_SCENE_SECONDS), next);
}

static bool ShouldHold(uint8 *ram) {
    if (g_mode != PACING_UNLIMITED)
        return false;
    int scene = WramRead16(ram, WRAM_SCENE_NUMBER);
    if (!PacingSceneHasActions(ram))
        return false;
    int next = NextTimedSecond(ram);
    if (next <= LAST_OPENING_EVENT_SECOND)
        return false;
    int now = (int16)WramRead16(ram, WRAM_SCENE_SECONDS);
    if (now < next - 1 || ram[WRAM_VAR_EVENT_STATE] == EVENT_RUNNING)
        return false;
    if (WramRead16(ram, WRAM_PVS_RELEASED_SCENE) == scene &&
        WramRead16(ram, WRAM_PVS_RELEASED_SECOND) == next)
        return false;
    if (ram[WRAM_PAD1_PRESSED + 1] & PAD_HIGH_SELECT) {
        ram[WRAM_PVS_RELEASED_SCENE] = (uint8)scene;
        ram[WRAM_PVS_RELEASED_SCENE + 1] = (uint8)(scene >> 8);
        ram[WRAM_PVS_RELEASED_SECOND] = (uint8)next;
        ram[WRAM_PVS_RELEASED_SECOND + 1] = (uint8)(next >> 8);
        Log(ram, "moved on", next);
        snes_osd_push("Moving on", 0);
        return false;
    }
    if (now >= next)
        TakeClocksBack(ram, now - (next - 1));
    if (!g_holding) {
        Log(ram, "holding", next);
        g_notice_frames = 0;
    }
    return true;
}

bool PacingOnFrame(uint8 *ram) {
    g_holding = ShouldHold(ram);
    if (g_holding && g_notice_frames-- <= 0) {
        snes_osd_push("Time stopped. SELECT: move on", HOLD_NOTICE_MS);
        g_notice_frames = HOLD_NOTICE_FRAMES;
    }
    return g_holding;
}
