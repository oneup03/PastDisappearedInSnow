#pragma once
#include "types.h"

/* WRAM bank $7E as the game lays it out, plus what the port adds. The game's data
 * bank is $7E, so its absolute $4xxx operands are these addresses. */
enum {
    /* Clock, as laid out by $C0:EE53 and $C0:EF5E. */
    WRAM_SCENE_SECONDS = 0x4177,  /* signed: negative until the scene's start time */
    WRAM_SCENE_FRAMES = 0x4179,
    WRAM_CHANNEL = 0x417B,
    WRAM_YEAR = 0x417D,
    WRAM_MONTH = 0x417F,
    WRAM_DAY = 0x4180,
    WRAM_WEEKDAY = 0x4181,
    WRAM_HOUR = 0x4182,
    WRAM_MINUTE = 0x4183,
    WRAM_SECOND = 0x4184,
    WRAM_FRAME = 0x4185,
    WRAM_CLOCK_FLAGS = 0x4186,

    /* What's waiting to go to the sound CPU. */
    WRAM_MUSIC_QUEUE = 0x4138,
    WRAM_EFFECT_QUEUE = 0x4139,

    /* New presses this frame, updated by $C0:CD02 just before the clock poll. */
    WRAM_PAD1_PRESSED = 0x414B,
    /* All ports combined. */
    WRAM_PAD_HELD = 0x4143,
    WRAM_PAD_PRESSED = 0x4149,
    WRAM_PAD_RELEASED = 0x414F,

    /* Script variables are words from $42C9. */
    WRAM_VAR_EVENT_STATE = 0x4349,  /* variable $40 */
    WRAM_SCENE_NUMBER = 0x434B,     /* variable $41 */

    /* What the current scene armed when it started: opcodes $04, $0F and $03. */
    EVENT_SLOT_COUNT = 8,
    WRAM_SLOT_SCRIPT = 0x446B,
    WRAM_SLOT_BANK = 0x447B,
    WRAM_SLOT_TIME = 0x448B,
    WRAM_DEADLINE_SCRIPT = 0x449B,
    WRAM_DEADLINE_TIME = 0x449F,


    /* The port's own state, in memory the game never touches (see tests). */
    WRAM_PVS_RELEASED_SCENE = 0xFF00,
    WRAM_PVS_RELEASED_SECOND = 0xFF02,
    WRAM_PVS_PART = 0xFF04,
    WRAM_PVS_DIRTY_ROWS = 0xFF06,
    WRAM_PVS_TEXT = 0xFF10,  /* a RunProgress for each text window */
};

static inline uint16 WramRead16(const uint8 *ram, uint32 addr) {
    return (uint16)(ram[addr] | ram[addr + 1] << 8);
}
