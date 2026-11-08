/* The clean pass through a looping track.
 *
 * The takes in the cue file already avoid the original player's sound effects wherever some
 * pass of the track is clean. Sometimes every pass has something on it at that moment. But
 * typing tones are narrow, and rarely in the same place at the same time, so this works in
 * time and frequency: where the pass stands well above the median of all the passes at one
 * point, that point comes from the pass nearest the median. Very little is touched, and
 * everything else stays exactly as the takes have it.
 *
 * Over birds, the passes never agree, so standing out says nothing. There the cue file lists each
 * of the player's sounds, and only those moments and frequencies get touched.
 *
 * A track that never truly repeats gets a fade instead: its end crosses into what leads up to
 * its start, on the same beat, so it goes round without a seam. With only one take, the player's
 * sounds come off it by other means: see src/loop_clean.c.
 *
 * This reads several recordings at once and takes a few seconds a loop, so a background
 * thread does it, once, and keeps the result beside the player's recordings.
 */
#include "loop_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    FFT_SIZE = 1024,
    FFT_HOP = 256,
    BINS = FFT_SIZE / 2 + 1,
    CHANNELS = READER_CHANNELS,
    STOP_CHECK_FRAMES = 256,
    SMOOTH_SAMPLES = 64,
};

static const float kStandsOut = 1.8f;           /* over the median: about 5 dB */
static const float kQuietest = 0.0007943f * FFT_SIZE / 4;  /* -62 dB: don't touch what can't be heard */
static const double kLowestHz = 300.0, kHighestHz = 9000.0;
static const double kPatchLowestHz = 150.0, kPatchTopHz = 2300.0;  /* the birds are over this */
enum { PATCH_LINE_REACH = 2 };  /* bins each way: a short sound is a wide line */
static const float kWindowSum = 2.0f;           /* of the window squared, at this hop */
static const double kPi = 3.14159265358979323846;

typedef struct Complex {
    float re, im;
} Complex;

static Complex g_twiddle[FFT_SIZE / 2];
static float g_window[FFT_SIZE];
static int g_reversed[FFT_SIZE];
static bool g_tables_ready;

static void PrepareTables(void) {
    if (g_tables_ready)
        return;
    int bits = 0;
    while ((1 << bits) < FFT_SIZE)
        bits++;
    for (int i = 0; i < FFT_SIZE; i++) {
        int reversed = 0;
        for (int b = 0; b < bits; b++)
            reversed |= ((i >> b) & 1) << (bits - 1 - b);
        g_reversed[i] = reversed;
        g_window[i] = (float)sqrt(0.5 - 0.5 * cos(2 * kPi * i / FFT_SIZE));
    }
    for (int i = 0; i < FFT_SIZE / 2; i++)
        g_twiddle[i] = (Complex){(float)cos(2 * kPi * i / FFT_SIZE), (float)-sin(2 * kPi * i / FFT_SIZE)};
    g_tables_ready = true;
}

static void Fft(Complex *x) {
    for (int i = 0; i < FFT_SIZE; i++) {
        if (i < g_reversed[i]) {
            Complex swap = x[i];
            x[i] = x[g_reversed[i]];
            x[g_reversed[i]] = swap;
        }
    }
    for (int size = 2; size <= FFT_SIZE; size *= 2) {
        int half = size / 2, step = FFT_SIZE / size;
        for (int start = 0; start < FFT_SIZE; start += size) {
            for (int i = 0; i < half; i++) {
                Complex w = g_twiddle[i * step], *a = &x[start + i], *b = &x[start + i + half];
                Complex t = {b->re * w.re - b->im * w.im, b->re * w.im + b->im * w.re};
                b->re = a->re - t.re;
                b->im = a->im - t.im;
                a->re += t.re;
                a->im += t.im;
            }
        }
    }
}

static void InverseFft(Complex *x) {
    for (int i = 0; i < FFT_SIZE; i++)
        x[i].im = -x[i].im;
    Fft(x);
    for (int i = 0; i < FFT_SIZE; i++) {
        x[i].re /= FFT_SIZE;
        x[i].im = -x[i].im / FFT_SIZE;
    }
}

