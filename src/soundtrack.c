/* Soundtrack albums in place of some of the broadcast's investigation music.
 *
 * A few of the tunes behind the investigations are ones the composer also wrote for the
 * Famicom Detective Club games, and players who have those albums can hear them instead of
 * the recording, which has the original player's typing and beeps in it. The albums are the
 * player's own files, found by name and length (data/soundtrack/tracks.toml) anywhere under the
 * soundtrack folder. Nothing of them is in this repository.
 *
 * A version comes in once the dialogue that opens its scene is over and plays on, going round
 * its loop through any hold, until the scene is about to move on. It's never in step with the
 * clock: a different arrangement can't be, so it simply starts from the top each time.
 */
#include "soundtrack.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_reader.h"
#include "desktop/sdl_compat.h"
#include "host_paths.h"
#include "numbers.h"
#include "types.h"

enum {
    CHANNELS = READER_CHANNELS,
    MAX_TRACKS = 32,
    MAX_REPLACED = 32,
    PART_KEYS = 3,
};

static const double kQueueSeconds = 0.25;
static const double kLengthSlack = 0.3;  /* rips of one album differ a little in length */
static const float kFadeStep = 1.0f / 90.0f;  /* a second and a half */

typedef struct Track {
    char tune[16], version[16], name[128];
    double seconds, loop_from, fade;
    long loop_samples;
    int rate;
    bool looked, found;
    Recording recording;
    int16 *lead_in;  /* for a faded loop: what comes just before its start, to fade into */
} Track;

typedef struct Replaced {
    int part, loop;
    char tune[16];
} Replaced;

static Track g_tracks[MAX_TRACKS];
static int g_track_count;
static Replaced g_replaced[MAX_REPLACED];
static int g_replaced_count;
static bool g_data_read;
static char g_dir[1024] = "soundtrack";
static char g_version[16] = "recording";

static SDL_AudioStream *g_out;
static int g_out_rate;
static Track *g_playing;
static int g_playing_part = -1, g_playing_loop = -1;
static Reader g_reader;
static long g_at;  /* the next sample to queue, in the track */
static float g_gain;

static const char *const kPartKeys[PART_KEYS] = {"zen", "chu", "kou"};

static void ReadData(void) {
    if (g_data_read)
        return;
    g_data_read = true;
    char path[1200], line[512];
    if (!snesrecomp_exe_dir_path("data/soundtrack/tracks.toml", path, sizeof(path)))
        snprintf(path, sizeof(path), "data/soundtrack/tracks.toml");
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        Track t = {0};
        Replaced r = {0};
        char part[8];
        int used = 0;
        double n[5];
        const char *at;
        /* "track = <tune> <version> <seconds> <rate> <loop from> <loop samples> <fade> <name>" */
        if (g_track_count < MAX_TRACKS && sscanf(line, "track = %15s %15s%n", t.tune, t.version, &used) == 2 &&
            ReadNumbers(line + used, n, 5, &at) == 5) {
            t.seconds = n[0];
            t.rate = (int)n[1];
            t.loop_from = n[2];
            t.loop_samples = (long)n[3];
            t.fade = n[4];
            while (isspace((unsigned char)*at))
                at++;
            snprintf(t.name, sizeof(t.name), "%s", at);
            t.name[strcspn(t.name, "\r\n")] = 0;
            g_tracks[g_track_count++] = t;
        } else if (g_replaced_count < MAX_REPLACED && sscanf(line, "replace = %7s %d %15s", part, &r.loop, r.tune) == 3) {
            r.part = -1;
            for (int i = 0; i < PART_KEYS; i++) {
                if (strcmp(part, kPartKeys[i]) == 0)
                    r.part = i;
            }
            if (r.part >= 0)
                g_replaced[g_replaced_count++] = r;
        }
    }
    fclose(f);
}

