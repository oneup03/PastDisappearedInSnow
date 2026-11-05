/* Satellaview hardware and BIOS services the game expects, reimplemented so it
 * runs with no BS-X BIOS and no base unit.
 *
 * The original gets the time from packets on a satellite data channel. The port
 * behaves like a perfect signal instead. All clock state stays in guest WRAM, so
 * save states and rewind capture it with everything else.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "audio.h"
#include "bsx_wram.h"
#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "mouse.h"
#include "pacing.h"
#include "title.h"
#include "session.h"
#include "snes/snes.h"

/* Bits of WRAM_CLOCK_FLAGS. The game's boot code waits for MINUTE_ROLLED and
 * shows a reception error if SIGNAL_LOST is set. */
enum {
    CLOCK_SYNCED = 0x01,
    CLOCK_FIRST_PACKET = 0x20,
    CLOCK_MINUTE_ROLLED = 0x40,
    CLOCK_SIGNAL_LOST = 0x80,
};

enum {
    FRAMES_PER_SECOND = 60,
    SECONDS_PER_MINUTE = 60,
    /* A real minute is 3606 frames at the SNES's 60.0988 Hz, so the game's frame
     * count runs 6 frames long before each minute's packet resets it. */
    FRAMES_PAST_MINUTE = 6,
};

/* The broadcast ran 6-7 PM. Only the minute and second drive the story. */
static const uint16 kStartYear = 1997;
static const uint8 kStartMonth = 2, kStartDay = 9, kStartWeekday = 0, kStartHour = 18;

/* The game's boot code waits for a minute to roll over, then joins the broadcast
 * wherever the clock says. So the first packet says one second before the start,
 * and the game joins at the very beginning. */
static void ClockReceiveFirstPacket(uint8 *ram) {
    ram[WRAM_YEAR] = (uint8)kStartYear;
    ram[WRAM_YEAR + 1] = (uint8)(kStartYear >> 8);
    ram[WRAM_MONTH] = kStartMonth;
    ram[WRAM_DAY] = kStartDay;
    ram[WRAM_WEEKDAY] = kStartWeekday;
    /* Development aid: PVS_CLOCK_START=<minute> reaches later scenes quickly. */
    const char *start_minute = getenv("PVS_CLOCK_START");
    int minute = start_minute ? atoi(start_minute) : 0;
    ram[WRAM_HOUR] = minute > 0 ? kStartHour : kStartHour - 1;
    ram[WRAM_MINUTE] = (uint8)(minute > 0 ? minute - 1 : 59);
    ram[WRAM_SECOND] = SECONDS_PER_MINUTE - 1;
    ram[WRAM_FRAME] = 0;
    ram[WRAM_CLOCK_FLAGS] |= CLOCK_SYNCED | CLOCK_FIRST_PACKET;
}

static void ClockReceiveMinutePacket(uint8 *ram) {
    if (++ram[WRAM_MINUTE] >= 60) {
        ram[WRAM_MINUTE] = 0;
        ram[WRAM_HOUR]++;
    }
    ram[WRAM_SECOND] = 0;
    ram[WRAM_FRAME] = 0;
    if (ram[WRAM_CLOCK_FLAGS] & CLOCK_FIRST_PACKET)
        ram[WRAM_CLOCK_FLAGS] |= CLOCK_MINUTE_ROLLED;
}

static void ClockTick(uint8 *ram) {
    if (++ram[WRAM_FRAME] >= FRAMES_PER_SECOND && ram[WRAM_SECOND] < SECONDS_PER_MINUTE) {
        ram[WRAM_FRAME] = 0;
        ram[WRAM_SECOND]++;
    }
    if (ram[WRAM_SECOND] >= SECONDS_PER_MINUTE && ram[WRAM_FRAME] >= FRAMES_PAST_MINUTE)
        ClockReceiveMinutePacket(ram);
}

/* Development aid: PVS_TRACE_SLOTS=1 logs each change to the game's timed event
 * slots, with the clock. Guest state is only read here. */
