/*
 * BS Tantei Club — desktop host shim.
 *
 * Identity and hooks, nothing else. The host itself is the framework's
 * (snesrecomp/runner/src/desktop/host_main.h): the pre-boot launcher, ROM
 * resolution and digest checks, config.ini and keybinds.ini, the window and
 * the SDL / OpenGL presenters, audio, gamepads, the in-game save-state
 * browser and rewind filmstrip, the OSD, the pacing clock, crash handlers and
 * the post-mortem report, mod packages, Generate & rebuild, and the netplay
 * barrier when the project is built with it. A fix there reaches this
 * project on a submodule pull; nothing in this file needs to change for it.
 *
 * This file used to BE the host -- a ~700-line copy made at scaffold time --
 * so every project scaffolded before a fix kept the bug. Two ports of the
 * same game, a week apart, had different hosts.
 *
 * Nothing here ever ships a ROM. The player supplies one; the host makes
 * that easy (launcher, then positional argument, then a copy beside the
 * executable, then the rom.cfg cache, then a file picker) and checks what it
 * is handed against the digests in rom_identity.txt.
 *
 * Grow this file only with what is specific to THIS title: a custom
 * presenter (prepare_frame / draw_frame), an SPC player, a pacing rule
 * (keep_pacing_debt), a Mods provider. See SnesDesktopHostGame for the
 * complete list of hooks and what each is for.
 */

#include "host_main.h"
#include "game_rtl.h"
#include "options.h"
#include "session.h"
#include "title.h"
#include "snesrecomp_rom_identity.h"  /* generated from rom_identity.txt */

#ifndef __ANDROID__
#define SDL_MAIN_HANDLED 1
#endif
#include "desktop/sdl_compat.h"
#ifdef __ANDROID__
/* SDLActivity loads libmain.so and dlsyms its SDL_main. */
#define main SDL_main
#endif

#include <string.h>

#include "setup_wizard.h"

#ifndef SNES_GAME_VERSION
#define SNES_GAME_VERSION "dev"
#endif

/* The image the launcher checks is built from all three verified packs. Naming it here
 * keeps the host from making up a label from the Region row, which says more. */
static const SnesRomRevision kRevisions[] = {
    {"BS Tantei Club: Yuki ni Kieta Kako, built from your packs", SNESRECOMP_ROM_EXPECTED_SHA256, NULL,
     SNESRECOMP_ROM_EXPECTED_CRC32, NULL},
};

static const SnesDesktopHostGame kGameHost = {
    .display_name        = "BS Tantei Club",
    .window_title        = "BS Tantei Club",
    .region              = g_options_region,
    .rom_revisions       = kRevisions,
    .num_rom_revisions   = 1,
    .rom_file            = SNESRECOMP_ROM_FILE,
    .expected_sha256_hex = SNESRECOMP_ROM_EXPECTED_SHA256,
    .expected_crc32_hex  = SNESRECOMP_ROM_EXPECTED_CRC32,
    .game_id             = SNESRECOMP_ROM_GAME_ID,
    .build_version       = SNES_GAME_VERSION,
    .game_info           = &kGameInfo,
    .num_players         = 1,
    /* Battery-backed SRAM shows the launcher's SAVES panel. Leave NULL for a
     * title without one. The path is exe-relative. */
    .sram_path           = "saves/save.srm",
    .before_run_frame    = &SessionBeforeFrame,
    .compute_viewport    = &OptionsComputeViewport,
    .display_aspect_supported = 1,
    .shader_supported    = 1,
    .rewind_settings     = 1,
    .in_game_launcher    = 1,
    .after_config        = &OptionsAfterConfig,
    .on_shutdown         = &SessionOnShutdown,
    .publish_native_field = &TitlePublishField,
    .configure_launcher  = &SetupWizardConfigure,
};

#ifndef __ANDROID__
#undef main   /* desktop: keep plain main() even if SDL_main.h remapped it */
#endif
int main(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0)
            SessionSetLaunchedRom(argv[i + 1]);
    }
    return snesrecomp_desktop_main(&kGameHost, argc, argv);
}