static bool ContainsIgnoringCase(const char *text, const char *part) {
    size_t n = strlen(part);
    for (; *text; text++) {
        size_t i = 0;
        while (i < n && text[i] && tolower((unsigned char)text[i]) == tolower((unsigned char)part[i]))
            i++;
        if (i == n)
            return true;
    }
    return false;
}

/* The length an MP3's frames add up to, near enough to tell rips apart. */
static double Seconds(const Recording *r) { return r->sample_rate ? (double)r->frame_count * READER_FRAME_SAMPLES / r->sample_rate : 0; }

typedef struct Search {
    Track *track;
    int depth;
} Search;

static SDL_EnumerationResult SDLCALL Look(void *data, const char *dir, const char *name) {
    Search *search = (Search *)data;
    Track *t = search->track;
    char path[1200];
    snprintf(path, sizeof(path), "%s%s", dir, name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info))
        return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        if (search->depth < 4) {
            search->depth++;
            SDL_EnumerateDirectory(path, Look, search);
            search->depth--;
        }
        return t->found ? SDL_ENUM_SUCCESS : SDL_ENUM_CONTINUE;
    }
    size_t len = strlen(name);
    if (len < 4 || !ContainsIgnoringCase(name + len - 4, ".mp3") || !ContainsIgnoringCase(name, t->name))
        return SDL_ENUM_CONTINUE;
    Recording r;
    if (!RecordingLoad(&r, path))
        return SDL_ENUM_CONTINUE;
    if (r.sample_rate == t->rate && fabs(Seconds(&r) - t->seconds) < kLengthSlack) {
        t->recording = r;
        t->found = true;
        fprintf(stderr, "[soundtrack] %s (%s): %s\n", t->tune, t->version, path);
        return SDL_ENUM_SUCCESS;
    }
    free(r.data);
    free(r.offsets);
    return SDL_ENUM_CONTINUE;
}

static void ReadLeadIn(Track *t) {
    long count = lround(t->fade * t->rate);
    if (count <= 0)
        return;
    t->lead_in = (int16 *)calloc((size_t)(count * CHANNELS), sizeof(int16));
    if (!t->lead_in)
        return;
    Reader reader;
    ReaderReset(&reader);
    ReaderSeek(&reader, &t->recording, t->rate, t->loop_from - t->fade);
    for (long got = 0; got < count;) {
        int16 *pcm;
        int take = ReaderRead(&reader, &pcm, (int)(count - got < READER_READY_SAMPLES ? count - got : READER_READY_SAMPLES));
        if (!take)
            break;
        memcpy(t->lead_in + got * CHANNELS, pcm, sizeof(int16) * (size_t)(take * CHANNELS));
        got += take;
    }
}

static Track *TrackFor(int part, int loop) {
    ReadData();
    if (strcmp(g_version, "recording") == 0)
        return NULL;
    for (int i = 0; i < g_replaced_count; i++) {
        if (g_replaced[i].part != part || g_replaced[i].loop != loop)
            continue;
        for (int j = 0; j < g_track_count; j++) {
            Track *t = &g_tracks[j];
            if (strcmp(t->tune, g_replaced[i].tune) != 0 || strcmp(t->version, g_version) != 0)
                continue;
            if (!t->looked) {
                t->looked = true;
                Search search = {t, 0};
                SDL_EnumerateDirectory(g_dir, Look, &search);
                if (t->found)
                    ReadLeadIn(t);
            }
            if (t->found)
                return t;
        }
    }
    return NULL;
}

void SoundtrackSetDirectory(const char *dir) { snprintf(g_dir, sizeof(g_dir), "%s", dir); }

void SoundtrackSetVersion(const char *version) {
    if (strcmp(version, g_version) != 0)
        SoundtrackStop();
    snprintf(g_version, sizeof(g_version), "%s", version);
}

bool SoundtrackReplaces(int part, int loop) { return TrackFor(part, loop) != NULL; }