static void TraceEventSlots(const uint8 *ram) {
    static int enabled = -1;
    static uint16 previous[EVENT_SLOT_COUNT * 3];
    if (enabled < 0)
        enabled = getenv("PVS_TRACE_SLOTS") != NULL;
    if (!enabled)
        return;
    for (int i = 0; i < EVENT_SLOT_COUNT; i++) {
        uint16 script = WramRead16(ram, WRAM_SLOT_SCRIPT + i * 2);
        uint16 bank = WramRead16(ram, WRAM_SLOT_BANK + i * 2);
        uint16 when = WramRead16(ram, WRAM_SLOT_TIME + i * 2);
        if (script == previous[i * 3] && bank == previous[i * 3 + 1] && when == previous[i * 3 + 2])
            continue;
        previous[i * 3] = script;
        previous[i * 3 + 1] = bank;
        previous[i * 3 + 2] = when;
        fprintf(stderr, "[slots] clock %02d:%02d scene+%us slot %d script %02X:%04X fires at %us\n",
                ram[WRAM_MINUTE], ram[WRAM_SECOND], WramRead16(ram, WRAM_SCENE_SECONDS), i,
                bank & 0xFF, script, when);
    }
}

/* Development aid for comparing a timeline against a recording or a reference run.
 * PVS_TRACE_SHOTS=<dir> writes a frame about every 30 seconds of clock (or every
 * PVS_TRACE_SHOT_EVERY frames), named by clock. PVS_TRACE_STRIP=<file> appends a tiny
 * picture of every frame drawn, with its clock in frames, for tools/analysis/align_video.py. */
enum { STRIP_WIDTH = 32, STRIP_HEIGHT = 28 };

static void TraceStrip(const char *path, int clock_frames) {
    static FILE *f;
    if (!f)
        f = fopen(path, "wb");
    if (!f)
        return;
    uint8 row[STRIP_WIDTH * STRIP_HEIGHT];
    for (int y = 0; y < STRIP_HEIGHT; y++) {
        const uint8 *line = g_ppu->renderBuffer + (y * 8 + 4) * g_ppu->renderPitch;
        for (int x = 0; x < STRIP_WIDTH; x++) {
            const uint8 *px = line + (x * 8 + 4) * 4;
            row[y * STRIP_WIDTH + x] = (uint8)((px[0] * 29 + px[1] * 150 + px[2] * 77) >> 8);
        }
    }
    uint32 stamp = (uint32)clock_frames;
    fwrite(&stamp, sizeof(stamp), 1, f);
    fwrite(row, 1, sizeof(row), f);
    fflush(f);
}

