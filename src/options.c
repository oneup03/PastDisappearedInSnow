/* Player options. The launcher's Mods page is where the framework puts anything that
 * changes a game's picture or play, so these are features of a preloaded package
 * (mods/preloaded/packages/pvs.options) that this code implements.
 */
#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "display_aspect.h"
#include "mod_runtime.h"
#include "mouse.h"
#include "options.h"
#include "pacing.h"
#include "session.h"
#include "snes_rewind.h"
#include "text.h"

/* Four minutes of rewind instead of the framework's six seconds: one snapshot a
 * second, about 96 MB. The launcher's rewind rows still have the last word. */
enum {
    REWIND_DEPTH = 240,
    REWIND_INTERVAL = 60,
    FRAMEWORK_REWIND_DEPTH = 60,
    FRAMEWORK_REWIND_INTERVAL = 6,
    LAUNCHER_REWIND_DEPTH = 50,
    LAUNCHER_REWIND_INTERVAL = 15,
};

static bool g_integer_scale;

/* The launcher always shows assets/img/boxart.tga, so put the chosen language's art there for
 * next time. Options are only readable once the game is running. */
void OptionsChooseBoxArt(void) {
    char leaf[64], from[1024], to[1024];
    snprintf(leaf, sizeof(leaf), "assets/img/boxart_%s.tga", strcmp(OptionsLanguage(), "ja") == 0 ? "ja" : "en");
    AssetPath(leaf, from, sizeof(from));
    AssetPath("assets/img/boxart.tga", to, sizeof(to));
    FILE *in = fopen(from, "rb");
    if (!in)
        return;
    static uint8 buffer[4 << 20];
    size_t size = fread(buffer, 1, sizeof(buffer), in);
    fclose(in);
    /* Only when it differs, so nothing is written on an ordinary start. */
    static uint8 current[4 << 20];
    FILE *old = fopen(to, "rb");
    size_t old_size = old ? fread(current, 1, sizeof(current), old) : 0;
    if (old)
        fclose(old);
    if (old_size == size && memcmp(buffer, current, size) == 0)
        return;
    FILE *out = fopen(to, "wb");
    if (out) {
        fwrite(buffer, 1, size, out);
        fclose(out);
    }
}

void OptionsAfterConfig(void) {
    /* These values mean nobody has chosen otherwise. */
    if (g_config.rewind_depth == FRAMEWORK_REWIND_DEPTH && g_config.rewind_interval == FRAMEWORK_REWIND_INTERVAL) {
        g_config.rewind_depth = REWIND_DEPTH;
        g_config.rewind_interval = REWIND_INTERVAL;
    }
}

void OptionsBeforeFrame(void) {
    if (g_config.rewind_depth != LAUNCHER_REWIND_DEPTH || g_config.rewind_interval != LAUNCHER_REWIND_INTERVAL)
        return;
    g_config.rewind_depth = REWIND_DEPTH;
    g_config.rewind_interval = REWIND_INTERVAL;
    snes_rewind_set_defaults(g_config.rewind_enabled, REWIND_DEPTH, REWIND_INTERVAL);
    snes_rewind_shutdown();
    snes_rewind_configure();
}

const char *OptionsLanguage(void) {
    /* Development aid: PVS_LANGUAGE overrides the launcher. */
    static char language[32];
    const char *chosen = getenv("PVS_LANGUAGE");
    if (chosen)
        return chosen;
    if (snes_mod_runtime_feature_option_value_c("pvs.options", "language", "lang", language, sizeof(language)) > 0)
        return language;
    return "ja";
}

const char *OptionsAudioSource(void) {
    /* Development aid: PVS_AUDIO overrides the launcher. */
    static char source[32];
    const char *chosen = getenv("PVS_AUDIO");
    if (chosen)
        return chosen;
    if (snes_mod_runtime_feature_option_value_c("pvs.options", "audio", "source", source, sizeof(source)) > 0)
        return source;
    return "recording";
}

static void ApplyStartOption(void) {
    char from[16];
    if (snes_mod_runtime_feature_option_value_c("pvs.options", "start", "from", from, sizeof(from)) > 0)
        SessionSetStart(from);
}

static void ApplyPacingOption(void) {
    char mode[16];
    if (snes_mod_runtime_feature_option_value_c("pvs.options", "pacing", "mode", mode, sizeof(mode)) > 0)
        PacingSetMode(strcmp(mode, "unlimited") == 0 ? PACING_UNLIMITED : PACING_REAL_TIME);
}

static void EnableIntegerScale(void) { g_integer_scale = true; }

/* The language and audio source are read straight from their options when a part's image loads. */
static void NoAction(void) {}

static void ResetOptions(void) {
    PacingSetMode(PACING_REAL_TIME);
    g_integer_scale = false;
}

SNES_MOD_CONSTRUCTOR(RegisterOptions) {
    snes_mod_register_reset_callback(ResetOptions);
    snes_mod_register_activation_plugin("pvs.pacing", ApplyPacingOption);
    snes_mod_register_presentation_plugin("pvs.integer-scale", EnableIntegerScale);
    snes_mod_register_activation_plugin("pvs.start", ApplyStartOption);
    snes_mod_register_presentation_plugin("pvs.language", NoAction);
    snes_mod_register_presentation_plugin("pvs.audio", NoAction);
}

void OptionsComputeViewport(int frame_w, int frame_h, int drawable_w, int drawable_h,
                            SnesDisplayViewport *viewport) {
    if (g_integer_scale && !g_config.ignore_aspect_ratio) {
        SnesDisplayAspect_ComputeViewport(frame_w, frame_h, drawable_w, drawable_h,
                                          (SnesDisplayAspect)g_config.display_aspect, false, true, viewport);
    }
    MouseSetViewport(frame_w, frame_h, viewport);
}
