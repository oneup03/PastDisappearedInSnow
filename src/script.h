#pragma once
#include <stdbool.h>

#include "types.h"

/* The game's scene bytecode. Scenes are decoded the same linear way the game's own
 * pre-scan at $C0:8444 walks them, with operand sizes from its table at $C0:79F4. */
enum {
    OP_END = 0x02,
    OP_DEADLINE = 0x03,
    OP_LABEL = 0x05,
    OP_STRING = 0x06,
    OP_NEWLINE = 0x0A,
    OP_TEXT_HOOK = 0x0D,  /* unused by the game; the translation path takes it over */
    OP_SPEED = 0x0C,
    OP_WAIT = 0x0E,
    OP_MENU = 0x14,
    OP_SPEAKER = 0x19,
    OP_1A = 0x1A,
    OP_SOUND = 0x1D,
    MENU_ENTRY_SIZE = 19,
    MENU_LABEL_OFFSET = 3,
    MENU_LABEL_SIZE = 16,
    SCENE_TABLE = 0xC2001C,  /* long pointers to each scene's script, by scene number */
    WAIT_FOR_BUTTON = 0xFFFF,
};

typedef struct ScriptItem {
    uint32 addr;      /* address of this item */
    uint32 next;      /* address of what follows */
    int op;           /* opcode, or -1 for one character of text */
} ScriptItem;

/* Decode what's at addr. */
void ScriptStep(uint32 addr, ScriptItem *item);

/* A scene's first address, or 0 past the end of the table. */
uint32 ScriptScene(int scene);
