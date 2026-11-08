/* One program, three parts.
 *
 * Each part is its own recompiled module with its own image. When a part ends the
 * screen is already dark, so the machine is rebuilt around the next one between two
 * frames: the window, audio device and save file all stay open. SRAM carries the
 * score across, using the game's own logic.
 */
#include "session.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "endings.h"
#include "beam_frame_driver.h"
#include "bsx_wram.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "host_paths.h"
#include "launcher_cache.h"
#include "art.h"
#include "title.h"
#include "options.h"
#include "pacing.h"
#include "program_module.h"
#include "snes/snes.h"
#include "snes_osd.h"
#include "soundtrack.h"
#include "text.h"

typedef struct Part {
    const char *key;
    const char *module_id;
} Part;

static const Part kParts[PART_COUNT] = {
    [PART_ZENPEN] = {"zen", "main"},
    [PART_CHUUHEN] = {"chu", "chu"},
    [PART_KOUHEN] = {"kou", "kou"},
};

static int g_part;
static bool g_next_part_requested;
static bool g_start_checked;
static bool g_frames_run;  /* the title screen is for the session's start only */
static char g_image_dir[1024] = ".";
static bool g_image_dir_known;
static char g_start_from[16] = "continue";

/* The last folder separator, which on Windows may be either kind: _fullpath gives '\\'. */
static const char *LastSlash(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash))
        slash = backslash;
#endif
    return slash;
}

void SessionSetLaunchedRom(const char *path) {
    /* The host may change directory, so hold on to where this really is. */
    char absolute[sizeof(g_image_dir)];
#ifdef _WIN32
    if (!_fullpath(absolute, path, sizeof(absolute)))
        return;
#else
    char resolved[PATH_MAX];
    if (!realpath(path, resolved) || strlen(resolved) >= sizeof(absolute))
        return;
    strcpy(absolute, resolved);
#endif
    char *slash = (char *)LastSlash(absolute);
    if (slash) {
        *slash = 0;
        strcpy(g_image_dir, absolute);
        g_image_dir_known = true;
    }
}

static bool IsPlayerRun(void);

void SessionOnMachineBuilt(void) {
    TextSetLanguage(OptionsLanguage());
    TextOnImageLoaded(g_part);
    ArtLoad(OptionsLanguage(), g_part);
    EndingsOnImageLoaded(g_part);
    TitleOnImageLoaded(g_part, !g_frames_run && (IsPlayerRun() || getenv("PVS_TITLE")));
}

void SessionRequestNextPart(void) { g_next_part_requested = true; }

void SessionSetStart(const char *from) { snprintf(g_start_from, sizeof(g_start_from), "%s", from); }

static void ResumePath(char *path, size_t size) { snprintf(path, size, "%s/resume.sav", RtlSaveRoot()); }

/* No resume when something is driving the game: tests must start from power-on. */
static bool IsPlayerRun(void) {
    const char *video = getenv("SDL_VIDEODRIVER");
    return !(video && strcmp(video, "dummy") == 0);
}

void SessionOnShutdown(void) {
    if (!IsPlayerRun())
        return;
    char path[1100];
    ResumePath(path, sizeof(path));
    RtlSaveSnapshot(path);
}

static uint8 *ReadPartImage(int part, size_t *size) {
    char path[1100];
    snprintf(path, sizeof(path), "%s/%s.sfc", g_image_dir, kParts[part].key);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[session] cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8 *image = length > 0 ? (uint8 *)malloc((size_t)length) : NULL;
    if (image && fread(image, 1, (size_t)length, f) != (size_t)length) {
        free(image);
        image = NULL;
    }
    fclose(f);
    *size = (size_t)length;
    return image;
}

/* Rebuild the machine around a part. Only between frames, never from guest code. */
static bool SwitchToPart(int part) {
    const SnesProgramModule *module = snes_program_module_find(kParts[part].module_id);
    size_t size = 0;
    uint8 *image = ReadPartImage(part, &size);
    if (!module || !image) {
        fprintf(stderr, "[session] part %s is not available\n", kParts[part].key);
        free(image);
        return false;
    }
    if (!snes_program_module_rom_matches(module, image, size)) {
        fprintf(stderr, "[session] %s.sfc is not the image this build was generated from\n",
                kParts[part].key);
        free(image);
        return false;
    }
    fprintf(stderr, "[session] switching to part %s\n", kParts[part].key);
    RtlApuLock();
    RtlWriteSram();
    snes_free(g_snes);
    snes_program_module_select(module);
    if (g_rtl_game_info->session_reset)
        g_rtl_game_info->session_reset();
    g_part = part;
    Snes *snes = SnesInit(image, (int)size);
    if (snes)
        RtlReadSram();
    RtlApuUnlock();
    free(image);
    if (!snes)
        Die("Could not start the next part.");
    return true;
}

/* Without --rom the launcher chose it, or the host found it: the last ROM used is in the
 * launcher's cache, and otherwise the images are beside or just above the executable. */
