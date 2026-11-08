#pragma once
#include <stdbool.h>

/* Soundtrack albums in place of some of the broadcast's investigation music. See src/soundtrack.c. */

/* Where the player keeps albums: any folders under it. */
void SoundtrackSetDirectory(const char *dir);

/* "recording" (none), "sfc" or "remaster". */
void SoundtrackSetVersion(const char *version);

/* Whether a loop, by its number in the part's cue file, has a version the player has. */
bool SoundtrackReplaces(int part, int loop);

/* Once a frame: the loop whose version should play, or -1 for none. Fades in and out, and
 * returns how far in it is, 0 to 1, for the recording to fade the other way. */
float SoundtrackOnFrame(int part, int loop);

/* Stop at once, as for fast-forward. */
void SoundtrackStop(void);

void SoundtrackShutdown(void);
