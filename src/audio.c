/* The broadcast's radio audio.
 *
 * The original stream ran alongside the game and took no notice of what the player did,
 * so a recording is simply a timeline: each part has straight lines from the broadcast
 * clock to places in its recording (data/cues). Every frame, the player asks where the
 * clock says the audio should be and steers towards it, so pausing, fast-forward, save
 * states and rewind all land in the right place with no state of their own.
 *
 * The investigation music repeats exactly, and the person who recorded it was playing
 * along, so the recording has their sound effects in it. Each repeat has them in different
 * places, so for each slice of a loop the cue file names the repeat that's cleanest there.
 * Playback works in "logical" recording time, which maps to whichever repeat is the clean
 * one. When unlimited mode stops the clock, logical time just goes round the loop.
 *
 * A background thread then makes each loop cleaner still (src/loop_render.c) and keeps the
 * result beside the recordings. Once a loop's is ready, it's what plays. Until then, or if
 * a recording it needs is missing, the takes do.
 */
#include "audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_reader.h"
#include "bsx_wram.h"
#include "desktop/sdl_compat.h"
#include "host_paths.h"
#include "loop_render.h"
#include "pacing.h"
#include "session.h"
#include "soundtrack.h"

enum {
    CHANNELS = READER_CHANNELS,
    FRAMES_PER_MINUTE = 3606,
    RENDER_VERSION = 3,
    MAX_PASS_SEGMENTS = 256,
    MAX_PATCH_LINES = 8,
    PATCH_GROW = 256,
    RENDERED_BLOCK_SAMPLES = 2048,
    MAX_MAP_SEGMENTS = 16,
    MAX_LOOPS = 32,
    MAX_KEPT = 128,
    EVENT_RUNNING = 0xFF,
    CLOCK_MINUTE_ROLLED = 0x40,
    BROADCAST_HOUR = 18,
};

static const char *const kPartKeys[PART_COUNT] = {"zen", "chu", "kou"};
static const double kQueueSeconds = 0.20;
static const double kResyncSeconds = 0.25;
static const double kNudgeSeconds = 0.03;
static const float kNudgeRatio = 0.005f;
static const double kFramesPerSecond = 60.0988;
static const float kFadeStep = 1.0f / 15.0f;

/* Part of the broadcast clock: clock = rate * media + offset, from this clock time on. */
typedef struct MapSegment {
    double clock_from, rate, offset;
} MapSegment;

/* Where to read a stretch of a loop's phase from: the start of the repeat that's cleanest
 * there, which may be anywhere in the recording, or in another part's. */
typedef struct Take {
    double phase, length, source;
    int part;       /* -1 for this part's own recording */
    float gain[CHANNELS];  /* to match this part's level: a track was mixed differently in places */
} Take;

/* A stretch of the recording where the same music repeats. Times are recording seconds. */
typedef struct Loop {
    double start, end, period;
    Take *takes;
    int take_count;
    RenderPass *passes;  /* every pass of this track, for the background render */
    int pass_count;
    void *rendered;      /* its clean pass once that's ready, set atomically */
    double fade;         /* a track that never truly repeats: seconds to cross into its start */
    RenderPatch *patches;  /* the player's sounds on it, for the background render */
    int patch_count;
    float tone[PART_COUNT][TONE_BANDS];  /* another recording's tone against this part's */
    bool has_tone[PART_COUNT];
    RenderRange *as_is;    /* stretches the background render leaves exactly as the takes are */
    double opening_ends;   /* where the dialogue its scene opens with is over */
    double closing_starts; /* and where the next long stretch of it starts */
    int as_is_count;
    CleanPlan clean;       /* cleaning its one take: see src/loop_clean.c */
} Loop;

/* A stretch inside a loop to leave as recorded: a voice or a cue over the music. */
typedef struct Kept {
    double from, to;
} Kept;

typedef struct AudioPart {
    bool loaded, missing;
    Recording recording;
    MapSegment map[MAX_MAP_SEGMENTS];
    int map_count;
    Loop loops[MAX_LOOPS];
    int loop_count;
    Kept kept[MAX_KEPT];
    int kept_count;
    double stop_at;   /* from here on the recording has nothing the game should play */
    double live_from; /* from here on it has none of the game's own sounds in it */
} AudioPart;

static AudioPart g_parts[PART_COUNT];
static CleanSounds g_sounds[PART_COUNT];  /* the player's sounds, for cleaning one take */

static bool Cleans(const Loop *loop) {
    return loop->clean.filter || loop->clean.long_sounds || loop->clean.copy_count || loop->clean.bar_count;
}