static void FindImageDir(void) {
    if (g_image_dir_known)
        return;
    char path[1024];
    if (snesrecomp_rom_cache_read(path, sizeof(path)) && path[0]) {
        SessionSetLaunchedRom(path);
        if (g_image_dir_known)
            return;
    }
    static const char *const kLeaves[] = {"images/zen.sfc", "../images/zen.sfc"};
    for (size_t i = 0; i < sizeof(kLeaves) / sizeof(kLeaves[0]); i++) {
        if (snesrecomp_exe_dir_path(kLeaves[i], path, sizeof(path))) {
            FILE *f = fopen(path, "rb");
            if (f) {
                fclose(f);
                SessionSetLaunchedRom(path);
                return;
            }
        }
    }
}

void SessionCheckFiles(SessionFiles *files) {
    memset(files, 0, sizeof(*files));
    /* Look without settling where the images are: the launcher may pick another ROM yet. */
    char saved_dir[sizeof(g_image_dir)];
    bool saved_known = g_image_dir_known;
    strcpy(saved_dir, g_image_dir);
    FindImageDir();
    char dir[sizeof(g_image_dir)];
    bool known = g_image_dir_known;
    strcpy(dir, g_image_dir);
    strcpy(g_image_dir, saved_dir);
    g_image_dir_known = saved_known;
    if (!known)
        return;
    char root[1024], path[1200];
    const char *slash = LastSlash(dir);
    snprintf(root, sizeof(root), "%.*s", slash ? (int)(slash - dir) : 1, slash ? dir : ".");
    for (int part = 0; part < PART_COUNT; part++) {
        snprintf(path, sizeof(path), "%s/%s.sfc", dir, kParts[part].key);
        FILE *f = fopen(path, "rb");
        if (f) {
            files->image[part] = true;
            fclose(f);
        }
        snprintf(path, sizeof(path), "%s/audio/%s.mp3", root, kParts[part].key);
        f = fopen(path, "rb");
        if (f) {
            files->recording[part] = true;
            fclose(f);
        }
    }
    snprintf(path, sizeof(path), "%s/inputs", root);
    AudioFindRecordings(path, files->recording);
    AudioFindRecordings(dir, files->recording);
}

const char *SessionPartName(int part) {
    static const char *const kNames[PART_COUNT] = {"Zenpen", "Chuuhen", "Kouhen"};
    return part >= 0 && part < PART_COUNT ? kNames[part] : "";
}

/* Images live in <folder>/images, recordings in <folder>/inputs or beside the images, and
 * the audio taken from them in <folder>/audio. */
static void ImportRecordings(void) {
    char root[1024], audio_dir[1100], inputs[1100];
    const char *slash = LastSlash(g_image_dir);
    snprintf(root, sizeof(root), "%.*s", slash ? (int)(slash - g_image_dir) : 1, slash ? g_image_dir : ".");
    snprintf(audio_dir, sizeof(audio_dir), "%s/audio", root);
    snprintf(inputs, sizeof(inputs), "%s/inputs", root);
    AudioImportRecordings(inputs, audio_dir);
    AudioImportRecordings(g_image_dir, audio_dir);
    AudioSetDirectory(audio_dir);
    char soundtrack_dir[1100];
    snprintf(soundtrack_dir, sizeof(soundtrack_dir), "%s/soundtrack", root);
    SoundtrackSetDirectory(soundtrack_dir);
}

static void ApplyStart(void) {
    FindImageDir();
    ImportRecordings();
    /* Development aids: PVS_PACING=unlimited and PVS_START_PART=<key>. */
    const char *pacing = getenv("PVS_PACING");
    if (pacing && strcmp(pacing, "unlimited") == 0)
        PacingSetMode(PACING_UNLIMITED);
    AudioSetSource(OptionsAudioSource());
    SoundtrackSetVersion(OptionsSoundtrack());
    const char *from = getenv("PVS_START_PART");
    from = from ? from : g_start_from;
    for (int part = PART_ZENPEN; part < PART_COUNT; part++) {
        if (strcmp(from, kParts[part].key) == 0) {
            if (part != g_part)
                SwitchToPart(part);
            return;
        }
    }
    if (IsPlayerRun()) {
        char path[1100];
        ResumePath(path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            if (RtlLoadSnapshot(path)) {
                fprintf(stderr, "[session] resumed from %s\n", path);
                TitleCancel();
            }
        }
    }
}

void SessionBeforeFrame(void) {
    OptionsBeforeFrame();
    ArtOnFrame();
    if (!g_start_checked) {
        g_start_checked = true;
        ApplyStart();
        OptionsChooseBoxArt();
    }
    g_frames_run = true;
    if (g_next_part_requested) {
        g_next_part_requested = false;
        if (g_part + 1 < PART_COUNT) {
            SwitchToPart(g_part + 1);
        } else {
            /* Replaced by the ending flow once the host menu exists. */
            fprintf(stderr, "[session] the story is finished\n");
            RtlWriteSram();
            exit(0);
        }
    }
    g_ram[WRAM_PVS_PART] = (uint8)g_part;
    AudioOnFrame(g_ram, g_part, PacingIsHolding(), snes_osd_turbo() != 0);
}

bool SessionSnapshotPreflight(const void *data, size_t size) {
    if (!snes_beam_frame_driver_state_preflight(data, size))
        return false;
    uint32 version;
    size_t wram_offset = 0;
    if (!RtlSnapshotGuestLayout(&version, &wram_offset) || wram_offset + 0x20000 > size)
        return false;
    int part = ((const uint8 *)data)[wram_offset + WRAM_PVS_PART];
    if (part >= PART_COUNT)
        return false;
    return part == g_part || SwitchToPart(part);
}
