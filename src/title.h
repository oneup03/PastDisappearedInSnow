#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "types.h"

/* A part's image has loaded. The session's first part gets a title screen that waits. */
void TitleOnImageLoaded(int part, bool first);

/* While the title waits: the clock holds, and START or a click carries on. */
bool TitleIsWaiting(void);
void TitleOnFrame(uint8 *ram);

/* A save state was loaded over it. */
void TitleCancel(void);

/* SnesDesktopHostGame.publish_native_field. */
void TitlePublishField(uint8_t *field, unsigned number);