/* Two real signals in one transform: x = left + i right. Each spectrum comes back out. */
static inline Complex LeftOf(const Complex *x, int bin) {
    const Complex a = x[bin], b = x[(FFT_SIZE - bin) % FFT_SIZE];
    return (Complex){(a.re + b.re) / 2, (a.im - b.im) / 2};
}

static inline Complex RightOf(const Complex *x, int bin) {
    const Complex a = x[bin], b = x[(FFT_SIZE - bin) % FFT_SIZE];
    return (Complex){(a.im + b.im) / 2, (b.re - a.re) / 2};
}

/* Read samples from a recording at a rate, into interleaved floats or samples. */
static bool ReadSpan(Reader *reader, const Recording *from, int rate, double media, long count, int16 *out) {
    ReaderSeek(reader, from, rate, media);
    for (long done = 0; done < count;) {
        int16 *pcm;
        long want = count - done;
        int got = ReaderRead(reader, &pcm, want > READER_READY_SAMPLES ? READER_READY_SAMPLES : (int)want);
        if (!got) {
            memset(out + done * CHANNELS, 0, (size_t)((count - done) * CHANNELS) * sizeof(int16));
            return false;
        }
        memcpy(out + done * CHANNELS, pcm, (size_t)(got * CHANNELS) * sizeof(int16));
        done += got;
    }
    return true;
}

/* A pass lined up with the loop, one sample per phase sample, running on past the end. A
 * segment whose offset isn't a number is where the pass isn't this track: a section the
 * broadcast played in another order there. Those frames are marked in `usable`, one per hop. */
static int16 *ReadPass(Reader *reader, const RenderPass *pass, int rate, long count, bool *usable, long frames) {
    double lowest = INFINITY, highest = -INFINITY;
    for (int i = 0; i < pass->segment_count; i++) {
        if (isnan(pass->segments[i].offset))
            continue;
        if (pass->segments[i].offset < lowest)
            lowest = pass->segments[i].offset;
        if (pass->segments[i].offset > highest)
            highest = pass->segments[i].offset;
    }
    if (!isfinite(lowest))
        lowest = highest = 0;
    long extra = lround((highest - lowest) * rate) + 2;
    int16 *span = (int16 *)malloc((size_t)((count + extra) * CHANNELS) * sizeof(int16));
    int16 *lined_up = (int16 *)malloc((size_t)(count * CHANNELS) * sizeof(int16));
    if (!span || !lined_up) {
        free(span);
        free(lined_up);
        return NULL;
    }
    ReaderReset(reader);
    ReadSpan(reader, pass->from, rate, lowest, count + extra, span);
    for (long f = 0; f < frames; f++)
        usable[f] = true;
    for (int i = 0; i < pass->segment_count; i++) {
        long from = lround(pass->segments[i].phase * rate);
        long to = i + 1 < pass->segment_count ? lround(pass->segments[i + 1].phase * rate) : count;
        if (to > count)
            to = count;
        if (to <= from)
            continue;
        if (isnan(pass->segments[i].offset)) {
            memset(lined_up + from * CHANNELS, 0, (size_t)((to - from) * CHANNELS) * sizeof(int16));
            /* Every frame that reaches into it. */
            long first = from >= FFT_SIZE ? (from - FFT_SIZE) / FFT_HOP + 1 : 0;
            for (long f = first; f < frames && f * FFT_HOP < to; f++)
                usable[f] = false;
            continue;
        }
        long shift = lround((pass->segments[i].offset - lowest) * rate);
        memcpy(lined_up + from * CHANNELS, span + (from + shift) * CHANNELS, (size_t)((to - from) * CHANNELS) * sizeof(int16));
    }
    free(span);
    return lined_up;
}

