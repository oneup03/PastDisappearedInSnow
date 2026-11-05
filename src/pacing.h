#pragma once
#include <stdbool.h>

#include "types.h"

/* How the broadcast clock behaves. See src/pacing.c. */
typedef enum {
    PACING_REAL_TIME,
    PACING_UNLIMITED,
} PacingMode;

void PacingSetMode(PacingMode mode);
PacingMode PacingGetMode(void);

/* Runs once per frame after the game has read the pad, before the clocks tick.
 * Returns true if the clocks must not tick this frame. */
bool PacingOnFrame(uint8 *ram);

/* Whether the current scene lets the player act. */
bool PacingSceneHasActions(const uint8 *ram);

/* The answer PacingOnFrame gave for the current frame. */
bool PacingIsHolding(void);
