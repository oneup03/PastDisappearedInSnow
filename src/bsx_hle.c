/* Satellaview hardware and BIOS services the game expects, reimplemented so it
 * runs with no BS-X BIOS and no base unit.
 *
 * The original gets the time from packets on a satellite data channel. The port
 * behaves like a perfect signal instead. All clock state stays in guest WRAM, so
 * save states and rewind capture it with everything else.
 */
#include <stdio.h>
#include <stdlib.h>

#include "cpu_state.h"

/* WRAM bank $7E, as laid out by $C0:EE53 and $C0:EF5E. */
enum {
    WRAM_CHANNEL = 0x417B,
    WRAM_YEAR = 0x417D,
    WRAM_MONTH = 0x417F,
    WRAM_DAY = 0x4180,
    WRAM_WEEKDAY = 0x4181,
    WRAM_HOUR = 0x4182,
    WRAM_MINUTE = 0x4183,
    WRAM_SECOND = 0x4184,
    WRAM_FRAME = 0x4185,
    WRAM_CLOCK_FLAGS = 0x4186,
};

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

static void ClockReceiveFirstPacket(uint8 *ram) {
    ram[WRAM_YEAR] = (uint8)kStartYear;
    ram[WRAM_YEAR + 1] = (uint8)(kStartYear >> 8);
    ram[WRAM_MONTH] = kStartMonth;
    ram[WRAM_DAY] = kStartDay;
    ram[WRAM_WEEKDAY] = kStartWeekday;
    ram[WRAM_HOUR] = kStartHour;
    ram[WRAM_MINUTE] = 0;
    ram[WRAM_SECOND] = 0;
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

/* $C0:EEBF, called once per frame. Replaces reading $2190-$2193. */
RecompReturn HleBsxClockPoll(CpuState *cpu) {
    if (cpu->ram[WRAM_CLOCK_FLAGS] & CLOCK_SYNCED)
        ClockTick(cpu->ram);
    else
        ClockReceiveFirstPacket(cpu->ram);
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

/* $C0:F4A4 resets the SPC and returns to the BIOS. Moving to the next episode
 * replaces this once the session manager exists. */
RecompReturn HleBsxEpisodeExit(CpuState *cpu) {
    (void)cpu;
    fprintf(stderr, "[bsx] episode finished\n");
    fflush(stderr);
    exit(0);
}
