#pragma once
#include <stdbool.h>

#include "audio_reader.h"

/* Takes the original player's sounds off a track that plays only once, or that the passes can't
 * clean on their own. See src/loop_clean.c. */

enum { CLEAN_PITCHES = 6, CLEAN_MOST_LAGS = 16 };

/* One of the player's sounds, from data/cues/sounds.toml: how long it's heard, in seconds, and its
 * strongest pitches with each one's level under the strongest. */
typedef struct CleanSound {
    char name[8];
    double core;
    bool typing;
    int count;
    float hz[CLEAN_PITCHES];
    float db[CLEAN_PITCHES];
} CleanSound;

typedef struct CleanSounds {
    CleanSound *sounds;  /* for the filter, in the order they're listed */
    int count;
    CleanSound *longs;   /* longer than the filter's window: the alarm */
    int long_count;
} CleanSounds;

typedef struct CleanPlan {
    bool filter;                        /* the one-take filter */
    bool long_sounds;                   /* the long sounds, like the alarm: with the filter or alone */
    double copies[CLEAN_MOST_LAGS];     /* seconds to where the same music plays again exactly */
    int copy_count;
    double bars[CLEAN_MOST_LAGS];       /* seconds to where its bars come round: alike, not the same */
    int bar_count;
    const CleanSounds *sounds;
} CleanPlan;

/* Where the track repeats within its own recording: pitch by pitch, a moment that stands clearly
 * above its copy reads the copy, and one that briefly stands over its bars comes down to them.
 * `audio` is the loop, interleaved, from `source` seconds into `from`. False if it gave up. */
bool CleanCopies(const CleanPlan *plan, float *audio, long samples, int rate, const Recording *from, double source,
                 const float *gain, bool (*stopping)(void));

/* The filter and then the long sounds, all the way round the loop. False if it gave up. */
bool CleanFilter(const CleanPlan *plan, float *audio, long samples, int rate, bool (*stopping)(void));
