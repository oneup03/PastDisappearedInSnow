#pragma once
#include "display_aspect.h"

/* SnesDesktopHostGame.region: the launcher's Region row, which also says whether every
 * part and recording is there. Filled in by OptionsAfterConfig, before the launcher opens. */
extern char g_options_region[96];

/* SnesDesktopHostGame.after_config. */
void OptionsAfterConfig(void);

/* Each frame. The launcher only offers certain rewind settings and turns ours into its own default. */
void OptionsBeforeFrame(void);

/* Put the chosen language's box art where the launcher will show it next time. */
void OptionsChooseBoxArt(void);

/* The chosen language's folder under translation/, or "ja". */
const char *OptionsLanguage(void);

/* "recording" or "rom". */
const char *OptionsAudioSource(void);

/* SnesDesktopHostGame.compute_viewport: integer scaling, when chosen. */
void OptionsComputeViewport(int frame_w, int frame_h, int drawable_w, int drawable_h,
                            SnesDisplayViewport *viewport);
