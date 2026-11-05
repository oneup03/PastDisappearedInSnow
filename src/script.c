#include "script.h"

#include "common_rtl.h"

static const uint8 kOperandSize[0x20] = {
    0, 0, 0, 2, 2, 3, 0, 2, 0, 0, 0, 0, 2, 0, 2, 2,
    3, 2, 5, 0, 0, 1, 4, 6, 6, 3, 4, 6, 1, 1, 0, 0,
};

void ScriptStep(uint32 addr, ScriptItem *item) {
    uint8 byte = *RomPtr(addr);
    item->addr = addr;
    if (byte >= 0x20) {
        item->op = -1;
        item->next = addr + ((byte >= 0x80 && !(byte >= 0xA0 && byte < 0xE0)) ? 2 : 1);
        return;
    }
    item->op = byte;
    if (byte == OP_STRING) {
        uint32 at = addr + 4;
        while (*RomPtr(at++) != 0) {}
        item->next = at;
    } else if (byte == OP_MENU) {
        item->next = addr + 2 + *RomPtr(addr + 1) * MENU_ENTRY_SIZE;
    } else {
        item->next = addr + 1 + kOperandSize[byte];
    }
}

uint32 ScriptScene(int scene) {
    const uint8 *entry = RomPtr(SCENE_TABLE + scene * 3);
    if (entry[0] == 0xFF && entry[1] == 0xFF)
        return 0;
    return entry[0] | entry[1] << 8 | (uint32)entry[2] << 16;
}