/* Cleaning that stands in for a second take, so the first time through can play it too. */
static bool CleansTheTake(const Loop *loop) { return loop->clean.filter || loop->clean.copy_count || loop->clean.bar_count; }
static char g_audio_dir[1024] = "audio";
static SDL_AudioStream *g_stream;
static int g_stream_rate;
static int g_current_part = -1;
static Reader g_reader;
static double g_logical;      /* the recording time of the next sample to decode */
static bool g_positioned;     /* false: must start from where the clock says */
static long g_block_samples;  /* left to play before the next look at the cue data */
static float g_read_gain[CHANNELS] = {1.0f, 1.0f};
static const int16 *g_rendered;  /* when a loop's clean pass is what's playing */
static long g_rendered_at, g_rendered_length;
static float g_gain = 1.0f;
static float g_soundtrack;   /* how far a soundtrack version has faded in over this */
static SDL_Thread *g_render_thread;
static SDL_AtomicInt g_render_stop;
static bool g_use_recording = true;

void AudioSetDirectory(const char *audio_dir) { snprintf(g_audio_dir, sizeof(g_audio_dir), "%s", audio_dir); }

static int PartOfKey(const char *key) {
    for (int i = 0; i < PART_COUNT; i++) {
        if (strcmp(key, kPartKeys[i]) == 0)
            return i;
    }
    return -1;
}

/* "patch = <from> <to>" and then the lines above the low band, in Hz. */
static void ReadPatch(Loop *loop, const char *text) {
    double from, to;
    int used = 0;
    if (sscanf(text, "patch = %lf %lf%n", &from, &to, &used) != 2)
        return;
    float *lines = (float *)malloc(sizeof(float) * MAX_PATCH_LINES);
    int count = 0, more = 0;
    float line;
    for (const char *at = text + used; count < MAX_PATCH_LINES && sscanf(at, "%f%n", &line, &more) == 1; at += more)
        lines[count++] = line;
    if (loop->patch_count % PATCH_GROW == 0)
        loop->patches = (RenderPatch *)realloc(loop->patches, sizeof(RenderPatch) * (size_t)(loop->patch_count + PATCH_GROW));
    loop->patches[loop->patch_count++] = (RenderPatch){from, to, lines, count};
}

/* "copies = <lag> ..." or "bars = <lag> ...": where the music comes round again, in seconds. */
static void ReadLags(const char *text, double *lags, int *count) {
    const char *at = strchr(text, '=');
    double lag;
    int more = 0;
    for (at = at ? at + 1 : text; *count < CLEAN_MOST_LAGS && sscanf(at, "%lf%n", &lag, &more) == 1; at += more)
        lags[(*count)++] = lag;
}

/* data/cues/sounds.toml: "<kind> = <part> <name> <seconds heard> <hz>[:<db>] ...", where the
 * kind is sound, typing or long. */
static void ReadSounds(void) {
    static bool read;
    if (read)
        return;
    read = true;
    char path[1200], line[1024];
    if (!snesrecomp_exe_dir_path("data/cues/sounds.toml", path, sizeof(path)))
        snprintf(path, sizeof(path), "data/cues/sounds.toml");
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[audio] no %s: one-take tracks stay as recorded\n", path);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char kind[8], key[8];
        CleanSound sound = {0};
        int used = 0;
        if (sscanf(line, "%7s = %7s %7s %lf%n", kind, key, sound.name, &sound.core, &used) != 4)
            continue;
        int part = PartOfKey(key);
        bool is_long = strcmp(kind, "long") == 0;
        if (part < 0 || (!is_long && strcmp(kind, "sound") != 0 && strcmp(kind, "typing") != 0))
            continue;
        sound.typing = strcmp(kind, "typing") == 0;
        const char *at = line + used;
        float hz, db;
        int more = 0;
        while (sound.count < CLEAN_PITCHES && sscanf(at, "%f%n", &hz, &more) == 1) {
            at += more;
            db = 0;
            if (*at == ':' && sscanf(at + 1, "%f%n", &db, &more) == 1)
                at += 1 + more;
            sound.hz[sound.count] = hz;
            sound.db[sound.count++] = db;
        }
        CleanSounds *set = &g_sounds[part];
        CleanSound **list = is_long ? &set->longs : &set->sounds;
        int *count = is_long ? &set->long_count : &set->count;
        CleanSound *grown = (CleanSound *)realloc(*list, sizeof(CleanSound) * (size_t)(*count + 1));
        if (!grown)
            break;
        grown[(*count)++] = sound;
        *list = grown;
    }
    fclose(f);
}

/* "tone = <part>" and a gain in dB for each band: see TONE_BANDS. */
static void ReadTone(Loop *loop, const char *text) {
    char key[8];
    int used = 0;
    if (sscanf(text, "tone = %3s%n", key, &used) != 1)
        return;
    int part = PartOfKey(key);
    if (part < 0)
        return;
    float values[TONE_BANDS];
    int count = 0, more = 0;
    for (const char *at = text + used; count < TONE_BANDS && sscanf(at, "%f%n", &values[count], &more) == 1; at += more)
        count++;
    if (count != TONE_BANDS)
        return;
    memcpy(loop->tone[part], values, sizeof(values));
    loop->has_tone[part] = true;
}