/* Where two stretches meet, ease out the step between them over the samples before it. They've
 * been chosen to nearly coincide, but it costs nothing to leave no step at all. The sample after
 * the end of the loop is its first, so this does the wrap too. */
static void EaseStep(float *audio, long length, long at) {
    if (at < SMOOTH_SAMPLES + 1)
        return;
    for (int c = 0; c < CHANNELS; c++) {
        const float *x = audio + c;
        float next = x[(at % length) * CHANNELS], last = x[(at - 1) * CHANNELS], before = x[(at - 2) * CHANNELS];
        float step = (next - last) - (last - before);
        for (int i = 0; i < SMOOTH_SAMPLES; i++) {
            float amount = 0.5f - 0.5f * (float)cos(kPi * (i + 1) / SMOOTH_SAMPLES);
            audio[(at - SMOOTH_SAMPLES + i) * CHANNELS + c] += step * amount;
        }
    }
}

/* A tone as a gain for each bin: smooth between the middles of the bands below 11 kHz, and
 * the top band on its own above, as that's where a lower bitrate simply stops. */
static void ToneGains(const float *tone, int rate, float *gain) {
    static const double kMiddles[TONE_BANDS - 1] = {42, 85, 170, 340, 680, 1360, 2720, 5430, 9190};
    const double top = 11000.0, edge = 500.0;
    for (int b = 0; b < BINS; b++) {
        double hz = (double)b * rate / FFT_SIZE, db;
        if (hz <= kMiddles[0]) {
            db = tone[0];
        } else if (hz >= kMiddles[TONE_BANDS - 2]) {
            db = tone[TONE_BANDS - 2];
        } else {
            int band = 0;
            while (hz > kMiddles[band + 1])
                band++;
            double along = log(hz / kMiddles[band]) / log(kMiddles[band + 1] / kMiddles[band]);
            db = tone[band] + (tone[band + 1] - tone[band]) * along;
        }
        double below = pow(10.0, db / 20), above = pow(10.0, tone[TONE_BANDS - 1] / 20);
        double share = hz <= top - edge ? 0.0 : hz >= top + edge ? 1.0 : (hz - (top - edge)) / (2 * edge);
        gain[b] = (float)(below * (1 - share) + above * share);
    }
}

/* Filter both channels by a gain per bin, the same zero-phase filter for each. */
static void FilterSpan(const int16 *in, long count, const float *gain, float *out) {
    memset(out, 0, sizeof(float) * (size_t)(count * CHANNELS));
    for (long start = FFT_HOP - FFT_SIZE; start < count; start += FFT_HOP) {
        Complex x[FFT_SIZE];
        for (int n = 0; n < FFT_SIZE; n++) {
            long at = start + n;
            bool inside = at >= 0 && at < count;
            x[n] = (Complex){inside ? in[at * CHANNELS] / 32768.0f * g_window[n] : 0.0f,
                             inside ? in[at * CHANNELS + 1] / 32768.0f * g_window[n] : 0.0f};
        }
        Fft(x);
        for (int b = 0; b < FFT_SIZE; b++) {
            float g = gain[b <= FFT_SIZE / 2 ? b : FFT_SIZE - b];
            x[b].re *= g;
            x[b].im *= g;
        }
        InverseFft(x);
        for (int n = 0; n < FFT_SIZE; n++) {
            long at = start + n;
            if (at >= 0 && at < count) {
                out[at * CHANNELS] += x[n].re * g_window[n] / kWindowSum;
                out[at * CHANNELS + 1] += x[n].im * g_window[n] / kWindowSum;
            }
        }
    }
}

/* What the takes make, as floats from -1 to 1. A take from a recording with another tone is
 * read with some to spare either side, so its filter has settled where it starts and ends. */