/* Queue enough to play on: round the loop, fading its end into what leads to its start. */
static void Fill(Track *t) {
    const long loop_from = lround(t->loop_from * t->rate), loop_to = loop_from + t->loop_samples;
    const long fade = lround(t->fade * t->rate);
    static int16 block[READER_READY_SAMPLES * CHANNELS];
    while (SDL_GetAudioStreamQueued(g_out) < (int)(kQueueSeconds * t->rate) * CHANNELS * (int)sizeof(int16)) {
        if (g_at >= loop_to) {
            g_at = loop_from;
            ReaderSeek(&g_reader, &t->recording, t->rate, (double)g_at / t->rate);
        }
        int16 *pcm;
        long most = loop_to - g_at < READER_READY_SAMPLES ? loop_to - g_at : READER_READY_SAMPLES;
        int take = ReaderRead(&g_reader, &pcm, (int)most);
        if (!take) {
            g_at = loop_to;
            continue;
        }
        memcpy(block, pcm, sizeof(int16) * (size_t)(take * CHANNELS));
        if (t->lead_in) {
            for (int i = 0; i < take; i++) {
                long into = g_at + i - (loop_to - fade);
                if (into < 0)
                    continue;
                float w = (float)into / (float)fade;
                for (int c = 0; c < CHANNELS; c++) {
                    float v = block[i * CHANNELS + c] * (1 - w) + t->lead_in[into * CHANNELS + c] * w;
                    block[i * CHANNELS + c] = (int16)lrintf(v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v);
                }
            }
        }
        SDL_PutAudioStreamData(g_out, block, take * CHANNELS * (int)sizeof(int16));
        g_at += take;
    }
}

static void Start(Track *t) {
    if (g_out && g_out_rate != t->rate) {
        SDL_DestroyAudioStream(g_out);
        g_out = NULL;
    }
    if (!g_out) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, CHANNELS, t->rate};
        g_out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        g_out_rate = t->rate;
        if (!g_out) {
            fprintf(stderr, "[soundtrack] no audio stream: %s\n", SDL_GetError());
            return;
        }
        SDL_ResumeAudioStreamDevice(g_out);
    }
    SDL_ClearAudioStream(g_out);
    ReaderReset(&g_reader);
    g_at = 0;
    ReaderSeek(&g_reader, &t->recording, t->rate, 0);
    g_playing = t;
    g_gain = 0;
}

float SoundtrackOnFrame(int part, int loop) {
    Track *want = loop >= 0 ? TrackFor(part, loop) : NULL;
    bool same = want && want == g_playing && part == g_playing_part && loop == g_playing_loop;
    if (g_playing && !same) {
        /* Out first; a new scene's version starts from the top once this one has gone. */
        g_gain = g_gain - kFadeStep > 0 ? g_gain - kFadeStep : 0;
        if (g_gain == 0) {
            fprintf(stderr, "[soundtrack] %s faded out\n", g_playing->tune);
            SoundtrackStop();
        }
    } else if (want && !g_playing) {
        Start(want);
        g_playing_part = part;
        g_playing_loop = loop;
        fprintf(stderr, "[soundtrack] %s (%s) in place of %s loop %d\n", want->tune, want->version, kPartKeys[part], loop);
    }
    if (!g_playing || !g_out)
        return 0;
    if (same)
        g_gain = g_gain + kFadeStep < 1 ? g_gain + kFadeStep : 1;
    SDL_SetAudioStreamGain(g_out, g_gain);
    Fill(g_playing);
    return g_gain;
}

void SoundtrackStop(void) {
    if (g_out)
        SDL_ClearAudioStream(g_out);
    g_playing = NULL;
    g_playing_part = g_playing_loop = -1;
    g_gain = 0;
}

void SoundtrackShutdown(void) {
    SoundtrackStop();
    if (g_out)
        SDL_DestroyAudioStream(g_out);
    g_out = NULL;
}
