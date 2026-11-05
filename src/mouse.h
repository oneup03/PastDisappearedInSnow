#pragma once
#include "cpu_state.h"
#include "display_aspect.h"
#include "types.h"

/* Call from the clock poll, which the game runs straight after its input poll. */
void MouseOnFrame(uint8 *ram);

/* Where the picture sits in the window, in drawable pixels. */
void MouseSetViewport(int frame_w, int frame_h, const SnesDisplayViewport *viewport);
