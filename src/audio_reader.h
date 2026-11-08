#pragma once
#include <stdbool.h>

#include "minimp3.h"
#include "types.h"

/* Reads a recording from any place in it, at whatever rate it's wanted. Each reader has its
 * own decoder, so more than one can run at a time. */

enum {
    READER_CHANNELS = 2,
    READER_FRAME_SAMPLES = 1152,
    READER_READY_SAMPLES = READER_FRAME_SAMPLES * 2,
    READER_RESAMPLE_TAPS = 32,
    READER_RESAMPLE_PHASES = 3,  /* the recordings are at 32 and 48 kHz, so 2:3 or 3:2 */
    READER_INPUT_SAMPLES = READER_FRAME_SAMPLES * 2 + READER_RESAMPLE_TAPS,
};

typedef struct Recording {
    uint8 *data;
    uint32 *offsets;  /* where each MP3 frame starts */
    int frame_count;
    int sample_rate;
} Recording;

typedef struct Reader {
    mp3dec_t decoder;
    const Recording *reading;
    int next_frame;
    int skip_samples;
    int rate;             /* of what's read: the recording's own, or it's resampled */
    long position;        /* in samples of that rate, where the next sample is from */
    int16 ready[READER_READY_SAMPLES * READER_CHANNELS];
    int ready_at, ready_count;
    float input[READER_CHANNELS][READER_INPUT_SAMPLES];
    int input_count, input_at, input_phase;
    float kernel[READER_RESAMPLE_PHASES][READER_RESAMPLE_TAPS];
    int kernel_up, kernel_down;
} Reader;

/* Read a whole MP3 file into memory and index its frames. Leading ID3 tags are skipped. */
bool RecordingLoad(Recording *r, const char *path);

/* Forget where it is. Reading then needs a seek first. */
void ReaderReset(Reader *r);

/* Go to a time in a recording, unless that's already where it is. */
void ReaderSeek(Reader *r, const Recording *from, int rate, double media);

/* Up to `most` samples, in the reader's own buffer, which the caller may change. 0 at the end. */
int ReaderRead(Reader *r, int16 **pcm, int most);
