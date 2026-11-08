/* Cleaning a track that the passes alone can't: the player's typing, beeps and alarms, taken off
 * one take of the music.
 *
 * Three steps, each a port of a tool in tools/analysis/clean/, where they were tuned by ear:
 *
 * - Copies (copy_merge.py). Where the track repeats within its own recording, line the copy up to
 *   the sample and, pitch by pitch, read whichever is quieter where this one stands clearly above
 *   it: a sound laid over the music only ever adds. Where only the bars come round, alike but not
 *   the same, they still say what level each pitch should be at, and a brief point well over that
 *   comes down to it.
 *
 * - The filter (one_take_filter.py). The player's sounds are known: their pitches come from the
 *   sound chip (data/cues/sounds.toml). Where a sound's strongest pitches all jump well above what
 *   they hold just before and just after, together, that sound is there, and those pitches and
 *   what goes with them come down to the level around them, for as long as it lasts. While the
 *   text types, its pitches are learnt and taken off for as long as the typing goes on.
 *
 * - Long sounds (long_sounds.py). The filter judges a pitch against 0.15 s either side, which
 *   sits inside a sound as long as the alarm. Those are judged across the whole sound instead.
 *
 * The tools worked at 32 kHz. At 48 kHz every transform is half as long again, so each step sees
 * the same pitches and times.
 */
#include "loop_clean.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    CHANNELS = READER_CHANNELS,
    TOOL_RATE = 32000,
    MOST_PLANS = 6,
};

static const double kPi = 3.14159265358979323846;
static const float kNothing = -1e30f;  /* a level that can't stand out: off the filter's range */

typedef struct Cx {
    float re, im;
} Cx;

/* ---- transforms of 2^a 3^b points -------------------------------------------------------- */

typedef struct Plan {
    int n;
    Cx *twiddle;
    float *hann, *root;  /* the window, and its square root for reading and writing both */
    Cx *scratch;
} Plan;

static Plan g_plans[MOST_PLANS];
static int g_plan_count;

/* Only the render thread uses these. */
static const Plan *PlanFor(int n) {
    for (int i = 0; i < g_plan_count; i++) {
        if (g_plans[i].n == n)
            return &g_plans[i];
    }
    if (g_plan_count == MOST_PLANS)
        return NULL;
    Plan *p = &g_plans[g_plan_count];
    p->twiddle = (Cx *)malloc(sizeof(Cx) * (size_t)n);
    p->hann = (float *)malloc(sizeof(float) * (size_t)n);
    p->root = (float *)malloc(sizeof(float) * (size_t)n);
    p->scratch = (Cx *)malloc(sizeof(Cx) * (size_t)n);
    if (!p->twiddle || !p->hann || !p->root || !p->scratch) {
        free(p->twiddle);
        free(p->hann);
        free(p->root);
        free(p->scratch);
        return NULL;
    }
    for (int k = 0; k < n; k++) {
        p->twiddle[k] = (Cx){(float)cos(2 * kPi * k / n), (float)-sin(2 * kPi * k / n)};
        p->hann[k] = (float)(0.5 - 0.5 * cos(2 * kPi * k / n));
        p->root[k] = sqrtf(p->hann[k]);
    }
    p->n = n;
    g_plan_count++;
    return p;
}

static void Transform(const Plan *p, const Cx *in, Cx *out, int n, int stride) {
    if (n == 1) {
        out[0] = in[0];
        return;
    }
    const int r = n % 2 == 0 ? 2 : 3, m = n / r, step = p->n / n;
    for (int j = 0; j < r; j++)
        Transform(p, in + j * stride, out + j * m, m, stride * r);
    for (int k = 0; k < m; k++) {
        Cx v[3];
        for (int j = 0; j < r; j++) {
            Cx a = out[j * m + k], w = p->twiddle[(j * k * step) % p->n];
            v[j] = (Cx){a.re * w.re - a.im * w.im, a.re * w.im + a.im * w.re};
        }
        for (int q = 0; q < r; q++) {
            Cx sum = {0, 0};
            for (int j = 0; j < r; j++) {
                Cx w = p->twiddle[(j * q * (p->n / r)) % p->n];
                sum.re += v[j].re * w.re - v[j].im * w.im;
                sum.im += v[j].re * w.im + v[j].im * w.re;
            }
            out[q * m + k] = sum;
        }
    }
}

static void Fft(const Plan *p, Cx *x) {
    Transform(p, x, p->scratch, p->n, 1);
    memcpy(x, p->scratch, sizeof(Cx) * (size_t)p->n);
}

static void InverseFft(const Plan *p, Cx *x) {
    for (int i = 0; i < p->n; i++)
        x[i].im = -x[i].im;
    Fft(p, x);
    for (int i = 0; i < p->n; i++) {
        x[i].re /= p->n;
        x[i].im = -x[i].im / p->n;
    }
}

/* Both channels in one transform, as left + i right. The mid of the two, at one bin. */
static float MidMagnitude(const Cx *x, int n, int bin) {
    const Cx a = x[bin], b = x[(n - bin) % n];
    /* left = (a + conj b) / 2, right = (a - conj b) / 2i; their mean is half their sum. */
    float lre = (a.re + b.re) / 2, lim = (a.im - b.im) / 2;
    float rre = (a.im + b.im) / 2, rim = (b.re - a.re) / 2;
    float re = (lre + rre) / 2, im = (lim + rim) / 2;
    return sqrtf(re * re + im * im);
}

/* A frame of interleaved audio, `first` its first sample (it may start before 0 or run past the
 * end, where it reads silence), windowed, into one transform. */
static void ReadFrame(const float *audio, long count, long first, const float *window, int n, Cx *x) {
    for (int i = 0; i < n; i++) {
        long at = first + i;
        bool inside = at >= 0 && at < count;
        x[i] = (Cx){inside ? audio[at * CHANNELS] * window[i] : 0.0f, inside ? audio[at * CHANNELS + 1] * window[i] : 0.0f};
    }
}

/* The mid alone, windowed, its magnitude for bins below `bins`. */
static void MidSpectrum(const Plan *p, const float *audio, long count, long first, const float *window, Cx *x, float *magnitude, int bins) {
    for (int i = 0; i < p->n; i++) {
        long at = first + i;
        float mid = at >= 0 && at < count ? (audio[at * CHANNELS] + audio[at * CHANNELS + 1]) / 2 : 0.0f;
        x[i] = (Cx){mid * window[i], 0.0f};
    }
    Fft(p, x);
    for (int b = 0; b < bins; b++)
        magnitude[b] = sqrtf(x[b].re * x[b].re + x[b].im * x[b].im);
}

/* ---- small statistics ---------------------------------------------------------------------- */

