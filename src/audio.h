#pragma once
#include <stdbool.h>

#include "types.h"

/* The broadcast's radio audio, from the player's recordings. See src/audio.c. */

/* Pull audio/<part>.mp3 out of any recordings beside the launched ROM. */
void AudioImportRecordings(const char *search_dir, const char *audio_dir);

/* Choose the audio folder. No file there means ROM sound only. */
void AudioSetDirectory(const char *audio_dir);

/* Once per frame, at the frame boundary. */
void AudioOnFrame(const uint8 *ram, int part, bool clock_held, bool turbo);

/* Whether the game's own sound should stay silent right now, because the recording
 * already has it. Music always is. Effects are in the recording during scripted scenes;
 * when the player is acting, the recorded player's are someone else's actions. */
bool AudioRecordingCoversMusic(const uint8 *ram);
bool AudioRecordingCoversEffects(const uint8 *ram);

/* "recording" or "rom". */
void AudioSetSource(const char *source);

void AudioShutdown(void);