/* "pass = <part> <left gain> <right gain>" and then <phase> <offset> pairs. */
static void ReadPass(Loop *loop, const char *text) {
    char key[8];
    float left, right;
    int used = 0;
    if (sscanf(text, "pass = %3s %f %f%n", key, &left, &right, &used) != 3)
        return;
    int part = PartOfKey(key);
    if (part < 0)
        return;
    RenderSegment *segments = (RenderSegment *)malloc(sizeof(RenderSegment) * MAX_PASS_SEGMENTS);
    int count = 0;
    double phase, offset;
    int more = 0;
    for (const char *at = text + used; count < MAX_PASS_SEGMENTS && sscanf(at, "%lf %lf%n", &phase, &offset, &more) == 2; at += more)
        segments[count++] = (RenderSegment){phase, offset};
    loop->passes = (RenderPass *)realloc(loop->passes, sizeof(RenderPass) * (size_t)(loop->pass_count + 1));
    loop->passes[loop->pass_count++] = (RenderPass){&g_parts[part].recording, {left, right}, segments, count};
}

static bool ReadCues(int part, AudioPart *p) {
    char leaf[64], path[1200], line[8192];
    snprintf(leaf, sizeof(leaf), "data/cues/%s.toml", kPartKeys[part]);
    if (!snesrecomp_exe_dir_path(leaf, path, sizeof(path)))
        snprintf(path, sizeof(path), "%s", leaf);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[audio] no %s\n", path);
        return false;
    }
    double rate = 0, offset = 0;
    char key[8] = "";
    float left = 1.0f, right = 1.0f;
    while (fgets(line, sizeof(line), f)) {
        double a, b, c;
        Loop *loop = p->loop_count ? &p->loops[p->loop_count - 1] : NULL;
        if (sscanf(line, "rate = %lf", &a) == 1) {
            rate = a;
        } else if (sscanf(line, "offset = %lf", &a) == 1) {
            offset = a;
        } else if (sscanf(line, "stop = %lf", &a) == 1) {
            p->stop_at = a;
        } else if (sscanf(line, "live = %lf", &a) == 1) {
            p->live_from = a;
        } else if (sscanf(line, "map = %lf %lf %lf", &a, &b, &c) == 3 && p->map_count < MAX_MAP_SEGMENTS) {
            p->map[p->map_count++] = (MapSegment){a, b, c};
        } else if (sscanf(line, "loop = %lf %lf %lf", &a, &b, &c) == 3 && p->loop_count < MAX_LOOPS) {
            p->loops[p->loop_count++] = (Loop){.start = a, .end = b, .period = c};
        } else if (sscanf(line, "fade = %lf", &a) == 1 && loop) {
            loop->fade = a;
        } else if (strncmp(line, "patch = ", 8) == 0 && loop) {
            ReadPatch(loop, line);
        } else if (strncmp(line, "pass = ", 7) == 0 && loop) {
            ReadPass(loop, line);
        } else if (strncmp(line, "tone = ", 7) == 0 && loop) {
            ReadTone(loop, line);
        } else if (strncmp(line, "clean = filter", 14) == 0 && loop) {
            loop->clean.filter = loop->clean.long_sounds = true;
        } else if (strncmp(line, "clean = long", 12) == 0 && loop) {
            loop->clean.long_sounds = true;
        } else if (strncmp(line, "copies = ", 9) == 0 && loop) {
            ReadLags(line, loop->clean.copies, &loop->clean.copy_count);
        } else if (strncmp(line, "bars = ", 7) == 0 && loop) {
            ReadLags(line, loop->clean.bars, &loop->clean.bar_count);
        } else if (sscanf(line, "asis = %lf %lf", &a, &b) == 2 && loop) {
            loop->as_is = (RenderRange *)realloc(loop->as_is, sizeof(RenderRange) * (size_t)(loop->as_is_count + 1));
            loop->as_is[loop->as_is_count++] = (RenderRange){a, b};
        } else if (sscanf(line, "take = %lf %lf %lf %3s %f %f", &a, &b, &c, key, &left, &right) >= 3 && loop) {
            int source = PartOfKey(key);
            loop->takes = (Take *)realloc(loop->takes, sizeof(Take) * (size_t)(loop->take_count + 1));
            loop->takes[loop->take_count++] = (Take){a, b, c, source, {left, right}};
            key[0] = 0;
            left = right = 1.0f;
        } else if (sscanf(line, "keep = %lf %lf", &a, &b) == 2 && p->kept_count < MAX_KEPT) {
            p->kept[p->kept_count++] = (Kept){a, b};
        }
    }
    fclose(f);
    ReadSounds();
    for (int i = 0; i < p->loop_count; i++)
        p->loops[i].clean.sounds = &g_sounds[part];
    if (!p->map_count && rate > 0)
        p->map[p->map_count++] = (MapSegment){0, rate, offset};
    return p->map_count > 0;
}