static void WriteShot(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n256 224\n255\n");
    for (int y = 0; y < 224; y++) {
        const uint8 *row = g_ppu->renderBuffer + y * g_ppu->renderPitch;
        for (int x = 0; x < 256; x++) {
            const uint8 rgb[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4]};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* Development aid: PVS_TRACE_TEXT_SHOTS=<dir> saves the screen a moment after each string
 * finishes drawing, named by part and string, once each. */
static char g_text_shot[64];
static int g_text_shot_wait;

void BsxTraceTextShot(const char *name) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("PVS_TRACE_TEXT_SHOTS") != NULL;
    if (!enabled)
        return;
    snprintf(g_text_shot, sizeof(g_text_shot), "%s", name);
    g_text_shot_wait = 4;
}

static void TraceTextShot(void) {
    if (!g_text_shot[0] || --g_text_shot_wait > 0 || g_snes->disableRender)
        return;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.ppm", getenv("PVS_TRACE_TEXT_SHOTS"), g_text_shot);
    FILE *exists = fopen(path, "rb");
    if (exists)
        fclose(exists);
    else
        WriteShot(path);
    g_text_shot[0] = 0;
}

void BsxTraceShot(void) {
    TraceTextShot();
    static const char *dir, *strip;
    static int checked;
    static int next_frame;
    static int interval = 1800;
    if (!checked) {
        checked = 1;
        dir = getenv("PVS_TRACE_SHOTS");
        strip = getenv("PVS_TRACE_STRIP");
        if (getenv("PVS_TRACE_SHOT_EVERY"))
            interval = atoi(getenv("PVS_TRACE_SHOT_EVERY"));
    }
    if ((!dir && !strip) || g_snes->disableRender)
        return;
    const uint8 *ram = g_cpu.ram;
    int frame = ram[WRAM_MINUTE] * 3606 + ram[WRAM_SECOND] * 60 + ram[WRAM_FRAME];
    if (strip)
        TraceStrip(strip, frame);
    if (!dir || (frame < next_frame && frame + 3600 > next_frame))
        return;
    next_frame = frame + interval;
    char path[256];
    snprintf(path, sizeof(path), "%s/c%02d_%02d_%02d.ppm", dir, ram[WRAM_MINUTE], ram[WRAM_SECOND], ram[WRAM_FRAME]);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n256 224\n255\n");
    for (int y = 0; y < 224; y++) {
        const uint8 *row = g_ppu->renderBuffer + y * g_ppu->renderPitch;
        for (int x = 0; x < 256; x++) {
            const uint8 rgb[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4]};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

/* $C0:D32A, called once per frame after the clock poll. Ticks the seconds counter
 * that timed event slots compare against. */
RecompReturn HleBsxSceneTick(CpuState *cpu) {
    uint8 *ram = cpu->ram;
    if (!PacingIsHolding() && !TitleIsWaiting()) {
        uint16 frame = (uint16)(WramRead16(ram, WRAM_SCENE_FRAMES) + 1);
        if (frame >= FRAMES_PER_SECOND) {
            uint16 seconds = (uint16)(WramRead16(ram, WRAM_SCENE_SECONDS) + 1);
            ram[WRAM_SCENE_SECONDS] = (uint8)seconds;
            ram[WRAM_SCENE_SECONDS + 1] = (uint8)(seconds >> 8);
            frame = 0;
        }
        ram[WRAM_SCENE_FRAMES] = (uint8)frame;
        ram[WRAM_SCENE_FRAMES + 1] = (uint8)(frame >> 8);
    }
    cpu->S += 2;
    return RECOMP_RETURN_NORMAL;
}

/* $C0:EEBF, called once per frame. Replaces reading $2190-$2193. */
RecompReturn HleBsxClockPoll(CpuState *cpu) {
    MouseOnFrame(cpu->ram);
    TitleOnFrame(cpu->ram);
    TraceEventSlots(cpu->ram);
    if (TitleIsWaiting())
        ;  /* no time signal yet: the game waits for it behind the title */
    else if (PacingOnFrame(cpu->ram))
        ;
    else if (cpu->ram[WRAM_CLOCK_FLAGS] & CLOCK_SYNCED)
        ClockTick(cpu->ram);
    else
        ClockReceiveFirstPacket(cpu->ram);
    cpu->S += 2;
    return RECOMP_RETURN_NORMAL;
}

/* Development aid: PVS_TRACE_SOUND=1 logs each sound the game asks for, with the clock. */
static void TraceSound(const uint8 *ram, const char *kind, uint8 sound) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("PVS_TRACE_SOUND") != NULL;
    if (enabled)
        fprintf(stderr, "[sound] clock %d scene %d %s %d\n",
                ram[WRAM_MINUTE] * 3606 + ram[WRAM_SECOND] * 60 + ram[WRAM_FRAME],
                ram[WRAM_SCENE_NUMBER], kind, sound);
}

/* $C0:CAEE and $C0:CAFC queue a piece of music or a sound effect for the sound CPU. The
 * recording has these mixed in already, so they'd play twice. Stack: return, saved S,
 * sound number. The queue keeps the lowest number waiting, as the originals do. */
RecompReturn HleSoundPlayMusic(CpuState *cpu) {
    TraceSound(cpu->ram, "music", cpu->ram[cpu->S + 5]);
    if (!AudioRecordingCoversMusic(cpu->ram))
        cpu->ram[WRAM_MUSIC_QUEUE] = cpu->ram[cpu->S + 5];
    cpu->S += 2;
    return RECOMP_RETURN_NORMAL;
}

RecompReturn HleSoundPlayEffect(CpuState *cpu) {
    uint8 sound = cpu->ram[cpu->S + 5];
    uint8 queued = cpu->ram[WRAM_EFFECT_QUEUE];
    TraceSound(cpu->ram, "effect", sound);
    if (!AudioRecordingCoversEffects(cpu->ram) && (queued == 0 || queued >= sound))
        cpu->ram[WRAM_EFFECT_QUEUE] = sound;
    cpu->S += 2;
    return RECOMP_RETURN_NORMAL;
}

/* $C0:EE80 asks the BIOS (search_channel_map) for the time channel. The channel
 * number is only compared against itself, so any value works. */
RecompReturn HleBsxChannelLookup(CpuState *cpu) {
    cpu->ram[WRAM_CHANNEL] = 0;
    cpu->ram[WRAM_CHANNEL + 1] = 0;
    cpu->S += 2;
    return RECOMP_RETURN_NORMAL;
}

/* $C0:F25A keeps the BIOS's checksum over BS-X SRAM valid. There is no BIOS. */
RecompReturn HleBsxSramChecksum(CpuState *cpu) {
    cpu->S += 2;
    return RECOMP_RETURN_NORMAL;
}

/* $C0:F4A4 resets the SPC and returns to the BIOS. The screen is already dark and
 * what follows only marks time, so the game can run on until the frame ends. */
RecompReturn HleBsxEpisodeExit(CpuState *cpu) {
    SessionRequestNextPart();
    cpu->S += 2;
    return RECOMP_RETURN_NORMAL;
}
