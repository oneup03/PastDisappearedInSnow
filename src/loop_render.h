#pragma once
#include <stdbool.h>

#include "audio_reader.h"

/* Renders one loop's clean pass from the player's recordings. See src/loop_render.c. */

typedef struct RenderTake {
    double phase, length, source;  /* seconds: this stretch of the loop reads source + phase */
    const Recording *from;
    float gain[READER_CHANNELS];
} RenderTake;

/* Another pass of the track. Over stretches of the loop it lines up at phase + offset. */
typedef struct RenderSegment {
    double phase, offset;
} RenderSegment;

typedef struct RenderPass {
    const Recording *from;
    float gain[READER_CHANNELS];
    const RenderSegment *segments;
    int segment_count;
} RenderPass;

/* A known sound on the pass: over this stretch, everything low and these lines higher up come
 * from whichever pass is quieter there. */
typedef struct RenderPatch {
    double from, to;
    const float *lines;
    int line_count;
} RenderPatch;

typedef struct RenderPlan {
    int rate;
    long samples;
    const RenderTake *takes;
    int take_count;
    const RenderPass *passes;
    int pass_count;
    double fade;  /* seconds over which the end crosses into what leads up to the start, or 0 */
    const RenderPatch *patches;  /* in time order. With these, only these get repaired */
    int patch_count;
} RenderPlan;

/* The clean pass, as interleaved samples to free, or NULL if `stopping` says to give up. */
int16 *LoopRender(const RenderPlan *plan, bool (*stopping)(void));