static float *RenderTakes(Reader *reader, const RenderPlan *plan) {
    float *base = (float *)calloc((size_t)(plan->samples * CHANNELS), sizeof(float));
    int16 *scratch = (int16 *)malloc((size_t)((plan->samples + 2 * FFT_SIZE) * CHANNELS) * sizeof(int16));
    float *filtered = (float *)malloc(sizeof(float) * (size_t)((plan->samples + 2 * FFT_SIZE) * CHANNELS));
    if (!base || !scratch || !filtered) {
        free(base);
        free(scratch);
        free(filtered);
        return NULL;
    }
    float gain[BINS];
    const float *tone_of_gain = NULL;
    for (int i = 0; i < plan->take_count; i++) {
        const RenderTake *take = &plan->takes[i];
        long from = lround(take->phase * plan->rate);
        long to = i + 1 < plan->take_count ? lround(plan->takes[i + 1].phase * plan->rate) : plan->samples;
        if (to > plan->samples)
            to = plan->samples;
        if (to <= from)
            continue;
        double media = take->source + take->phase;
        if (!take->tone) {
            ReadSpan(reader, take->from, plan->rate, media, to - from, scratch);
            for (long n = 0; n < (to - from) * CHANNELS; n++)
                base[from * CHANNELS + n] = scratch[n] / 32768.0f * take->gain[n % CHANNELS];
            continue;
        }
        long before = FFT_SIZE;
        if (media * plan->rate < before)
            before = (long)(media * plan->rate);
        long count = to - from + before + FFT_SIZE;
        ReadSpan(reader, take->from, plan->rate, media - (double)before / plan->rate, count, scratch);
        if (take->tone != tone_of_gain) {
            ToneGains(take->tone, plan->rate, gain);
            tone_of_gain = take->tone;
        }
        FilterSpan(scratch, count, gain, filtered);
        for (long n = 0; n < (to - from) * CHANNELS; n++)
            base[from * CHANNELS + n] = filtered[before * CHANNELS + n] * take->gain[n % CHANNELS];
    }
    free(scratch);
    free(filtered);
    for (int i = 1; i < plan->take_count; i++)
        EaseStep(base, plan->samples, lround(plan->takes[i].phase * plan->rate));
    return base;
}

/* Over the last stretch, fade from what's there to the audio leading up to the first take. */
static bool CrossIntoStart(Reader *reader, const RenderPlan *plan, float *base) {
    long samples = lround(plan->fade * plan->rate);
    if (samples > plan->samples)
        samples = plan->samples;
    int16 *before = (int16 *)malloc((size_t)(samples * CHANNELS) * sizeof(int16));
    if (!before)
        return false;
    const RenderTake *first = &plan->takes[0];
    ReaderReset(reader);
    ReadSpan(reader, first->from, plan->rate, first->source + first->phase - plan->fade, samples, before);
    float *end = base + (plan->samples - samples) * CHANNELS;
    for (long i = 0; i < samples; i++) {
        double along = (i + 0.5) / samples * kPi / 2;
        for (int c = 0; c < CHANNELS; c++) {
            float incoming = before[i * CHANNELS + c] / 32768.0f * first->gain[c];
            end[i * CHANNELS + c] = (float)(end[i * CHANNELS + c] * cos(along) + incoming * sin(along));
        }
    }
    free(before);
    return true;
}

/* Which bins the patches over this frame cover. False if there are none. */
static bool PatchedBins(const RenderPlan *plan, double from, double to, int *first_patch, bool *bins) {
    while (*first_patch < plan->patch_count && plan->patches[*first_patch].to <= from)
        (*first_patch)++;
    bool any = false;
    const double per_bin = (double)plan->rate / FFT_SIZE;
    memset(bins, 0, sizeof(bool) * BINS);
    /* They overlap, so one that's over may still be followed by one that isn't. */
    for (int i = *first_patch; i < plan->patch_count && plan->patches[i].from < to; i++) {
        const RenderPatch *patch = &plan->patches[i];
        if (patch->to <= from)
            continue;
        if (!any) {
            for (int b = (int)ceil(kPatchLowestHz / per_bin); b <= (int)(kPatchTopHz / per_bin); b++)
                bins[b] = true;
        }
        any = true;
        for (int l = 0; l < patch->line_count; l++) {
            int centre = (int)lround(patch->lines[l] / per_bin);
            for (int b = centre - PATCH_LINE_REACH; b <= centre + PATCH_LINE_REACH; b++) {
                if (b > 0 && b < BINS - 1)
                    bins[b] = true;
            }
        }
    }
    return any;
}

