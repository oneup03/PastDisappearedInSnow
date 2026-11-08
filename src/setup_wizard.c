/* The launcher's first-run wizard, for a game made from three packs.
 *
 * Released builds hold no recompiled code: on the player's machine the wizard takes their own
 * files, generates the code and builds the game. The framework's wizard assumes one ROM, with
 * its code in src/gen, so this points it at tools/setup_wizard.py and our layout. */
#include "setup_wizard.h"

#include <stdio.h>

#include "host_paths.h"
#include "snesrecomp_codegen_host.h"

static int (*g_generate)(const char *, char *, size_t, char *, size_t, RecompLauncherCPrepareProgressFn, void *);

/* The file the wizard was given is a pack, which the game can't run: what it plays next is
 * the image built from it. */
static int GenerateAndUseImage(const char *source, char *out, size_t out_size, char *error, size_t error_size,
                               RecompLauncherCPrepareProgressFn progress, void *context) {
    if (!g_generate(source, out, out_size, error, error_size, progress, context))
        return 0;
    static const char *const kLeaves[] = {"images/zen.sfc", "../images/zen.sfc"};
    for (size_t i = 0; i < sizeof(kLeaves) / sizeof(kLeaves[0]); i++) {
        char path[1024];
        if (snesrecomp_exe_dir_path(kLeaves[i], path, sizeof(path))) {
            FILE *f = fopen(path, "rb");
            if (f) {
                fclose(f);
                snprintf(out, out_size, "%s", path);
                break;
            }
        }
    }
    return 1;
}

void SetupWizardConfigure(RecompLauncherCGameInfo *info) {
    if (info->in_session)
        return;
    static const SnesrecompCodegenHostConfig kConfig = {
        .display_name = "BS Tantei Club",
        .snesrecomp_cli_relpath = "tools/setup_wizard.py",
        .cfg_dir = "recomp/zen",
        .out_dir = "src/gen/zen",
        .funcs_h = "recomp/zen/funcs.h",
        .gen_marker_relpath = "src/gen/zen/dispatch_v2.c",
        .cmake_target = "BsTanteiClubSNESRecomp",
        .exe_basename = "BsTanteiClubSNESRecomp",
        .prepare_note = "Checks your three episode packs, recompiles them and builds the game on this "
                        "computer. Choose any one of the .bs files, with the other two beside it.",
        .prepare_note_windows = "Checks your three episode packs and recompiles them, then closes and builds "
                                "the game in a console window. Choose any one of the .bs files, with the "
                                "other two beside it.",
    };
    /* The framework wired itself up for the one-ROM layout first; what it decided doesn't apply. */
    info->needs_setup = 0;
    info->prepare_required_before_continue = 0;
    snesrecomp_codegen_host_apply(info, &kConfig);
    if (info->prepare_with_progress) {
        g_generate = info->prepare_with_progress;
        info->prepare_with_progress = GenerateAndUseImage;
    }
#ifdef SNESRECOMP_SETUP_HOST
    /* The digests describe the finished image, and the player has packs. The script checks those. */
    info->rom_revisions = NULL;
    info->num_rom_revisions = 0;
    info->has_expected_crc = 0;
    info->known_sha256 = NULL;
    info->num_known_sha256 = 0;
#endif
}