/* A soundtrack version waits for the dialogue a scene opens with: lines kept as recorded that
 * run on, with no more than a short pause, from the loop's start. */
static double OpeningEnds(const AudioPart *p, const Loop *loop) {
    enum { MOST_PASSES = 64 };
    static const double kPause = 2.0, kAfter = 0.5;
    double at = loop->start;
    for (int pass = 0; pass < MOST_PASSES; pass++) {
        bool moved = false;
        for (int i = 0; i < p->kept_count; i++) {
            if (p->kept[i].from <= at + kPause && p->kept[i].to > at) {
                at = p->kept[i].to;
                moved = true;
            }
        }
        if (!moved)
            break;
    }
    return at == loop->start ? at : at + kAfter;
}

/* A soundtrack version gives way to the next real stretch of dialogue. Short lines go under it. */
static double ClosingStarts(const AudioPart *p, const Loop *loop) {
    static const double kLongLine = 4.0;
    double at = loop->end;
    for (int i = 0; i < p->kept_count; i++) {
        const Kept *k = &p->kept[i];
        if (k->from >= loop->opening_ends && k->from < at && k->to - k->from >= kLongLine)
            at = k->from;
    }
    return at;
}

static bool LoadPart(int part) {
    AudioPart *p = &g_parts[part];
    if (p->loaded || p->missing)
        return p->loaded;
    p->missing = true;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s.mp3", g_audio_dir, kPartKeys[part]);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    if (!ReadCues(part, p) || !RecordingLoad(&p->recording, path))
        return false;
    for (int i = 0; i < p->loop_count; i++) {
        p->loops[i].opening_ends = OpeningEnds(p, &p->loops[i]);
        p->loops[i].closing_starts = ClosingStarts(p, &p->loops[i]);
    }
    p->missing = false;
    p->loaded = true;
    fprintf(stderr, "[audio] %s: %d frames at %d Hz\n", kPartKeys[part], p->recording.frame_count, p->recording.sample_rate);
    return true;
}