static int CompareFloats(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

int16 *LoopRender(const RenderPlan *plan, bool (*stopping)(void)) {
    PrepareTables();
    int16 *result = NULL;
    const int count = plan->pass_count;
    const long length = plan->samples;
    Reader *reader = (Reader *)calloc(1, sizeof(Reader));
    int16 **passes = (int16 **)calloc((size_t)(count ? count : 1), sizeof(int16 *));
    long frames = count >= 2 ? length / FFT_HOP : 0;  /* it takes several passes to tell */
    bool *usable = (bool *)malloc(sizeof(bool) * (size_t)(count ? count : 1) * (size_t)(frames ? frames : 1));
    Complex *spectra = (Complex *)malloc(sizeof(Complex) * FFT_SIZE * (size_t)(count ? count : 1));
    float *magnitude = (float *)malloc(sizeof(float) * BINS * (size_t)(count ? count : 1));
    float *base = NULL, *change = NULL;
    if (!reader || !passes || !spectra || !magnitude || !usable)
        goto done;
    ReaderReset(reader);
    base = RenderTakes(reader, plan);
    change = (float *)calloc((size_t)(length * CHANNELS), sizeof(float));
    if (!base || !change)
        goto done;
    for (int i = 0; i < count; i++) {
        if (stopping())
            goto done;
        passes[i] = ReadPass(reader, &plan->passes[i], plan->rate, length + FFT_SIZE, usable + (size_t)i * (size_t)frames, frames);
        if (!passes[i])
            goto done;
    }
    int lowest_bin = (int)ceil(kLowestHz * FFT_SIZE / plan->rate);
    int highest_bin = (int)ceil(kHighestHz * FFT_SIZE / plan->rate);
    if (highest_bin > BINS - 1)
        highest_bin = BINS - 1;
    int first_patch = 0;
    for (long frame = 0; frame < frames; frame++) {
        if (frame % STOP_CHECK_FRAMES == 0 && stopping())
            goto done;
        long start = frame * FFT_HOP;
        bool kept_as_is = false;
        for (int i = 0; i < plan->as_is_count && !kept_as_is; i++) {
            long low = lround(plan->as_is[i].from * plan->rate), high = lround(plan->as_is[i].to * plan->rate);
            kept_as_is = start < high && start + FFT_SIZE > low;
        }
        if (kept_as_is)
            continue;
        bool patched[BINS];
        if (plan->patch_count) {
            double from = (double)start / plan->rate;
            if (!PatchedBins(plan, from, from + (double)FFT_SIZE / plan->rate, &first_patch, patched))
                continue;
            lowest_bin = 1;
            highest_bin = BINS - 1;
        }
        Complex base_spectrum[FFT_SIZE], out[FFT_SIZE];
        /* The frame wraps round the end of the loop, as playing does. */
        for (int n = 0; n < FFT_SIZE; n++) {
            const float *at = base + ((start + n) % length) * CHANNELS;
            base_spectrum[n] = (Complex){at[0] * g_window[n], at[1] * g_window[n]};
        }
        /* Only the passes that are this track over the whole frame. */
        int present_count = 0, present[32];
        for (int i = 0; i < count && present_count < 32; i++) {
            if (usable[(size_t)i * (size_t)frames + (size_t)frame])
                present[present_count++] = i;
        }
        if (present_count < 2)
            continue;
        Fft(base_spectrum);
        for (int k = 0; k < present_count; k++) {
            const int i = present[k];
            Complex *x = spectra + (size_t)i * FFT_SIZE;
            const int16 *at = passes[i] + start * CHANNELS;
            const float *gain = plan->passes[i].gain;
            for (int n = 0; n < FFT_SIZE; n++) {
                x[n] = (Complex){at[n * CHANNELS] / 32768.0f * gain[0] * g_window[n],
                                 at[n * CHANNELS + 1] / 32768.0f * gain[1] * g_window[n]};
            }
            Fft(x);
            float *m = magnitude + (size_t)i * BINS;
            for (int b = lowest_bin; b < highest_bin; b++) {
                Complex l = LeftOf(x, b), r = RightOf(x, b);
                float re = (l.re + r.re) / 2, im = (l.im + r.im) / 2;
                m[b] = sqrtf(re * re + im * im);
            }
        }
        bool any = false;
        memset(out, 0, sizeof(out));
        for (int b = lowest_bin; b < highest_bin; b++) {
            float sorted[32];
            for (int k = 0; k < present_count; k++)
                sorted[k] = magnitude[(size_t)present[k] * BINS + (size_t)b];
            qsort(sorted, (size_t)present_count, sizeof(float), CompareFloats);
            float median = present_count % 2 ? sorted[present_count / 2] : (sorted[present_count / 2 - 1] + sorted[present_count / 2]) / 2;
            Complex left = LeftOf(base_spectrum, b), right = RightOf(base_spectrum, b);
            float re = (left.re + right.re) / 2, im = (left.im + right.im) / 2;
            float here = sqrtf(re * re + im * im);
            if (plan->patch_count) {
                if (!patched[b] || here <= sorted[0])
                    continue;
                median = sorted[0];  /* the sound is on one and not the other */
            } else if (here <= kStandsOut * (median + 1e-5f) || here <= kQuietest) {
                continue;
            }
            int nearest = 0;
            float best = 1e30f;
            for (int k = 0; k < present_count; k++) {
                const int i = present[k];
                float distance = fabsf(magnitude[(size_t)i * BINS + (size_t)b] - median);
                if (distance < best) {
                    best = distance;
                    nearest = i;
                }
            }
            const Complex *x = spectra + (size_t)nearest * FFT_SIZE;
            Complex to_left = LeftOf(x, b), to_right = RightOf(x, b);
            Complex dl = {to_left.re - left.re, to_left.im - left.im};
            Complex dr = {to_right.re - right.re, to_right.im - right.im};
            out[b] = (Complex){dl.re - dr.im, dl.im + dr.re};
            out[FFT_SIZE - b] = (Complex){dl.re + dr.im, dr.re - dl.im};
            any = true;
        }
        if (!any)
            continue;
        InverseFft(out);
        for (int n = 0; n < FFT_SIZE; n++) {
            float *at = change + ((start + n) % length) * CHANNELS;
            at[0] += out[n].re * g_window[n];
            at[1] += out[n].im * g_window[n];
        }
    }
    for (long n = 0; n < length * CHANNELS; n++)
        base[n] += change[n] / kWindowSum;
    /* Copies read the recording around the one take, so they come before the end crosses over. */
    if (plan->clean && plan->take_count == 1) {
        const RenderTake *take = &plan->takes[0];
        if (!CleanCopies(plan->clean, base, length, plan->rate, take->from, take->source + take->phase, take->gain, stopping))
            goto done;
    }
    if (plan->fade > 0 && !CrossIntoStart(reader, plan, base))
        goto done;
    if (plan->clean && !CleanFilter(plan->clean, base, length, plan->rate, stopping))
        goto done;
    EaseStep(base, length, length);
    result = (int16 *)malloc((size_t)(length * CHANNELS) * sizeof(int16));
    if (result) {
        for (long n = 0; n < length * CHANNELS; n++) {
            float value = base[n] * 32768.0f;
            result[n] = (int16)lrintf(value > 32767.0f ? 32767.0f : value < -32768.0f ? -32768.0f : value);
        }
    }
done:
    if (passes) {
        for (int i = 0; i < count; i++)
            free(passes[i]);
    }
    free(passes);
    free(usable);
    free(spectra);
    free(magnitude);
    free(base);
    free(change);
    free(reader);
    return result;
}
