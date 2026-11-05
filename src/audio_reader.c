/* See audio_reader.h. */
#define MINIMP3_IMPLEMENTATION
#include "audio_reader.h"

#include <math.h>
#include <string.h>

enum { SEEK_WARMUP_FRAMES = 6 };  /* the bit reservoir reaches back into earlier frames */

static const double kPi = 3.14159265358979323846;

static int DecodeFrame(Reader *r, const Recording *from, int frame, int16 *pcm) {
    mp3dec_frame_info_t info;
    uint32 start = from->offsets[frame];
    uint32 end = frame + 1 < from->frame_count ? from->offsets[frame + 1] : start;
    return mp3dec_decode_frame(&r->decoder, from->data + start, (int)(end - start), pcm, &info);
}

static int GreatestDivisor(int a, int b) { return b ? GreatestDivisor(b, a % b) : a; }

static void PrepareResampler(Reader *r, int from_rate, int to_rate) {
    int divisor = GreatestDivisor(from_rate, to_rate);
    int up = to_rate / divisor, down = from_rate / divisor;
    if (up == r->kernel_up && down == r->kernel_down)
        return;
    r->kernel_up = up;
    r->kernel_down = down;
    /* A windowed sinc: its cutoff is the lower Nyquist, in cycles per input sample. */
    double cutoff = 0.5 * (up < down ? (double)up / down : 1.0) * 0.97;
    for (int phase = 0; phase < up; phase++) {
        double sum = 0;
        for (int tap = 0; tap < READER_RESAMPLE_TAPS; tap++) {
            double x = tap - (READER_RESAMPLE_TAPS / 2 - 1) - (double)phase / up;
            double w = x / (READER_RESAMPLE_TAPS / 2);
            double sinc = fabs(x) < 1e-9 ? 2 * cutoff : sin(2 * kPi * cutoff * x) / (kPi * x);
            double window = fabs(w) >= 1 ? 0 : 0.42 + 0.5 * cos(kPi * w) + 0.08 * cos(2 * kPi * w);
            r->kernel[phase][tap] = (float)(sinc * window);
            sum += r->kernel[phase][tap];
        }
        for (int tap = 0; tap < READER_RESAMPLE_TAPS; tap++)
            r->kernel[phase][tap] = (float)(r->kernel[phase][tap] / sum);
    }
}

static void AppendInput(Reader *r, const int16 *pcm, int samples) {
    /* Keep only what the filter can still reach. After a seek that may be nothing so far. */
    int keep_from = r->input_at - (READER_RESAMPLE_TAPS / 2 - 1);
    if (keep_from > r->input_count)
        keep_from = r->input_count;
    if (keep_from > 0) {
        for (int c = 0; c < READER_CHANNELS; c++)
            memmove(r->input[c], r->input[c] + keep_from, sizeof(float) * (size_t)(r->input_count - keep_from));
        r->input_count -= keep_from;
        r->input_at -= keep_from;
    }
    if (r->input_count + samples > READER_INPUT_SAMPLES)
        samples = READER_INPUT_SAMPLES - r->input_count;
    for (int i = 0; i < samples; i++) {
        for (int c = 0; c < READER_CHANNELS; c++)
            r->input[c][r->input_count + i] = pcm[i * READER_CHANNELS + c];
    }
    r->input_count += samples;
}

/* Everything the input can give so far, into ready. */
static void Resample(Reader *r) {
    r->ready_at = 0;
    r->ready_count = 0;
    while (r->input_at + READER_RESAMPLE_TAPS / 2 < r->input_count && r->ready_count < READER_READY_SAMPLES) {
        const float *kernel = r->kernel[r->input_phase];
        for (int c = 0; c < READER_CHANNELS; c++) {
            const float *in = r->input[c] + r->input_at - (READER_RESAMPLE_TAPS / 2 - 1);
            float sum = 0;
            for (int tap = 0; tap < READER_RESAMPLE_TAPS; tap++)
                sum += kernel[tap] * in[tap];
            sum = sum > 32767.0f ? 32767.0f : sum < -32768.0f ? -32768.0f : sum;
            r->ready[r->ready_count * READER_CHANNELS + c] = (int16)lrintf(sum);
        }
        r->ready_count++;
        r->input_phase += r->kernel_down;
        r->input_at += r->input_phase / r->kernel_up;
        r->input_phase %= r->kernel_up;
    }
}

void ReaderReset(Reader *r) {
    r->reading = NULL;
    r->ready_count = 0;
    r->next_frame = -1;
}

void ReaderSeek(Reader *r, const Recording *from, int rate, double media) {
    long position = lround(media * rate);
    if (from == r->reading && rate == r->rate && position == r->position)
        return;
    long sample = lround(media * from->sample_rate);
    if (sample < 0)
        sample = 0;
    int frame = (int)(sample / READER_FRAME_SAMPLES);
    bool other_rate = from->sample_rate != rate;
    r->reading = from;
    r->rate = rate;
    r->position = position;
    r->ready_count = 0;
    r->skip_samples = (int)(sample % READER_FRAME_SAMPLES);
    r->next_frame = frame;
    mp3dec_init(&r->decoder);
    if (other_rate) {
        PrepareResampler(r, from->sample_rate, rate);
        memset(r->input, 0, sizeof(r->input));
        r->input_count = READER_RESAMPLE_TAPS;
        r->input_at = READER_RESAMPLE_TAPS - 1;
        r->input_phase = 0;
    }
    int16 scratch[MINIMP3_MAX_SAMPLES_PER_FRAME];
    for (int warm = frame - SEEK_WARMUP_FRAMES; warm < frame; warm++) {
        if (warm < 0 || warm >= from->frame_count)
            continue;
        int samples = DecodeFrame(r, from, warm, scratch);
        if (other_rate && warm == frame - 1 && samples > 0) {
            AppendInput(r, scratch, samples);
            r->input_at = r->input_count;
        }
    }
    if (other_rate) {
        r->input_at += r->skip_samples;
        r->skip_samples = 0;
    }
}

/* Decode the next frame into ready. False if there's nothing more to read. */
static bool DecodeMore(Reader *r) {
    if (!r->reading || r->next_frame < 0 || r->next_frame >= r->reading->frame_count)
        return false;
    int16 pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int samples = DecodeFrame(r, r->reading, r->next_frame++, pcm);
    if (samples <= 0)
        return true;
    if (r->reading->sample_rate != r->rate) {
        AppendInput(r, pcm, samples);
        Resample(r);
        return true;
    }
    int skip = r->skip_samples < samples ? r->skip_samples : samples;
    r->skip_samples -= skip;
    r->ready_at = 0;
    r->ready_count = samples - skip;
    memcpy(r->ready, pcm + skip * READER_CHANNELS, (size_t)(r->ready_count * READER_CHANNELS) * sizeof(int16));
    return true;
}

int ReaderRead(Reader *r, int16 **pcm, int most) {
    while (r->ready_count == 0) {
        if (!DecodeMore(r))
            return 0;
    }
    int take = r->ready_count < most ? r->ready_count : most;
    *pcm = r->ready + r->ready_at * READER_CHANNELS;
    r->position += take;
    r->ready_at += take;
    r->ready_count -= take;
    return take;
}