static bool OpenStream(int rate) {
    if (g_stream && g_stream_rate == rate)
        return true;
    if (g_stream)
        SDL_DestroyAudioStream(g_stream);
    SDL_AudioSpec spec = {SDL_AUDIO_S16, CHANNELS, rate};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    g_stream_rate = rate;
    if (!g_stream) {
        fprintf(stderr, "[audio] no audio stream: %s\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    return true;
}

static double ClockToMedia(const AudioPart *p, double clock) {
    const MapSegment *segment = &p->map[0];
    for (int i = 1; i < p->map_count; i++) {
        if (clock >= p->map[i].clock_from)
            segment = &p->map[i];
    }
    return (clock - segment->offset) / segment->rate;
}

static const Loop *LoopAt(const AudioPart *p, double media) {
    for (int i = 0; i < p->loop_count; i++) {
        if (media >= p->loops[i].start && media < p->loops[i].end)
            return &p->loops[i];
    }
    return NULL;
}

/* A track with only one take, once cleaned, plays its render the first time through as well:
 * there the render is the recording, cleaned, up to where its end starts to cross back into its
 * start. Until then, or -1 if not here. */
static double CleanedUntil(const AudioPart *p, double logical) {
    const Loop *loop = LoopAt(p, logical);
    if (!loop || !CleansTheTake(loop) || loop->take_count != 1 || loop->pass_count >= 2 ||
        !SDL_GetAtomicPointer((void **)&loop->rendered))
        return -1;
    const Take *take = &loop->takes[0];
    const double from = take->source - take->phase, until = from + loop->period - loop->fade;
    if (take->part >= 0 || fabs(remainder(from - loop->start, loop->period)) > 1e-6 || logical < from || logical >= until)
        return -1;
    return until;
}

/* Where to read for a logical time, and how long that holds before looking again. Takes
 * are cut at particular samples, so this has to land on them exactly. Holding keeps to the
 * music: a voice over one pass of it is only for the first time through. */
static double PhysicalMedia(const AudioPart *p, double logical, const Loop *holding, const AudioPart **from, double *hold_for) {
    enum { A_LONG_TIME = 3600 };
    const int rate = p->recording.sample_rate;
    const double half_sample = 0.5 / rate;
    *hold_for = A_LONG_TIME;
    *from = p;
    g_read_gain[0] = g_read_gain[1] = 1.0f;
    g_rendered = NULL;
    const double cleaned_until = holding ? -1 : CleanedUntil(p, logical + half_sample);
    for (int i = 0; i < p->kept_count && !holding && cleaned_until < 0; i++) {
        if (logical + half_sample >= p->kept[i].from && logical + half_sample < p->kept[i].to) {
            *hold_for = p->kept[i].to - logical;
            return logical;
        }
    }
    /* A hold less than a period into its loop goes back to before the loop starts. */
    const Loop *loop = holding ? holding : LoopAt(p, logical);
    if (!loop || !loop->take_count) {
        for (int i = 0; i < p->loop_count; i++) {
            if (p->loops[i].start > logical && p->loops[i].start - logical < *hold_for)
                *hold_for = p->loops[i].start - logical;
        }
        return logical;
    }
    double phase = fmod(logical - loop->start + half_sample, loop->period);
    phase = (phase < 0 ? phase + loop->period : phase) - half_sample;
    const Take *take = NULL;
    const int16 *rendered = (const int16 *)SDL_GetAtomicPointer((void **)&loop->rendered);
    if (rendered) {
        g_rendered = rendered;
        g_rendered_length = lround(loop->period * rate);
        g_rendered_at = lround(phase * rate);
        *hold_for = loop->period - phase;
        if (cleaned_until >= 0 && cleaned_until - logical < *hold_for)
            *hold_for = cleaned_until - logical;
    } else {
        take = &loop->takes[loop->take_count - 1];
        for (int i = 0; i < loop->take_count; i++) {
            if (phase + half_sample < loop->takes[i].phase + loop->takes[i].length) {
                take = &loop->takes[i];
                break;
            }
        }
        *hold_for = take->phase + take->length - phase;
    }
    for (int i = 0; i < p->kept_count && !holding; i++) {
        if (p->kept[i].from > logical + half_sample && p->kept[i].from - logical < *hold_for)
            *hold_for = p->kept[i].from - logical;
    }
    if (!take)
        return logical;
    if (take->part >= 0) {
        /* Without that part's recording, this one's has to do. */
        if (!LoadPart(take->part))
            return logical;
        *from = &g_parts[take->part];
    }
    g_read_gain[0] = take->gain[0];
    g_read_gain[1] = take->gain[1];
    return take->source + phase;
}

/* Start playing from a logical time, dropping what's queued. */
static void Reposition(const AudioPart *p, double logical) {
    const int rate = p->recording.sample_rate;
    SDL_ClearAudioStream(g_stream);
    g_logical = round(logical * rate) / rate;
    g_block_samples = 0;
    g_positioned = true;
    g_rendered = NULL;
    ReaderReset(&g_reader);
}

static double QueuedSeconds(const AudioPart *p) {
    return SDL_GetAudioStreamQueued(g_stream) / (double)(CHANNELS * sizeof(int16) * (size_t)p->recording.sample_rate);
}

/* Where the audio that's playing right now has reached. */
static double PlayingMedia(const AudioPart *p) { return g_logical - QueuedSeconds(p); }

/* Development aid: PVS_TRACE_PCM=<file> keeps a copy of everything sent to the stream. */
static void TracePcm(const int16 *pcm, int samples) {
    static FILE *f;
    static int checked;
    if (!checked) {
        checked = 1;
        const char *path = getenv("PVS_TRACE_PCM");
        f = path ? fopen(path, "wb") : NULL;
    }
    if (f) {
        fwrite(pcm, sizeof(int16) * CHANNELS, (size_t)samples, f);
        fflush(f);
    }
}

/* Decode until there's enough queued. While the clock is held, logical time goes back a
 * period each time it reaches the hold, so the music carries on. */
static void FillQueue(const AudioPart *p, const Loop *hold_loop, double hold_at) {
    const int rate = p->recording.sample_rate;
    while (QueuedSeconds(p) < kQueueSeconds && !(p->stop_at > 0 && g_logical >= p->stop_at)) {
        if (hold_loop && g_logical >= hold_at) {
            g_logical = round((g_logical - hold_loop->period) * rate) / rate;
            g_block_samples = 0;
        }
        if (g_block_samples <= 0) {
            double hold_for;
            const AudioPart *from;
            double physical = PhysicalMedia(p, g_logical, hold_loop, &from, &hold_for);
            if (hold_loop && g_logical + hold_for > hold_at)
                hold_for = hold_at - g_logical;
            if (!g_rendered)
                ReaderSeek(&g_reader, &from->recording, rate, physical);
            g_block_samples = lround(hold_for * rate);
            if (g_block_samples < 1)
                g_block_samples = 1;
        }
        int take;
        const int16 *out;
        if (g_rendered) {
            long left = g_rendered_length - g_rendered_at;
            if (left < g_block_samples)
                g_block_samples = left > 0 ? left : 0;
            if (g_block_samples <= 0)
                continue;
            take = g_block_samples < RENDERED_BLOCK_SAMPLES ? (int)g_block_samples : RENDERED_BLOCK_SAMPLES;
            out = g_rendered + g_rendered_at * CHANNELS;
            g_rendered_at += take;
        } else {
            int16 *pcm;
            take = ReaderRead(&g_reader, &pcm, g_block_samples < READER_READY_SAMPLES ? (int)g_block_samples : READER_READY_SAMPLES);
            if (!take)
                return;
            if (g_read_gain[0] != 1.0f || g_read_gain[1] != 1.0f) {
                for (int i = 0; i < take * CHANNELS; i++) {
                    float value = pcm[i] * g_read_gain[i % CHANNELS];
                    pcm[i] = (int16)lrintf(value > 32767.0f ? 32767.0f : value < -32768.0f ? -32768.0f : value);
                }
            }
            out = pcm;
        }
        SDL_PutAudioStreamData(g_stream, out, take * CHANNELS * (int)sizeof(int16));
        TracePcm(out, take);
        g_logical += (double)take / rate;
        g_block_samples -= take;
    }
}

/* ---- the background render -------------------------------------------------------- */

typedef struct RenderedHeader {
    char magic[4];
    uint32 key, samples, rate;
} RenderedHeader;

static uint32 HashBytes(uint32 hash, const void *bytes, size_t size) {
    const uint8 *at = (const uint8 *)bytes;
    for (size_t i = 0; i < size; i++)
        hash = (hash ^ at[i]) * 16777619u;
    return hash;
}

/* Changes whenever the numbers behind a render do, so an old file isn't used. */
static uint32 LoopKey(const AudioPart *p, const Loop *loop) {
    uint32 hash = 2166136261u;
    const int version = RENDER_VERSION;
    hash = HashBytes(hash, &version, sizeof(version));
    hash = HashBytes(hash, &p->recording.sample_rate, sizeof(int));
    hash = HashBytes(hash, &loop->period, sizeof(double));
    if (loop->fade > 0)
        hash = HashBytes(hash, &loop->fade, sizeof(double));
    for (int i = 0; i < loop->patch_count; i++) {
        const RenderPatch *patch = &loop->patches[i];
        hash = HashBytes(hash, &patch->from, sizeof(double));
        hash = HashBytes(hash, &patch->to, sizeof(double));
        hash = HashBytes(hash, patch->lines, sizeof(float) * (size_t)patch->line_count);
    }
    for (int i = 0; i < loop->take_count; i++) {
        const Take *take = &loop->takes[i];
        hash = HashBytes(hash, &take->phase, sizeof(double));
        hash = HashBytes(hash, &take->source, sizeof(double));
        hash = HashBytes(hash, &take->part, sizeof(int));
        hash = HashBytes(hash, take->gain, sizeof(take->gain));
    }
    for (int part = 0; part < PART_COUNT; part++) {
        if (loop->has_tone[part]) {
            hash = HashBytes(hash, &part, sizeof(part));
            hash = HashBytes(hash, loop->tone[part], sizeof(loop->tone[part]));
        }
    }
    for (int i = 0; i < loop->as_is_count; i++)
        hash = HashBytes(hash, &loop->as_is[i], sizeof(RenderRange));
    if (Cleans(loop)) {
        const CleanPlan *clean = &loop->clean;
        hash = HashBytes(hash, &clean->filter, sizeof(clean->filter));
        hash = HashBytes(hash, &clean->long_sounds, sizeof(clean->long_sounds));
        hash = HashBytes(hash, clean->copies, sizeof(double) * (size_t)clean->copy_count);
        hash = HashBytes(hash, clean->bars, sizeof(double) * (size_t)clean->bar_count);
        const CleanSounds *set = clean->sounds;
        if ((clean->filter || clean->long_sounds) && set) {
            for (int i = 0; i < set->count + set->long_count; i++) {
                const CleanSound *sound = i < set->count ? &set->sounds[i] : &set->longs[i - set->count];
                hash = HashBytes(hash, sound->name, strlen(sound->name));
                hash = HashBytes(hash, &sound->core, sizeof(sound->core));
                hash = HashBytes(hash, &sound->typing, sizeof(sound->typing));
                hash = HashBytes(hash, sound->hz, sizeof(float) * (size_t)sound->count);
                hash = HashBytes(hash, sound->db, sizeof(float) * (size_t)sound->count);
            }
        }
    }
    for (int i = 0; i < loop->pass_count; i++) {
        const RenderPass *pass = &loop->passes[i];
        hash = HashBytes(hash, &pass->from->frame_count, sizeof(int));
        hash = HashBytes(hash, pass->gain, sizeof(pass->gain));
        hash = HashBytes(hash, pass->segments, sizeof(RenderSegment) * (size_t)pass->segment_count);
    }
    return hash;
}

static int16 *LoadRendered(const char *path, uint32 key, long samples, int rate) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    RenderedHeader header;
    int16 *pcm = NULL;
    if (fread(&header, sizeof(header), 1, f) == 1 && memcmp(header.magic, "PVSL", 4) == 0 && header.key == key &&
        header.samples == (uint32)samples && header.rate == (uint32)rate) {
        size_t size = (size_t)(samples * CHANNELS) * sizeof(int16);
        pcm = (int16 *)malloc(size);
        if (pcm && fread(pcm, 1, size, f) != size) {
            free(pcm);
            pcm = NULL;
        }
    }
    fclose(f);
    return pcm;
}

static void SaveRendered(const char *path, const int16 *pcm, uint32 key, long samples, int rate) {
    char temporary[1300];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *f = fopen(temporary, "wb");
    if (!f)
        return;
    RenderedHeader header = {{'P', 'V', 'S', 'L'}, key, (uint32)samples, (uint32)rate};
    bool wrote = fwrite(&header, sizeof(header), 1, f) == 1 &&
                 fwrite(pcm, sizeof(int16) * CHANNELS, (size_t)samples, f) == (size_t)samples;
    fclose(f);
    if (wrote)
        SDL_RenamePath(temporary, path);
    else
        SDL_RemovePath(temporary);
}

static bool RenderStopping(void) { return SDL_GetAtomicInt(&g_render_stop) != 0; }

static int16 *RenderLoop(const AudioPart *p, const Loop *loop, long samples) {
    int rate = p->recording.sample_rate;
    /* It's the same render or none: without a recording, the takes are still fine. */
    for (int i = 0; i < loop->pass_count; i++) {
        if (!loop->passes[i].from->frame_count)
            return NULL;
    }
    RenderTake *takes = (RenderTake *)malloc(sizeof(RenderTake) * (size_t)loop->take_count);
    if (!takes)
        return NULL;
    bool have_all = true;
    for (int i = 0; i < loop->take_count; i++) {
        const Take *take = &loop->takes[i];
        const AudioPart *from = take->part >= 0 ? &g_parts[take->part] : p;
        have_all = have_all && from->loaded;
        const float *tone = take->part >= 0 && loop->has_tone[take->part] ? loop->tone[take->part] : NULL;
        takes[i] = (RenderTake){take->phase, take->length, take->source, &from->recording, {take->gain[0], take->gain[1]}, tone};
    }
    RenderPlan plan = {rate, samples, takes, loop->take_count, loop->passes, loop->pass_count, loop->fade,
                       loop->patches, loop->patch_count, loop->as_is, loop->as_is_count, Cleans(loop) ? &loop->clean : NULL};
    int16 *pcm = have_all ? LoopRender(&plan, RenderStopping) : NULL;
    free(takes);
    return pcm;
}

/* Reads only what doesn't change once loaded, and hands each result over atomically. */
static int SDLCALL RenderThread(void *unused) {
    (void)unused;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
    char folder[1100];
    snprintf(folder, sizeof(folder), "%s/loops", g_audio_dir);
    SDL_CreateDirectory(folder);
    for (int part = 0; part < PART_COUNT; part++) {
        AudioPart *p = &g_parts[part];
        if (!p->loaded)
            continue;
        for (int i = 0; i < p->loop_count && !RenderStopping(); i++) {
            Loop *loop = &p->loops[i];
            if (!loop->take_count || (loop->pass_count < 2 && loop->fade <= 0))
                continue;
            int rate = p->recording.sample_rate;
            long samples = lround(loop->period * rate);
            uint32 key = LoopKey(p, loop);
            char path[1200];
            snprintf(path, sizeof(path), "%s/%s_%d.pcm", folder, kPartKeys[part], i);
            int16 *pcm = LoadRendered(path, key, samples, rate);
            if (!pcm) {
                Uint64 began = SDL_GetTicks();
                pcm = RenderLoop(p, loop, samples);
                if (pcm) {
                    SaveRendered(path, pcm, key, samples, rate);
                    fprintf(stderr, "[audio] rendered %s loop %d in %.1f s\n", kPartKeys[part], i, (SDL_GetTicks() - began) / 1000.0);
                }
            }
            if (pcm)
                SDL_SetAtomicPointer(&loop->rendered, pcm);
        }
    }
    return 0;
}

static void StartRendering(void) {
    static bool started;
    if (started)
        return;
    started = true;
    for (int part = 0; part < PART_COUNT; part++)
        LoadPart(part);
    g_render_thread = SDL_CreateThread(RenderThread, "loop render", NULL);
    /* Development aid: PVS_RENDER_ONLY=1 renders whatever's missing, then exits. */
    if (getenv("PVS_RENDER_ONLY")) {
        SDL_WaitThread(g_render_thread, NULL);
        exit(0);
    }
}

static void SetGain(float target) {
    if (g_gain < target)
        g_gain = g_gain + kFadeStep < target ? g_gain + kFadeStep : target;
    else if (g_gain > target)
        g_gain = g_gain - kFadeStep > target ? g_gain - kFadeStep : target;
    SDL_SetAudioStreamGain(g_stream, g_gain * (1.0f - g_soundtrack));
}

static void Silence(void) {
    SDL_ClearAudioStream(g_stream);
    g_positioned = false;
}

void AudioOnFrame(const uint8 *ram, int part, bool clock_held, bool turbo) {
    if (!g_use_recording || !LoadPart(part)) {
        SoundtrackStop();
        g_soundtrack = 0;
        if (g_stream)
            SDL_ClearAudioStream(g_stream);
        g_current_part = -1;
        return;
    }
    const AudioPart *p = &g_parts[part];
    if (!OpenStream(p->recording.sample_rate))
        return;
    StartRendering();
    if (part != g_current_part) {
        g_current_part = part;
        g_positioned = false;
    }
    /* The clock's first packet says one second before the start. */
    bool started = ram[WRAM_HOUR] >= BROADCAST_HOUR && (ram[WRAM_CLOCK_FLAGS] & CLOCK_MINUTE_ROLLED);
    /* A minute is 3606 frames, so counting frames keeps this smooth across each minute. */
    double clock = (ram[WRAM_MINUTE] * FRAMES_PER_MINUTE + ram[WRAM_SECOND] * 60 + ram[WRAM_FRAME]) / kFramesPerSecond;
    double media = ClockToMedia(p, clock);
    if (!started || turbo || media < 0) {
        SoundtrackStop();
        g_soundtrack = 0;
        Silence();
        return;
    }
    if (p->stop_at > 0 && media >= p->stop_at) {
        /* What's queued can finish: the recording has faded to nothing by here. */
        g_positioned = false;
        return;
    }
    const Loop *hold_loop = clock_held ? LoopAt(p, media) : NULL;
    /* A soundtrack version, once the scene's opening dialogue is over, until just before the
     * next long stretch of it or the scene moving on. A hold keeps it going. */
    static const double kSoundtrackFade = 1.5;
    const Loop *scene = hold_loop ? hold_loop : LoopAt(p, media);
    bool replace = scene && (clock_held || (media >= scene->opening_ends && media < scene->closing_starts - kSoundtrackFade));
    g_soundtrack = SoundtrackOnFrame(part, replace ? (int)(scene - p->loops) : -1);
    if (clock_held && !hold_loop) {
        /* No music to go round at this hold, so fade to quiet and pick up afterwards. */
        SetGain(0.0f);
        if (g_gain == 0.0f) {
            Silence();
            return;
        }
    } else if (!g_positioned) {
        Reposition(p, media);
        SetGain(1.0f);
    } else if (!clock_held) {
        double error = PlayingMedia(p) - media;
        if (fabs(error) > kResyncSeconds) {
            /* Dip through silence rather than cut, as this can be a whole loop away. */
            SetGain(0.0f);
            if (g_gain == 0.0f)
                Reposition(p, media);
        } else {
            SetGain(1.0f);
            SDL_SetAudioStreamFrequencyRatio(g_stream, fabs(error) < kNudgeSeconds ? 1.0f
                                                       : error > 0 ? 1.0f - kNudgeRatio : 1.0f + kNudgeRatio);
        }
    } else {
        SetGain(1.0f);
    }
    SDL_SetAudioStreamGain(g_stream, g_gain * (1.0f - g_soundtrack));
    FillQueue(p, hold_loop, media);
    /* Development aid: PVS_TRACE_AUDIO=1 shows how closely playback follows the clock. */
    static int trace = -1, frames;
    if (trace < 0)
        trace = getenv("PVS_TRACE_AUDIO") != NULL;
    if (trace && frames++ % 600 == 0)
        fprintf(stderr, "[audio] clock %.2f wants media %.2f, playing %.2f, queue %.3f\n", clock, media,
                PlayingMedia(p), QueuedSeconds(p));
}

/* Where the recording is by the clock in WRAM, or a long way off if it isn't playing. */
static double CurrentMedia(const uint8 *ram) {
    if (g_current_part < 0)
        return 1e9;
    return ClockToMedia(&g_parts[g_current_part],
                        (ram[WRAM_MINUTE] * FRAMES_PER_MINUTE + ram[WRAM_SECOND] * 60 + ram[WRAM_FRAME]) / kFramesPerSecond);
}

static bool RecordingIsLive(const uint8 *ram) {
    if (g_current_part < 0)
        return false;
    const AudioPart *p = &g_parts[g_current_part];
    return p->live_from <= 0 || CurrentMedia(ram) < p->live_from;
}

bool AudioRecordingCoversMusic(const uint8 *ram) { return RecordingIsLive(ram); }

bool AudioRecordingCoversEffects(const uint8 *ram) {
    return RecordingIsLive(ram) && (ram[WRAM_VAR_EVENT_STATE] == EVENT_RUNNING || !PacingSceneHasActions(ram));
}

void AudioSetSource(const char *source) { g_use_recording = strcmp(source, "rom") != 0; }

void AudioShutdown(void) {
    SoundtrackShutdown();
    SDL_SetAtomicInt(&g_render_stop, 1);
    if (g_render_thread)
        SDL_WaitThread(g_render_thread, NULL);
    g_render_thread = NULL;
    if (g_stream)
        SDL_DestroyAudioStream(g_stream);
    g_stream = NULL;
}
