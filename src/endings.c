/* Parts 1 and 2 as acts of one story. On the air, each ended with the same cast list, a few
 * days apart. In one sitting that stops the story for over a minute to repeat itself, so the
 * list only runs at the end of part 3, which credits everyone.
 *
 * The script jumps over the list, and the deadline and the next scene move up by as long as it
 * took, leaving the clock alone. So the ending theme in the recording carries on unbroken under
 * the score and the teaser. Applied to the image in memory, so a save holds no trace of it.
 */
#include <stdio.h>
#include <string.h>

#include "common_rtl.h"
#include "endings.h"
#include "session.h"

typedef struct Patch {
    int part;
    uint32 addr;  /* file offset */
    uint8 size;
    uint8 before[4], after[4];
} Patch;

static const Patch kPatches[] = {
    /* Zenpen: over the list to where the score shows (the space keeps the pre-scan in step),
     * the fade at scene second 78, and the teaser's scene at clock 3408. */
    {PART_ZENPEN, 0x35F25, 4, {0x19, 0x34, 0x7A, 0xC0}, {0x11, 0xA5, 0x61, 0x20}},
    {PART_ZENPEN, 0x361B3, 2, {0x96, 0x00}, {78, 0x00}},
    {PART_ZENPEN, 0x361FB, 2, {0x98, 0x0D}, {0x50, 0x0D}},
    /* Chuuhen: the same, with the fade at scene second 80 and the teaser at clock 3418. */
    {PART_CHUUHEN, 0x3C712, 4, {0x19, 0xF4, 0x78, 0xC0}, {0x11, 0x92, 0xC9, 0x20}},
    {PART_CHUUHEN, 0x3C9A0, 2, {0x90, 0x00}, {80, 0x00}},
    {PART_CHUUHEN, 0x3C9E8, 2, {0x98, 0x0D}, {0x5A, 0x0D}},
};

void EndingsOnImageLoaded(int part) {
    for (size_t i = 0; i < sizeof(kPatches) / sizeof(kPatches[0]); i++) {
        const Patch *patch = &kPatches[i];
        if (patch->part != part)
            continue;
        uint8 *bytes = RomPtr(0xC00000 | patch->addr);
        if (memcmp(bytes, patch->before, patch->size) != 0) {
            fprintf(stderr, "[endings] part %d at %06X isn't as expected, leaving it\n", part, patch->addr);
            continue;
        }
        memcpy(bytes, patch->after, patch->size);
    }
}
