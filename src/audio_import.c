/* Copy the MP3 stream out of a recording, byte for byte. Nothing is re-encoded, so this
 * takes seconds and adds no loss. Recordings are known by size, because the archive's
 * Chuuhen and Kouhen filenames are the wrong way round. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "desktop/sdl_compat.h"

typedef struct KnownRecording {
    Uint64 size;
    const char *part;
} KnownRecording;

static const KnownRecording kRecordings[] = {
    {544397818, "zen"},
    {973857624, "chu"},
    {664082876, "kou"},
};

enum { HEADER_BYTES = 1 << 20, COPY_BYTES = 1 << 16 };

static unsigned ReadLe32(const unsigned char *p) {
    return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

/* Which stream is audio, and where the data starts. A stream's number is how many "strh"
 * headers come before it. */
static bool ReadHeader(const unsigned char *data, size_t size, int *stream, Sint64 *movi) {
    int index = -1;
    *stream = -1;
    for (size_t pos = 12; pos + 12 < size; pos++) {
        if (memcmp(data + pos, "movi", 4) == 0) {
            *movi = (Sint64)pos;
            return *stream >= 0;
        }
        if (memcmp(data + pos, "strh", 4) == 0) {
            index++;
            if (memcmp(data + pos + 8, "auds", 4) == 0 && *stream < 0)
                *stream = index;
        }
    }
    return false;
}

static bool CopyChunks(SDL_IOStream *in, SDL_IOStream *out, const char *tag) {
    unsigned char chunk[8];
    unsigned char *buffer = malloc(COPY_BYTES);
    bool ok = buffer != NULL;
    while (ok && SDL_ReadIO(in, chunk, sizeof(chunk)) == sizeof(chunk)) {
        unsigned size = ReadLe32(chunk + 4);
        if (memcmp(chunk, "LIST", 4) == 0) {
            SDL_SeekIO(in, 4, SDL_IO_SEEK_CUR);
        } else if (memcmp(chunk, "idx1", 4) == 0) {
            break;
        } else if (memcmp(chunk, tag, 4) == 0) {
            for (unsigned left = size; left && ok;) {
                unsigned step = left < COPY_BYTES ? left : COPY_BYTES;
                ok = SDL_ReadIO(in, buffer, step) == step && SDL_WriteIO(out, buffer, step) == step;
                left -= step;
            }
            if (size & 1)
                SDL_SeekIO(in, 1, SDL_IO_SEEK_CUR);
        } else {
            SDL_SeekIO(in, (Sint64)size + (size & 1), SDL_IO_SEEK_CUR);
        }
    }
    free(buffer);
    return ok;
}

static bool ExtractStream(const char *avi_path, const char *out_path) {
    SDL_IOStream *in = SDL_IOFromFile(avi_path, "rb");
    if (!in)
        return false;
    unsigned char *header = malloc(HEADER_BYTES);
    size_t got = header ? SDL_ReadIO(in, header, HEADER_BYTES) : 0;
    int stream = -1;
    Sint64 movi = -1;
    bool ok = header && ReadHeader(header, got, &stream, &movi);
    free(header);
    char temp_path[1200], tag[5];
    snprintf(temp_path, sizeof(temp_path), "%s.part", out_path);
    snprintf(tag, sizeof(tag), "%02dwb", stream);
    SDL_IOStream *out = ok ? SDL_IOFromFile(temp_path, "wb") : NULL;
    ok = out != NULL;
    if (ok) {
        SDL_SeekIO(in, movi + 4, SDL_IO_SEEK_SET);
        ok = CopyChunks(in, out, tag);
        ok = SDL_CloseIO(out) && ok;
    }
    SDL_CloseIO(in);
    if (ok)
        ok = SDL_RenamePath(temp_path, out_path);
    else if (out)
        SDL_RemovePath(temp_path);
    return ok;
}

static void ImportIfKnown(const char *dir, const char *name, const char *audio_dir) {
    char path[1200], out[1200];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info))
        return;
    for (size_t i = 0; i < sizeof(kRecordings) / sizeof(kRecordings[0]); i++) {
        if (info.size != kRecordings[i].size)
            continue;
        snprintf(out, sizeof(out), "%s/%s.mp3", audio_dir, kRecordings[i].part);
        SDL_PathInfo existing;
        if (SDL_GetPathInfo(out, &existing))
            return;
        SDL_CreateDirectory(audio_dir);
        fprintf(stderr, "[audio] importing %s as %s\n", name, kRecordings[i].part);
        if (!ExtractStream(path, out))
            fprintf(stderr, "[audio] could not import %s\n", name);
        return;
    }
}

void AudioImportRecordings(const char *search_dir, const char *audio_dir) {
    static const char *const kPatterns[] = {"*.divx", "*.avi"};
    for (size_t p = 0; p < 2; p++) {
        int count = 0;
        char **names = SDL_GlobDirectory(search_dir, kPatterns[p], SDL_GLOB_CASEINSENSITIVE, &count);
        for (int i = 0; i < count; i++)
            ImportIfKnown(search_dir, names[i], audio_dir);
        SDL_free(names);
    }
}

void AudioFindRecordings(const char *search_dir, bool found[3]) {
    static const char *const kPatterns[] = {"*.divx", "*.avi"};
    for (size_t p = 0; p < 2; p++) {
        int count = 0;
        char **names = SDL_GlobDirectory(search_dir, kPatterns[p], SDL_GLOB_CASEINSENSITIVE, &count);
        for (int i = 0; i < count; i++) {
            char path[1200];
            snprintf(path, sizeof(path), "%s/%s", search_dir, names[i]);
            SDL_PathInfo info;
            if (!SDL_GetPathInfo(path, &info))
                continue;
            for (size_t r = 0; r < sizeof(kRecordings) / sizeof(kRecordings[0]) && r < 3; r++) {
                if (info.size == kRecordings[r].size)
                    found[r] = true;
            }
        }
        SDL_free(names);
    }
}