static int CompareFloats(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* As numpy's median: the mean of the middle two of an even count. Sorts `v`. */
static float Median(float *v, int count) {
    qsort(v, (size_t)count, sizeof(float), CompareFloats);
    return count % 2 ? v[count / 2] : (v[count / 2 - 1] + v[count / 2]) / 2;
}

/* As numpy's percentile, linear between neighbours, of a sorted window. */
static float SortedPercentile(const float *sorted, int count, double q) {
    double at = q / 100.0 * (count - 1);
    int low = (int)floor(at);
    if (low >= count - 1)
        return sorted[count - 1];
    return (float)(sorted[low] + (at - low) * (sorted[low + 1] - sorted[low]));
}

static float Percentile(float *v, int count, double q) {
    qsort(v, (size_t)count, sizeof(float), CompareFloats);
    return SortedPercentile(v, count, q);
}

/* A window of floats kept sorted as one leaves and another comes in. */
static void SortedRemove(float *sorted, int *count, float value) {
    int low = 0, high = *count;
    while (low < high) {
        int mid = (low + high) / 2;
        if (sorted[mid] < value)
            low = mid + 1;
        else
            high = mid;
    }
    if (low < *count) {
        memmove(sorted + low, sorted + low + 1, sizeof(float) * (size_t)(*count - low - 1));
        (*count)--;
    }
}

static void SortedInsert(float *sorted, int *count, float value) {
    int low = 0, high = *count;
    while (low < high) {
        int mid = (low + high) / 2;
        if (sorted[mid] <= value)
            low = mid + 1;
        else
            high = mid;
    }
    memmove(sorted + low + 1, sorted + low, sizeof(float) * (size_t)(*count - low));
    sorted[low] = value;
    (*count)++;
}

/* The median of each run of `width` from t on, the last value standing in past the end. */
static void RunMedians(const float *v, long count, int width, float *out, float *window) {
    int held = 0;
    for (int i = 0; i < width; i++)
        SortedInsert(window, &held, v[i < count ? i : count - 1]);
    for (long t = 0; t < count; t++) {
        out[t] = held % 2 ? window[held / 2] : (window[held / 2 - 1] + window[held / 2]) / 2;
        long leaving = t, coming = t + width;
        SortedRemove(window, &held, v[leaving < count ? leaving : count - 1]);
        SortedInsert(window, &held, v[coming < count ? coming : count - 1]);
    }
}

/* Over the `side` frames just before each one and just after, as the tool has it: the first and
 * last frames stand in beyond the ends. Two percentiles of each. */
static void SidePercentiles(const float *v, long count, int side, double q1, double q2, float *past1, float *past2,
                            float *future1, float *future2, float *window) {
    int held = 0;
    for (int i = 0; i < side; i++)
        SortedInsert(window, &held, v[0]);
    for (long t = 0; t < count; t++) {
        past1[t] = SortedPercentile(window, held, q1);
        past2[t] = SortedPercentile(window, held, q2);
        SortedRemove(window, &held, v[t - side >= 0 ? t - side : 0]);
        SortedInsert(window, &held, v[t]);
    }
    held = 0;
    for (int i = 1; i <= side; i++)
        SortedInsert(window, &held, v[i < count ? i : count - 1]);
    for (long t = 0; t < count; t++) {
        future1[t] = SortedPercentile(window, held, q1);
        future2[t] = SortedPercentile(window, held, q2);
        long leaving = t + 1, coming = t + side + 1;
        SortedRemove(window, &held, v[leaving < count ? leaving : count - 1]);
        SortedInsert(window, &held, v[coming < count ? coming : count - 1]);
    }
}

/* ---- copies --------------------------------------------------------------------------------- */

static const float kCopyStandsDb = 2.0f;     /* over a copy, to read the copy there */
static const double kHoldSeconds = 3.0;      /* for where a copy is the same music */
static const double kCopyHolds = 0.7, kBarsHold = 0.4;
static const float kBarsOverDb = 6.0f;
static const double kBarsLongest = 0.15;     /* seconds: as long as a typing hit or a beep */
static const double kLowBandHz[2] = {80.0, 1500.0};

/* The low band of the mid, for lining copies up: brick-wall, by overlap-add. */
static float *LowBand(const int16 *pcm, long count, int rate) {
    const Plan *p = PlanFor(4096 * rate / TOOL_RATE);
    float *out = (float *)calloc((size_t)count, sizeof(float));
    Cx *x = (Cx *)malloc(sizeof(Cx) * (size_t)(p ? p->n : 1));
    if (!p || !out || !x) {
        free(out);
        free(x);
        return NULL;
    }
    const int n = p->n, hop = n / 4;
    const int low = (int)ceil(kLowBandHz[0] * n / rate), high = (int)floor(kLowBandHz[1] * n / rate);
    for (long first = hop - n; first < count; first += hop) {
        for (int i = 0; i < n; i++) {
            long at = first + i;
            float mid = at >= 0 && at < count ? (pcm[at * CHANNELS] + pcm[at * CHANNELS + 1]) / 65536.0f : 0.0f;
            x[i] = (Cx){mid * p->root[i], 0.0f};
        }
        Fft(p, x);
        for (int b = 0; b <= n / 2; b++) {
            if (b < low || b > high) {
                x[b] = (Cx){0, 0};
                if (b)
                    x[n - b] = (Cx){0, 0};
            }
        }
        InverseFft(p, x);
        for (int i = 0; i < n; i++) {
            long at = first + i;
            if (at >= 0 && at < count)
                out[at] += x[i].re * p->root[i] / 2.0f;
        }
    }
    free(x);
    return out;
}

typedef struct Copy {
    long at;        /* where it starts in the wide read */
    bool *holds;    /* by frame */
    bool bars;
} Copy;

/* To the sample around the lag, then where it holds, frame by frame. */
static bool LineUp(const float *low, long wide, long at0, long length, double lag, int rate, double holds_at, int hop, long frames,
                   Copy *copy) {
    const long reach = 64L * rate / TOOL_RATE, shift = lround(lag * rate);
    const int every = 4;  /* the band is far below the sample rate: every fourth sample will do */
    double own = 0;
    for (long i = 0; i < length; i += every)
        own += (double)low[at0 + i] * low[at0 + i];
    double best = -2;
    long best_at = -1;
    for (long d = -reach; d <= reach; d++) {
        long a = at0 + shift + d;
        if (a < 0 || a + length > wide)
            continue;
        double dot = 0, energy = 0;
        for (long i = 0; i < length; i += every) {
            dot += (double)low[at0 + i] * low[a + i];
            energy += (double)low[a + i] * low[a + i];
        }
        double r = dot / (sqrt(own * energy) + 1e-12);
        if (r > best) {
            best = r;
            best_at = a;
        }
    }
    if (best_at < 0)
        return false;
    copy->at = best_at;
    /* Local correlation over a few seconds, centred, by running sums. */
    const long w = (long)(kHoldSeconds * rate) | 1, half = (w - 1) / 2;
    double *sums = (double *)calloc((size_t)(length + 1) * 3, sizeof(double));
    copy->holds = (bool *)calloc((size_t)frames, sizeof(bool));
    if (!sums || !copy->holds) {
        free(sums);
        return false;
    }
    double *xy = sums, *xx = sums + length + 1, *yy = sums + 2 * (length + 1);
    for (long i = 0; i < length; i++) {
        double x = low[at0 + i], y = low[best_at + i];
        xy[i + 1] = xy[i] + x * y;
        xx[i + 1] = xx[i] + x * x;
        yy[i + 1] = yy[i] + y * y;
    }
    for (long f = 0; f < frames; f++) {
        long centre = f * hop < length - 1 ? f * hop : length - 1;
        long lo = centre - half < 0 ? 0 : centre - half, hi = centre - half + w > length ? length : centre - half + w;
        double num = xy[hi] - xy[lo], den = sqrt((xx[hi] - xx[lo]) * (yy[hi] - yy[lo])) + 1e-12;
        copy->holds[f] = num / den > holds_at;
    }
    free(sums);
    return true;
}

bool CleanCopies(const CleanPlan *plan, float *audio, long samples, int rate, const Recording *from, double source,
                 const float *gain, bool (*stopping)(void)) {
    const int copy_total = plan->copy_count + plan->bar_count;
    if (!copy_total || (rate * 2048) % TOOL_RATE)
        return true;
    const Plan *p = PlanFor(2048 * rate / TOOL_RATE);
    if (!p)
        return false;
    const int n = p->n, hop = n / 4, bins = n / 2 + 1;
    const long frames = (samples + n) / hop + 1;
    double reach = 0;
    for (int i = 0; i < plan->copy_count; i++)
        reach = fmax(reach, fabs(plan->copies[i]));
    for (int i = 0; i < plan->bar_count; i++)
        reach = fmax(reach, fabs(plan->bars[i]));
    reach += 2;
    const double wide_start = fmax(source - reach, 0);
    const long at0 = lround((source - wide_start) * rate);
    const long wide = samples + at0 + lround(reach * rate);
    bool ok = false;
    Reader *reader = (Reader *)calloc(1, sizeof(Reader));
    int16 *pcm = (int16 *)malloc(sizeof(int16) * (size_t)(wide * CHANNELS));
    float *low = NULL, *change = NULL;
    Copy copies[2 * CLEAN_MOST_LAGS];
    int count = 0;
    Cx *spectra = (Cx *)malloc(sizeof(Cx) * (size_t)n * (size_t)(copy_total + 1));
    float *mags = (float *)malloc(sizeof(float) * (size_t)bins * (size_t)(copy_total + 2));
    uint8 *marks = (uint8 *)calloc((size_t)frames * (size_t)bins, 1);
    memset(copies, 0, sizeof(copies));
    if (!reader || !pcm || !spectra || !mags || !marks)
        goto done;
    ReaderReset(reader);
    ReaderSeek(reader, from, rate, wide_start);
    for (long done = 0; done < wide;) {
        int16 *got_pcm;
        long want = wide - done;
        int got = ReaderRead(reader, &got_pcm, want > READER_READY_SAMPLES ? READER_READY_SAMPLES : (int)want);
        if (!got) {
            memset(pcm + done * CHANNELS, 0, sizeof(int16) * (size_t)((wide - done) * CHANNELS));
            break;
        }
        memcpy(pcm + done * CHANNELS, got_pcm, sizeof(int16) * (size_t)(got * CHANNELS));
        done += got;
    }
    if (stopping())
        goto done;
    low = LowBand(pcm, wide, rate);
    if (!low)
        goto done;
    for (int i = 0; i < copy_total; i++) {
        bool bars = i >= plan->copy_count;
        double lag = bars ? plan->bars[i - plan->copy_count] : plan->copies[i];
        if (LineUp(low, wide, at0, samples, lag, rate, bars ? kBarsHold : kCopyHolds, hop, frames, &copies[count])) {
            copies[count].bars = bars;
            count++;
        } else {
            free(copies[count].holds);
            copies[count].holds = NULL;
        }
        if (stopping())
            goto done;
    }
    free(low);
    low = NULL;
    change = (float *)calloc((size_t)(samples * CHANNELS), sizeof(float));
    if (!change)
        goto done;
    int bar_total = 0;
    for (int i = 0; i < count; i++)
        bar_total += copies[i].bars;
    const float stands = powf(10.0f, kCopyStandsDb / 20), over_by = powf(10.0f, kBarsOverDb / 20);
    const long longest = (long)(kBarsLongest * rate / hop);
    enum { OVER = 1, BRIEF = 2 };
    Cx *own = spectra + (size_t)copy_total * (size_t)n;
    float *own_mag = mags + (size_t)copy_total * bins, *best = mags + (size_t)(copy_total + 1) * bins;
    /* Two goes when there are bars: the first finds where each pitch stands over them, so the
     * second can tell which of those are brief. */
    for (int go = bar_total >= 2 ? 0 : 1; go < 2; go++) {
        for (long f = 0; f < frames; f++) {
            if (f % 256 == 0 && stopping())
                goto done;
            const long first = f * hop - n;
            int holding_copies = 0, holding_bars = 0;
            for (int i = 0; i < count; i++) {
                if (copies[i].holds[f])
                    copies[i].bars ? holding_bars++ : holding_copies++;
            }
            if (!holding_copies && holding_bars < 2)
                continue;
            ReadFrame(audio, samples, first, p->root, n, own);
            Fft(p, own);
            for (int b = 0; b < bins; b++)
                own_mag[b] = best[b] = MidMagnitude(own, n, b);
            Cx out[3072];
            memcpy(out, own, sizeof(Cx) * (size_t)n);
            for (int i = 0; i < count; i++) {
                if (!copies[i].holds[f])
                    continue;
                Cx *x = spectra + (size_t)i * n;
                for (int k = 0; k < n; k++) {
                    long at = copies[i].at + first + k;
                    bool inside = at >= copies[i].at && at < copies[i].at + samples && at >= 0 && at < wide;
                    x[k] = (Cx){inside ? pcm[at * CHANNELS] / 32768.0f * gain[0] * p->root[k] : 0.0f,
                                inside ? pcm[at * CHANNELS + 1] / 32768.0f * gain[1] * p->root[k] : 0.0f};
                }
                Fft(p, x);
                float *m = mags + (size_t)i * bins;
                for (int b = 0; b < bins; b++)
                    m[b] = MidMagnitude(x, n, b);
                if (copies[i].bars)
                    continue;
                for (int b = 0; b < bins; b++) {
                    if (m[b] < best[b] && own_mag[b] > m[b] * stands) {
                        best[b] = m[b];
                        out[b] = x[b];
                        if (b && b < n - b)
                            out[n - b] = x[n - b];
                    }
                }
            }
            if (holding_bars >= 2) {
                uint8 *mark = marks + (size_t)f * bins;
                for (int b = 0; b < bins; b++) {
                    float values[2 * CLEAN_MOST_LAGS];
                    int held = 0;
                    for (int i = 0; i < count; i++) {
                        if (copies[i].bars && copies[i].holds[f])
                            values[held++] = mags[(size_t)i * bins + b];
                    }
                    float median = Median(values, held);
                    float now = MidMagnitude(out, n, b);
                    if (go == 0) {
                        if (now > median * over_by)
                            mark[b] |= OVER;
                    } else if ((mark[b] & BRIEF) && now > median) {
                        float g = median / (now + 1e-12f);
                        if (g < 1) {
                            out[b].re *= g;
                            out[b].im *= g;
                            if (b && b < n - b) {
                                out[n - b].re *= g;
                                out[n - b].im *= g;
                            }
                        }
                    }
                }
            }
            if (go == 0)
                continue;
            bool any = false;
            for (int k = 0; k < n; k++) {
                out[k].re -= own[k].re;
                out[k].im -= own[k].im;
                any = any || out[k].re != 0 || out[k].im != 0;
            }
            if (!any)
                continue;
            InverseFft(p, out);
            for (int k = 0; k < n; k++) {
                long at = first + k;
                if (at >= 0 && at < samples) {
                    change[at * CHANNELS] += out[k].re * p->root[k] / 2.0f;
                    change[at * CHANNELS + 1] += out[k].im * p->root[k] / 2.0f;
                }
            }
        }
        if (go == 0) {
            /* Brief: a run along time no longer than a typing hit or a beep, and a frame more
             * either side. */
            for (int b = 0; b < bins; b++) {
                long start = -1;
                for (long f = 0; f <= frames; f++) {
                    bool over = f < frames && (marks[(size_t)f * bins + b] & OVER);
                    if (over && start < 0)
                        start = f;
                    if (!over && start >= 0) {
                        if (f - start <= longest) {
                            for (long g = start > 0 ? start - 1 : 0; g <= f && g < frames; g++)
                                marks[(size_t)g * bins + b] |= BRIEF;
                        }
                        start = -1;
                    }
                }
            }
        }
    }
    for (long i = 0; i < samples * CHANNELS; i++)
        audio[i] += change[i];
    ok = true;
done:
    for (int i = 0; i < 2 * CLEAN_MOST_LAGS; i++)
        free(copies[i].holds);
    free(reader);
    free(pcm);
    free(low);
    free(change);
    free(spectra);
    free(mags);
    free(marks);
    return ok;
}

/* ---- the filter ---------------------------------------------------------------------------- */

/* As tuned in one_take_filter.py: P there. */
static const double kSideSeconds = 0.15, kTrainSeconds = 0.3, kLongestBlip = 0.12;
static const float kT2 = 9, kT1 = 14, kLone = 4, kGrow = 6, kFloorDb = -65, kDepthDb = -90, kRelBonus = 6;
static const double kTopHz = 9000, kBassHz = 450, kLowestHz = 200, kGenericLowHz = 500, kHotTopHz = 4500;
static const float kBassDb = 9, kBassDepthDb = -12, kMainPenalty = 6, kStepDb = 4, kStepMargin = 20;
static const float kExtraDb = 8, kExtraNarrow = 6, kBeepDb = 15, kBeepNarrow = 15, kBeepNarrowLater = 12;
static const int kBeepAfter = 15, kReach = 2, kSlack = 12, kSkirt = 10;
static const float kSkirtDb = 18, kWide = 25, kConfident = 15, kHotMin = 0.15f, kHotRatio = 3, kGateDb = 4;
static const int kNeighbours[8] = {-12, -9, -6, -4, 4, 6, 9, 12};

typedef struct Grid {
    int n, hop, bins, cols, width;  /* cols: the filter's range; width: what's kept of each frame */
    long frames;
    double per_second, rate;
    float *mag, *bg, *target, *step, *rdb, *rel, *gain;
    float loud;
} Grid;

static inline float Mag(const Grid *g, long f, int b) { return b < g->width ? g->mag[f * g->width + b] : 0.0f; }
static inline bool Loud(const Grid *g, long f, int b) { return Mag(g, f, b) > g->loud; }
static inline float Rdb(const Grid *g, long f, int b) { return b >= 1 && b < g->cols ? g->rdb[f * g->cols + b] : kNothing; }
static inline float Rel(const Grid *g, long f, int b) { return b >= 1 && b < g->cols ? g->rel[f * g->cols + b] : kNothing; }
static inline float Step(const Grid *g, long f, int b) { return b >= 1 && b < g->cols ? g->step[f * g->cols + b] : 0.0f; }
static inline float Target(const Grid *g, long f, int b) { return g->target[f * g->cols + b]; }

static inline bool IsTyping(const CleanSound *s) { return s->typing; }

typedef struct Event {
    long t;
    int sound;
    float score;
    long order;
} Event;

static int ByScore(const void *a, const void *b) {
    const Event *x = (const Event *)a, *y = (const Event *)b;
    if (x->score != y->score)
        return x->score > y->score ? -1 : 1;
    return (x->order > y->order) - (x->order < y->order);
}

static const CleanSound *g_sorting_sounds;
static int ByTime(const void *a, const void *b) {
    const Event *x = (const Event *)a, *y = (const Event *)b;
    if (x->t != y->t)
        return (x->t > y->t) - (x->t < y->t);
    int names = strcmp(g_sorting_sounds[x->sound].name, g_sorting_sounds[y->sound].name);
    if (names)
        return names;
    return (x->score > y->score) - (x->score < y->score);
}

static float Near(const Grid *g, long f, int b) {
    float best = -INFINITY;
    for (int c = b - 1 < 1 ? 1 : b - 1; c < (b + 2 < g->bins ? b + 2 : g->bins); c++) {
        float v = Loud(g, f, c) ? Rel(g, f, c) : -99.0f;
        if (v > best)
            best = v;
    }
    return best;
}

static float MeanMag(const Grid *g, long from, long to, int c) {
    double sum = 0;
    for (long f = from; f < to; f++)
        sum += Mag(g, f, c);
    return to > from ? (float)(sum / (to - from)) : 0.0f;
}

/* Bring one pitch down, between a and e, to the level either side straight across under it. */
static void LevelOut(Grid *g, long a, long e, int c, bool bass_rule) {
    const long frames = g->frames;
    float l0 = MeanMag(g, a - 3 > 0 ? a - 3 : 0, a - 1 > 1 ? a - 1 : 1, c);
    float l1 = e + 2 < frames ? MeanMag(g, e + 2 < frames - 1 ? e + 2 : frames - 1, e + 4 < frames ? e + 4 : frames, c) : Mag(g, e, c);
    const float bass_floor = powf(10.0f, kBassDepthDb / 20), deepest = powf(10.0f, kDepthDb / 20);
    const bool low = c * g->rate / g->n < kBassHz;
    for (long u = a; u <= e; u++) {
        float level = l0 + (l1 - l0) * (float)(u - a + 1) / (float)(e - a + 2);
        float m = Mag(g, u, c);
        float gain = fminf(1.0f, fmaxf(level, Target(g, u, c)) / (m + 1e-12f));
        if (low) {
            if (bass_rule)
                gain = Rdb(g, u, c) > kBassDb ? fmaxf(gain, bass_floor) : 1.0f;
            else
                gain = fmaxf(gain, bass_floor);
        }
        if (bass_rule)
            gain = fmaxf(gain, deepest);
        float *at = &g->gain[u * g->cols + c];
        if (gain < *at)
            *at = gain;
    }
}

static void Repair(Grid *g, long t, const bool *pitches, long maxlen) {
    const long frames = g->frames;
    for (int b = 0; b < g->bins; b++) {
        if (!pitches[b])
            continue;
        float widest = kNothing;
        for (long u = t - 2 > 0 ? t - 2 : 0; u < t + 3 && u < frames; u++)
            widest = fmaxf(widest, Rdb(g, u, b));
        int reach = kReach + (widest > kWide ? 1 : 0);
        for (int c = b - reach < 1 ? 1 : b - reach; c < b + reach + 1 && c < g->bins; c++) {
            if (c >= g->cols)
                break;  /* off the range: nothing stands out there */
            long t_c = -1;
            float top = 0;
            for (long u = t - 2 > 0 ? t - 2 : 0; u < t + 3 && u < frames; u++) {
                float ex = Rdb(g, u, c);
                if (t_c < 0 || ex > top) {
                    top = ex;
                    t_c = u;
                }
            }
            if (!(top > kGrow))
                continue;  /* nothing of the sound at this pitch */
            long a = t_c, e = t_c;
            while (a > 0 && t_c - a < maxlen && Rdb(g, a - 1, c) > kGrow)
                a--;
            while (e < frames - 1 && e - t_c < maxlen && Rdb(g, e + 1, c) > kGrow)
                e++;
            LevelOut(g, a, e, c, true);
        }
    }
}

static float Around(const Grid *g, long f, int b) {
    float v[4] = {Rdb(g, f, b - 5), Rdb(g, f, b - 4), Rdb(g, f, b + 4), Rdb(g, f, b + 5)};
    return Median(v, 4);
}

static bool Peak(const Grid *g, long f, int b, int lo_b, int hi_b) {
    return b > lo_b && b < hi_b - 1 && Mag(g, f, b) >= Mag(g, f, b - 1) && Mag(g, f, b) >= Mag(g, f, b + 1);
}

static int RoundHalfEven(double x) { return (int)rint(x); }

/* What the tool's deblip() does to one stretch: the gain for every pitch and moment. */
static bool FilterGains(Grid *g, const CleanSounds *set) {
    const long frames = g->frames;
    const int bins = g->bins;
    const double fps = g->per_second;
    const int side = (int)(kSideSeconds * fps);
    float *column = (float *)malloc(sizeof(float) * (size_t)frames * 5);
    float *window = (float *)malloc(sizeof(float) * (size_t)(side + 2));
    float *rows = (float *)malloc(sizeof(float) * (size_t)frames * (CLEAN_PITCHES + 3));
    Event *events = NULL;
    long event_count = 0, event_room = 0;
    bool *taken = (bool *)calloc((size_t)frames, 1);
    bool *pitches = (bool *)calloc((size_t)bins * 3, 1);
    float *votes = (float *)calloc((size_t)bins * 2, sizeof(float));
    bool ok = false;
    if (!column || !window || !rows || !taken || !pitches || !votes)
        goto done;
    /* What each pitch holds either side: the level it's judged against, and how far it steps. */
    for (int b = 1; b < g->cols; b++) {
        float *v = column, *p40 = column + frames, *p20 = column + 2 * frames, *f40 = column + 3 * frames, *f20 = column + 4 * frames;
        for (long f = 0; f < frames; f++)
            v[f] = Mag(g, f, b);
        SidePercentiles(v, frames, side, 40, 20, p40, p20, f40, f20, window);
        for (long f = 0; f < frames; f++) {
            float bg = fmaxf(p40[f], f40[f]);
            g->bg[f * g->cols + b] = bg;
            g->target[f * g->cols + b] = fmaxf(p20[f], f20[f]);
            float step = fabsf(20 * log10f((f40[f] + 1e-9f) / (p40[f] + 1e-9f)));
            float step20 = fabsf(20 * log10f((f20[f] + 1e-9f) / (p20[f] + 1e-9f)));
            g->step[f * g->cols + b] = fminf(step, step20);
            g->rdb[f * g->cols + b] = 20 * log10f(v[f] + 1e-12f) - 20 * log10f(bg + 1e-12f);
        }
    }
    /* Against the neighbouring pitches at the same moment: a drum lifts them all. */
    for (long f = 0; f < frames; f++) {
        for (int b = 1; b < g->cols; b++) {
            float v[8];
            for (int k = 0; k < 8; k++) {
                int c = b + kNeighbours[k];
                c = c < 0 ? 0 : c > bins - 1 ? bins - 1 : c;
                v[k] = Rdb(g, f, c);
            }
            float nb = Median(v, 8), r = Rdb(g, f, b);
            g->rel[f * g->cols + b] = fminf(r, r - fmaxf(nb, 0.0f) + kRelBonus);
        }
    }
    /* Where each known sound is. */
    for (int si = 0; si < set->count; si++) {
        const CleanSound *s = &set->sounds[si];
        int strong[CLEAN_PITCHES], count = 0;
        for (int i = 0; i < s->count; i++) {
            int b = (int)lround(s->hz[i] * g->n / g->rate);
            bool apart = true;
            for (int j = 0; j < count; j++)
                apart = apart && abs(b - strong[j]) > 3;
            if (apart)
                strong[count++] = b;
        }
        if (!count)
            continue;
        float *score = rows + (size_t)CLEAN_PITCHES * frames, *spread = score + frames;
        for (int j = 0; j < count; j++) {
            for (long f = 0; f < frames; f++)
                rows[(size_t)j * frames + f] = Near(g, f, strong[j]);
        }
        const int k = count > 1 ? (int)fmax(2, ceil(count * 0.5)) : 1;
        for (long f = 0; f < frames; f++) {
            float v[CLEAN_PITCHES], steps[CLEAN_PITCHES];
            for (int j = 0; j < count; j++) {
                v[j] = rows[(size_t)j * frames + f];
                steps[j] = Step(g, f, strong[j]);
            }
            float sc;
            if (count > 1) {
                qsort(v, (size_t)count, sizeof(float), CompareFloats);
                sc = fmaxf(v[count - k], rows[f] - kMainPenalty);
            } else {
                sc = v[0];
            }
            float step = Median(steps, count);
            float main_abs = -INFINITY;
            for (int c = strong[0] - 1 < 1 ? 1 : strong[0] - 1; c < (strong[0] + 2 < bins ? strong[0] + 2 : bins); c++)
                main_abs = fmaxf(main_abs, Loud(g, f, c) ? Rdb(g, f, c) : -99.0f);
            /* A note starting or ending steps about as far as it stands out; a blip far more. */
            if (step > kStepDb && main_abs < step + kStepMargin)
                sc = -99;
            score[f] = sc;
        }
        for (long f = 0; f < frames; f++) {
            float before = score[(f - 1 + frames) % frames], after = score[(f + 1) % frames];
            spread[f] = fmaxf(score[f], fmaxf(before, after));
        }
        const long longest = (long)(s->core * fps) + g->n / g->hop + kSlack;
        const float need = count > 1 ? kT2 : kT1;
        long span = (long)(s->core * fps);
        if (span < 1)
            span = 1;
        for (long t = 0; t < frames; t++) {
            if (!(spread[t] >= need))
                continue;
            float top = spread[t];
            for (long u = t - span > 0 ? t - span : 0; u < t + span + 1 && u < frames; u++)
                top = fmaxf(top, spread[u]);
            if (spread[t] != top)
                continue;
            int main = 0;
            for (int j = 1; j < count; j++) {
                if (rows[(size_t)j * frames + t] > rows[(size_t)main * frames + t])
                    main = j;
            }
            const float *m = rows + (size_t)main * frames;
            long a = t, e = t;
            while (a > 0 && t - a <= longest && m[a - 1] > kGrow)
                a--;
            while (e < frames - 1 && e - a <= longest && m[e + 1] > kGrow)
                e++;
            if (e - a > longest)
                continue;
            if (event_count == event_room) {
                event_room = event_room ? event_room * 2 : 1024;
                Event *grown = (Event *)realloc(events, sizeof(Event) * (size_t)event_room);
                if (!grown)
                    goto done;
                events = grown;
            }
            events[event_count] = (Event){t, si, spread[t], event_count};
            event_count++;
        }
    }
    /* Every sound that fits gets taken off, strongest first, one at a time. */
    qsort(events, (size_t)event_count, sizeof(Event), ByScore);
    long kept = 0;
    for (long i = 0; i < event_count; i++) {
        const CleanSound *s = &set->sounds[events[i].sound];
        long span = (long)(s->core * fps) / 2;
        if (span < 2)
            span = 2;
        long t = events[i].t, lo = t - span > 0 ? t - span : 0, hi = t + span + 1 < frames ? t + span + 1 : frames;
        bool clash = false;
        for (long u = lo; u < hi && !clash; u++)
            clash = taken[u];
        if (clash)
            continue;
        for (long u = lo; u < hi; u++)
            taken[u] = true;
        events[kept++] = events[i];
    }
    g_sorting_sounds = set->sounds;
    qsort(events, (size_t)kept, sizeof(Event), ByTime);
    /* A lone typing hit, not part of a run of them, has to be clearer. */
    const long train = (long)(kTrainSeconds * fps);
    long *typed_at = (long *)malloc(sizeof(long) * (size_t)(kept ? kept : 1));
    if (!typed_at)
        goto done;
    long typed_count = 0;
    for (long i = 0; i < kept; i++) {
        if (IsTyping(&set->sounds[events[i].sound]))
            typed_at[typed_count++] = events[i].t;
    }
    long final = 0;
    for (long i = 0; i < kept; i++) {
        const Event ev = events[i];
        if (IsTyping(&set->sounds[ev.sound])) {
            bool company = false;
            for (long j = 0; j < typed_count && !company; j++) {
                long gap = labs(typed_at[j] - ev.t);
                company = gap > 0 && gap <= train;
            }
            if (!company && ev.score < kT1 + kLone)
                continue;
        }
        events[final++] = ev;
    }
    free(typed_at);
    for (long i = 0; i < frames * g->cols; i++)
        g->gain[i] = 1.0f;
    const long maxlen = (long)(kLongestBlip * fps) + g->n / g->hop;
    const int lo_b = (int)(kGenericLowHz * g->n / g->rate);
    const int hi_b = (int)fmin(kTopHz * g->n / g->rate, bins - 6);
    bool *harm = pitches + bins, *slide = pitches + 2 * bins;
    for (long i = 0; i < final; i++) {
        const long t = events[i].t;
        const CleanSound *s = &set->sounds[events[i].sound];
        memset(pitches, 0, (size_t)bins * 3);
        const int f0 = (int)lround(s->hz[0] * g->n / g->rate);
        /* The recording's pitch can sit a little off the chip's: measure it, between bins. */
        const int lo_f = f0 - 3 > 2 ? f0 - 3 : 2, hi_f = f0 + 4 < bins - 2 ? f0 + 4 : bins - 2;
        const long w0 = t - 2 > 0 ? t - 2 : 0, w1 = t + 3 < frames ? t + 3 : frames;
        int k0 = lo_f;
        float top = -1;
        for (int b = lo_f; b < hi_f; b++) {
            float m = 0;
            for (long u = w0; u < w1; u++)
                m = fmaxf(m, Mag(g, u, b));
            if (m > top) {
                top = m;
                k0 = b;
            }
        }
        float peak[3] = {0, 0, 0};
        for (int d = -1; d <= 1; d++) {
            for (long u = w0; u < w1; u++)
                peak[d + 1] = fmaxf(peak[d + 1], Mag(g, u, k0 + d));
        }
        double la = log(peak[0] + 1e-12), lb = log(peak[1] + 1e-12), lc = log(peak[2] + 1e-12);
        double den = la - 2 * lb + lc;
        double fa = k0 + (den < 0 ? 0.5 * (la - lc) / den : 0.0);
        if (fabs(fa - f0) > 3)
            fa = f0;
        for (int k = 2; k < 40; k++) {
            if (fa * k >= bins - 3)
                break;
            for (int d = -2; d <= 2; d++) {
                int h = RoundHalfEven(fa * k) + d;
                if (h >= 0 && h < bins)
                    harm[h] = true;
            }
        }
        /* Its own overtones, which the recording has and the chip's render may not. */
        for (long u = w0; u < w1; u++) {
            for (int b = lo_b; b < hi_b; b++) {
                if (!harm[b] || !Peak(g, u, b, lo_b, hi_b) || !Loud(g, u, b))
                    continue;
                float seg = Rdb(g, u, b);
                if (seg >= kExtraDb && seg - Around(g, u, b) >= kExtraNarrow)
                    pitches[b] = true;
            }
        }
        if (!IsTyping(s) || strcmp(s->name, "e48") == 0) {
            /* What else starts with a beep, stands out sharply and stops when it does. */
            for (int k = 1; k < 6; k++) {
                int l = (int)(fa * k * 0.9), h = (int)(fa * k * 1.25) + 2;
                for (int b = l > lo_b ? l : lo_b; b < h && b < hi_b; b++)
                    slide[b] = true;
            }
            for (long f = t - 3 > 0 ? t - 3 : 0; f < t + kBeepAfter && f < frames; f++) {
                const bool near_start = f <= t + 2;
                for (int b = lo_b; b < hi_b; b++) {
                    if (!Peak(g, f, b, lo_b, hi_b))
                        continue;
                    float seg = Rdb(g, f, b);
                    if (!(seg >= kBeepDb && seg - Around(g, f, b) >= (near_start ? kBeepNarrow : kBeepNarrowLater)))
                        continue;
                    if (!near_start && !slide[b])
                        continue;
                    const long r0 = t - maxlen > 0 ? t - maxlen : 0, r1 = t + maxlen < frames ? t + maxlen : frames;
                    long a = t - r0, e = t - r0;
                    while (a > 0 && Rdb(g, r0 + a - 1, b) > kGrow)
                        a--;
                    while (e < r1 - r0 - 1 && Rdb(g, r0 + e + 1, b) > kGrow)
                        e++;
                    if (a > 0 && e < r1 - r0 - 1)
                        pitches[b] = true;  /* it ends within reach: not a held note */
                }
            }
            /* A short beep's energy spreads either side of its pitch. */
            for (int k = 1; k < 4; k++) {
                int centre = RoundHalfEven(fa * k);
                if (centre >= bins - 3)
                    break;
                for (int c = centre - kSkirt > 2 ? centre - kSkirt : 2; c < centre + kSkirt + 1 && c < bins - 1; c++) {
                    float most = kNothing;
                    for (long u = t - 3 > 0 ? t - 3 : 0; u < t + 4 && u < frames; u++)
                        most = fmaxf(most, Rdb(g, u, c));
                    if (most > kSkirtDb)
                        pitches[c] = true;
                }
            }
        }
        for (int j = 0; j < s->count; j++) {
            int b = (int)lround(s->hz[j] * g->n / g->rate);
            if (b >= 0 && b < bins)
                pitches[b] = true;
        }
        Repair(g, t, pitches, maxlen);
    }
    /* While the text types, its pitches are known: learn them from the clear cases. */
    long confident = 0;
    for (long i = 0; i < final; i++)
        confident += IsTyping(&set->sounds[events[i].sound]) && events[i].score >= kConfident;
    if (confident >= 5) {
        float *local = votes + bins;
        for (long i = 0; i < final; i++) {
            if (!IsTyping(&set->sounds[events[i].sound]) || events[i].score < kConfident)
                continue;
            for (long u = events[i].t - 1 > 0 ? events[i].t - 1 : 0; u < events[i].t + 2 && u < frames; u++) {
                for (int b = 3; b < bins - 3; b++) {
                    if (Rdb(g, u, b) > 8 && Loud(g, u, b) && Mag(g, u, b) >= Mag(g, u, b - 1) && Mag(g, u, b) >= Mag(g, u, b + 1))
                        votes[b] += 1;
                }
            }
        }
        float around[21];
        for (int b = 0; b < bins; b++) {
            int n = 0;
            for (int c = b - 10 > 0 ? b - 10 : 0; c < b + 11 && c < bins; c++)
                around[n++] = votes[c];
            local[b] = Median(around, n);
        }
        long *times = (long *)malloc(sizeof(long) * (size_t)(final ? final : 1));
        if (!times)
            goto done;
        long typed = 0;
        for (long i = 0; i < final; i++) {
            if (IsTyping(&set->sounds[events[i].sound]))
                times[typed++] = events[i].t;
        }
        const long pad = (long)(0.05 * fps);
        long start = -1;
        for (long i = 0; i < typed; i++) {
            long t = times[i];
            if (start < 0)
                start = t;
            if (i + 1 == typed || times[i + 1] - t > train) {
                if (i > 0 && t > start) {
                    long a0 = start - pad > 0 ? start - pad : 0;
                    long e0 = t + pad + g->n / g->hop < frames - 1 ? t + pad + g->n / g->hop : frames - 1;
                    for (int b = 0; b < bins; b++) {
                        double hz = b * g->rate / g->n;
                        bool hot = votes[b] >= kHotMin * confident && votes[b] > kHotRatio * (local[b] + 1) && hz >= kLowestHz &&
                                   hz <= kHotTopHz;
                        if (!hot)
                            continue;
                        for (int c = b - 1 > 1 ? b - 1 : 1; c < b + 2 && c < bins && c < g->cols; c++) {
                            long run = -1;
                            for (long u = a0; u <= e0 + 1; u++) {
                                bool up = u <= e0 && Rdb(g, u, c) > kGateDb;
                                if (up && run < 0)
                                    run = u;
                                if (!up && run >= 0) {
                                    if (u - 1 - run <= maxlen)
                                        LevelOut(g, run, u - 1, c, false);
                                    run = -1;
                                }
                            }
                        }
                    }
                }
                start = -1;
            }
        }
        free(times);
    }
    /* Never below what that pitch usually holds either side. */
    for (long f = 0; f < frames; f++) {
        for (int b = 1; b < g->cols; b++) {
            float floor_gain = fminf(1.0f, g->bg[f * g->cols + b] / (Mag(g, f, b) + 1e-12f));
            float *at = &g->gain[f * g->cols + b];
            *at = fmaxf(*at, floor_gain);
        }
    }
    ok = true;
done:
    free(column);
    free(window);
    free(rows);
    free(events);
    free(taken);
    free(pitches);
    free(votes);
    return ok;
}

/* The audio again through the gains: only what changes is added back. */
static void ApplyGains(const Plan *p, const float *x, long count, const float *gains, int cols, long frames, int hop, float *y) {
    Cx buffer[3072];
    const int n = p->n;
    const float scale = (float)n / hop / 2;  /* the window's sum at this hop */
    for (long f = 0; f < frames; f++) {
        const float *gain = gains + f * cols;
        bool any = false;
        for (int b = 1; b < cols && !any; b++)
            any = gain[b] < 1.0f;
        if (!any)
            continue;
        const long first = f * hop - n;
        ReadFrame(x, count, first, p->root, n, buffer);
        Fft(p, buffer);
        for (int b = 0; b < n; b++) {
            int k = b <= n / 2 ? b : n - b;
            float less = k >= 1 && k < cols ? gain[k] - 1.0f : 0.0f;
            buffer[b].re *= less;
            buffer[b].im *= less;
        }
        InverseFft(p, buffer);
        for (int i = 0; i < n; i++) {
            long at = first + i;
            if (at >= 0 && at < count) {
                y[at * CHANNELS] += buffer[i].re * p->root[i] / scale;
                y[at * CHANNELS + 1] += buffer[i].im * p->root[i] / scale;
            }
        }
    }
}

static bool FilterPiece(const CleanPlan *plan, const float *x, long count, int rate, float *y) {
    memcpy(y, x, sizeof(float) * (size_t)(count * CHANNELS));
    const Plan *p = PlanFor(1024 * rate / TOOL_RATE);
    if (!p)
        return false;
    Grid g = {0};
    g.n = p->n;
    g.hop = g.n / 8;
    g.bins = g.n / 2 + 1;
    g.rate = rate;
    g.per_second = (double)rate / g.hop;
    g.cols = (int)fmin(g.bins, (int)(kTopHz * g.n / rate) + 8);
    g.width = (int)fmin(g.bins, g.cols + 8);
    g.frames = (count + g.n) / g.hop + 1;
    g.loud = powf(10.0f, kFloorDb / 20) * g.n / 4;
    const size_t cells = (size_t)g.frames * (size_t)g.cols;
    g.mag = (float *)malloc(sizeof(float) * (size_t)g.frames * (size_t)g.width);
    g.bg = (float *)malloc(sizeof(float) * cells);
    g.target = (float *)malloc(sizeof(float) * cells);
    g.step = (float *)malloc(sizeof(float) * cells);
    g.rdb = (float *)malloc(sizeof(float) * cells);
    g.rel = (float *)malloc(sizeof(float) * cells);
    g.gain = (float *)malloc(sizeof(float) * cells);
    Cx *buffer = (Cx *)malloc(sizeof(Cx) * (size_t)g.n);
    float *magnitude = (float *)malloc(sizeof(float) * (size_t)g.bins);
    bool ok = false;
    if (!g.mag || !g.bg || !g.target || !g.step || !g.rdb || !g.rel || !g.gain || !buffer || !magnitude)
        goto done;
    for (long f = 0; f < g.frames; f++) {
        MidSpectrum(p, x, count, f * g.hop - g.n, p->hann, buffer, magnitude, g.width);
        memcpy(g.mag + f * g.width, magnitude, sizeof(float) * (size_t)g.width);
    }
    for (long f = 0; f < g.frames; f++) {
        g.rdb[f * g.cols] = kNothing;
        g.rel[f * g.cols] = kNothing;
        g.step[f * g.cols] = 0;
        g.bg[f * g.cols] = INFINITY;
        g.target[f * g.cols] = INFINITY;
    }
    if (!FilterGains(&g, plan->sounds))
        goto done;
    for (long f = 0; f < g.frames; f++)
        g.gain[f * g.cols] = 1.0f;
    ApplyGains(p, x, count, g.gain, g.cols, g.frames, g.hop, y);
    ok = true;
done:
    free(g.mag);
    free(g.bg);
    free(g.target);
    free(g.step);
    free(g.rdb);
    free(g.rel);
    free(g.gain);
    free(buffer);
    free(magnitude);
    return ok;
}

/* ---- long sounds --------------------------------------------------------------------------- */

static const double kLongSide = 0.3, kLongTopHz = 12000;
static const float kLongStands = 20, kLongGrow = 6;
enum { LONG_GAP = 2 };

static bool LongPiece(const CleanPlan *plan, const float *x, long count, int rate, float *y) {
    memcpy(y, x, sizeof(float) * (size_t)(count * CHANNELS));
    const CleanSounds *set = plan->sounds;
    if (!set->long_count)
        return true;
    const Plan *p = PlanFor(1024 * rate / TOOL_RATE);
    if (!p)
        return false;
    const int n = p->n, hop = n / 8, bins = n / 2 + 1;
    const int width = (int)fmin(bins, (int)(kLongTopHz * n / rate) + 4);
    const long frames = (count + n) / hop + 1;
    const double fps = (double)rate / hop;
    const int side = (int)(kLongSide * fps);
    float *mag = (float *)malloc(sizeof(float) * (size_t)frames * (size_t)width);
    float *level = (float *)malloc(sizeof(float) * (size_t)frames * (size_t)width);
    float *work = (float *)malloc(sizeof(float) * (size_t)frames * (CLEAN_PITCHES + 4));
    float *window = (float *)malloc(sizeof(float) * (size_t)(frames + side + 64));
    Cx *buffer = (Cx *)malloc(sizeof(Cx) * (size_t)n);
    float *gain = NULL;
    Event *events = NULL;
    long event_count = 0, event_room = 0;
    bool ok = false;
    if (!mag || !level || !work || !window || !buffer)
        goto done;
    for (long f = 0; f < frames; f++) {
        MidSpectrum(p, x, count, f * hop - n, p->hann, buffer, mag + f * width, width);
        for (int b = 0; b < width; b++)
            level[f * width + b] = 20 * log10f(mag[f * width + b] + 1e-12f);
    }
    for (int si = 0; si < set->long_count; si++) {
        const CleanSound *s = &set->longs[si];
        int d = (int)(s->core * fps);
        if (d < 4)
            d = 4;
        float *col = work, *inside = work + frames, *around = work + 2 * frames, *score = work + 3 * frames;
        float *ex = work + 4 * frames;  /* the main pitch's, then each other's in turn */
        for (long f = 0; f < frames; f++)
            score[f] = INFINITY;
        float *kth = (float *)malloc(sizeof(float) * (size_t)frames * (size_t)s->count);
        if (!kth)
            goto done;
        for (int j = 0; j < s->count; j++) {
            int b = (int)lround(s->hz[j] * n / rate);
            for (long f = 0; f < frames; f++) {
                float best = -INFINITY;
                for (int c = b - 1 < 1 ? 1 : b - 1; c < b + 2 && c < width; c++)
                    best = fmaxf(best, level[f * width + c]);
                col[f] = best;
            }
            RunMedians(col, frames, d, inside, window);
            RunMedians(col, frames, side, around, window);
            for (long f = 0; f < frames; f++) {
                float before = f >= side + LONG_GAP ? around[f - side - LONG_GAP] : INFINITY;
                float after = f + d + LONG_GAP < frames ? around[f + d + LONG_GAP] : INFINITY;
                float e = inside[f] - fmaxf(before, after);
                kth[(size_t)f * s->count + j] = e;
                if (j == 0)
                    ex[f] = e;
            }
        }
        const int k = (int)fmax(2, ceil(s->count * 0.5));
        for (long f = 0; f < frames; f++) {
            float *v = kth + (size_t)f * s->count;
            qsort(v, (size_t)s->count, sizeof(float), CompareFloats);
            score[f] = fminf(v[s->count - k], ex[f]);
        }
        free(kth);
        for (long t = 0; t < frames; t++) {
            if (!(score[t] >= kLongStands))
                continue;
            float top = score[t];
            for (long u = t - d > 0 ? t - d : 0; u < t + d + 1 && u < frames; u++)
                top = fmaxf(top, score[u]);
            if (score[t] != top)
                continue;
            if (event_count == event_room) {
                event_room = event_room ? event_room * 2 : 16;
                Event *grown = (Event *)realloc(events, sizeof(Event) * (size_t)event_room);
                if (!grown)
                    goto done;
                events = grown;
            }
            events[event_count] = (Event){t, si, score[t], event_count};
            event_count++;
        }
    }
    if (!event_count) {
        ok = true;
        goto done;
    }
    qsort(events, (size_t)event_count, sizeof(Event), ByScore);
    gain = (float *)malloc(sizeof(float) * (size_t)frames * (size_t)width);
    bool *taken = (bool *)calloc((size_t)frames, 1);
    float *edge = (float *)malloc(sizeof(float) * (size_t)(side + 1));
    if (!gain || !taken || !edge) {
        free(taken);
        free(edge);
        goto done;
    }
    for (long i = 0; i < frames * width; i++)
        gain[i] = 1.0f;
    for (long i = 0; i < event_count; i++) {
        const CleanSound *s = &set->longs[events[i].sound];
        const long t = events[i].t;
        int d = (int)(s->core * fps);
        if (d < 4)
            d = 4;
        bool clash = false;
        for (long u = t; u < t + d && u < frames && !clash; u++)
            clash = taken[u];
        if (clash)
            continue;
        for (long u = t; u < t + d && u < frames; u++)
            taken[u] = true;
        int strong[CLEAN_PITCHES];
        for (int j = 0; j < s->count; j++)
            strong[j] = (int)lround(s->hz[j] * n / rate);
        int f0 = strong[0];
        if (strong[0] * (double)rate / n >= 4000) {
            for (int j = 1; j < s->count; j++)
                f0 = strong[j] < f0 ? strong[j] : f0;
        }
        const int lo_f = f0 - 3 > 2 ? f0 - 3 : 2, hi_f = f0 + 4 < bins - 2 ? f0 + 4 : bins - 2;
        int fa = lo_f;
        float top = -1;
        for (int b = lo_f; b < hi_f; b++) {
            float m = 0;
            for (long u = t; u < t + d && u < frames; u++)
                m = fmaxf(m, mag[u * width + b]);
            if (m > top) {
                top = m;
                fa = b;
            }
        }
        bool pitches[3073] = {false};
        for (int j = 0; j < s->count; j++)
            pitches[strong[j]] = true;
        for (int h = 1; h < 40; h++) {
            if (fa * h * (double)rate / n > kLongTopHz)
                break;
            pitches[fa * h] = true;
        }
        const long a0 = t - 3 > 0 ? t - 3 : 0, e0 = t + d + 3 < frames - 1 ? t + d + 3 : frames - 1;
        for (int b = 0; b < bins; b++) {
            if (!pitches[b])
                continue;
            for (int c = b - 2 > 1 ? b - 2 : 1; c < b + 3 && c < bins && c < width; c++) {
                long b0 = a0 - LONG_GAP - side > 0 ? a0 - LONG_GAP - side : 0, b1 = a0 - LONG_GAP > 1 ? a0 - LONG_GAP : 1;
                long c0 = e0 + LONG_GAP < frames - 1 ? e0 + LONG_GAP : frames - 1, c1 = e0 + LONG_GAP + side < frames ? e0 + LONG_GAP + side : frames;
                int held = 0;
                for (long u = b0; u < b1; u++)
                    edge[held++] = mag[u * width + c];
                int after_held = 0;
                float l0, l1;
                float *after = window;
                for (long u = c0; u < c1; u++)
                    after[after_held++] = mag[u * width + c];
                l1 = after_held ? Percentile(after, after_held, 40) : 0;
                l0 = held ? Percentile(edge, held, 40) : l1;
                if (!after_held)
                    l1 = l0;
                float most = 0;
                for (long u = a0; u <= e0; u++)
                    most = fmaxf(most, mag[u * width + c]);
                float sides = 20 * log10f(fmaxf(l0, l1) + 1e-12f);
                if (20 * log10f(most + 1e-12f) - sides < kLongGrow)
                    continue;  /* nothing of the sound at this pitch */
                const float ref = sides + kLongGrow;
                long a = a0, e = e0;
                const long lowest = t - d > 0 ? t - d : 0, highest = t + 2 * d < frames - 1 ? t + 2 * d : frames - 1;
                while (a > lowest && level[(a - 1) * width + c] > ref)
                    a--;
                while (e < highest && level[(e + 1) * width + c] > ref)
                    e++;
                for (long u = a; u <= e; u++) {
                    float lv = l0 + (l1 - l0) * (float)(u - a + 1) / (float)(e - a + 2);
                    float g = fminf(1.0f, lv / (mag[u * width + c] + 1e-12f));
                    if (g < gain[u * width + c])
                        gain[u * width + c] = g;
                }
            }
        }
    }
    free(taken);
    free(edge);
    ApplyGains(p, x, count, gain, width, frames, hop, y);
    ok = true;
done:
    free(mag);
    free(level);
    free(work);
    free(window);
    free(buffer);
    free(gain);
    free(events);
    return ok;
}

/* ---- round the loop, a piece at a time ------------------------------------------------------- */

typedef bool (*Piece)(const CleanPlan *plan, const float *x, long count, int rate, float *y);

/* In pieces of a minute that overlap and crossfade, to keep memory down, with a little of the
 * loop's other end either side so it goes round without a seam. */
static bool RoundTheLoop(Piece piece, const CleanPlan *plan, float *audio, long samples, int rate, bool (*stopping)(void)) {
    const long context = samples < 2L * rate ? samples : 2L * rate;
    const long total = samples + 2 * context, chunk = 60L * rate, overlap = 4L * rate;
    float *ext = (float *)malloc(sizeof(float) * (size_t)(total * CHANNELS));
    float *sum = (float *)calloc((size_t)(total * CHANNELS), sizeof(float));
    float *weight = (float *)calloc((size_t)total, sizeof(float));
    long starts = (total + chunk - 1) / chunk;
    if (starts > 1 && total - (starts - 1) * chunk < chunk / 4)
        starts--;  /* a short tail goes with the piece before it */
    /* The last piece is the longest: it takes that tail too. */
    const long last_from = (starts - 1) * chunk - overlap > 0 ? (starts - 1) * chunk - overlap : 0;
    const long longest = total - last_from > chunk + 2 * overlap ? total - last_from : chunk + 2 * overlap;
    float *out = (float *)malloc(sizeof(float) * (size_t)(longest * CHANNELS));
    bool ok = false;
    if (!ext || !sum || !weight || !out)
        goto done;
    for (long i = 0; i < total; i++) {
        long at = ((i - context) % samples + samples) % samples;
        ext[i * CHANNELS] = audio[at * CHANNELS];
        ext[i * CHANNELS + 1] = audio[at * CHANNELS + 1];
    }
    for (long i = 0; i < starts; i++) {
        if (stopping())
            goto done;
        long a = i * chunk, lo = a - overlap > 0 ? a - overlap : 0;
        long hi = i + 1 == starts ? total : (a + chunk + overlap < total ? a + chunk + overlap : total);
        if (!piece(plan, ext + lo * CHANNELS, hi - lo, rate, out))
            goto done;
        for (long j = 0; j < hi - lo; j++) {
            float ramp = 1.0f;
            if (lo > 0 && j < 2 * overlap)
                ramp = (float)j / (float)(2 * overlap - 1);
            if (hi < total && j >= hi - lo - 2 * overlap) {
                float down = 1.0f - (float)(j - (hi - lo - 2 * overlap)) / (float)(2 * overlap - 1);
                ramp = fminf(ramp, down);
            }
            sum[(lo + j) * CHANNELS] += out[j * CHANNELS] * ramp;
            sum[(lo + j) * CHANNELS + 1] += out[j * CHANNELS + 1] * ramp;
            weight[lo + j] += ramp;
        }
    }
    for (long i = 0; i < samples; i++) {
        float w = fmaxf(weight[context + i], 1e-9f);
        audio[i * CHANNELS] = sum[(context + i) * CHANNELS] / w;
        audio[i * CHANNELS + 1] = sum[(context + i) * CHANNELS + 1] / w;
    }
    ok = true;
done:
    free(ext);
    free(sum);
    free(weight);
    free(out);
    return ok;
}

bool CleanFilter(const CleanPlan *plan, float *audio, long samples, int rate, bool (*stopping)(void)) {
    if (!plan->sounds || (rate * 1024) % TOOL_RATE || samples < 2)
        return true;
    if (plan->filter && plan->sounds->count && !RoundTheLoop(FilterPiece, plan, audio, samples, rate, stopping))
        return false;
    if (plan->long_sounds && plan->sounds->long_count)
        return RoundTheLoop(LongPiece, plan, audio, samples, rate, stopping);
    return true;
}
