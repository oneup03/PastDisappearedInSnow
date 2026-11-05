#pragma once
#include <stdbool.h>
#include <stddef.h>

/* One program, three parts. See src/session.c. */
enum { PART_ZENPEN, PART_CHUUHEN, PART_KOUHEN, PART_COUNT };

/* Where the other parts' images live: beside the one the host launched. */
void SessionSetLaunchedRom(const char *path);

/* Where to begin at launch: "continue" or a part's key. */
void SessionSetStart(const char *from);

/* RtlGameInfo.initialize: the machine has just loaded the current part's image. */
void SessionOnMachineBuilt(void);

/* The current part has ended. Takes effect at the next frame boundary. */
void SessionRequestNextPart(void);

/* Desktop host hook, at each frame boundary. */
void SessionBeforeFrame(void);

/* Desktop host hook, as the program ends: keep the player's place. */
void SessionOnShutdown(void);

/* Snapshot hook. A snapshot from another part switches to that part first. */
bool SessionSnapshotPreflight(const void *data, size_t size);
