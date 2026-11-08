# BS Tantei Club: Yuki ni Kieta Kako, native port

A native port of Nintendo's 1997 Satellaview game *BS Detective Club: The Past That
Disappeared in the Snow*: all three parts in one program, in English, Simplified Chinese or Japanese, with
the broadcast's radio audio restored. It needs neither the BS-X BIOS nor the BS-X town.

**Work in progress.** You can build it on Linux or Windows and play it from start to finish,
but few people have tried it, and rough edges are likely.

No game data is included. You supply your own copies of the game, and the recordings if
you want the audio.

## What works

- All three parts run one after another in one program, with no restart.
- English, Simplified Chinese and Japanese text, with the original's timing kept so lines still
  match the voices.
- Save states that work across parts, rewind, and resuming where you left off.
- Two ways to play: **Real Time**, as it aired, or **Unlimited Time**, which stops the clock
  before each timed event until SELECT moves on.
- The broadcast's voices and music from a recording, in sync with the game's clock, or just
  the game's own sounds.
- Parts run on as acts of one story: the cast list that closed each broadcast only runs at
  the very end, which credits everyone, and the announcement from the reruns is gone.
- Stretched or integer scaling, and a scanline shader.
- Mouse or gamepad. The hand follows the mouse; left click is A, right click is B, and middle
  click is SELECT.

## What's missing

- The English is a machine translation with no human review, so expect some mistakes.
  It's plain text in [translation/en](translation/en), and fixes are welcome.
- The Simplified Chinese is too, in [translation/zh-Hans](translation/zh-Hans). Words drawn
  into the game's pictures, such as the command icons, stay in Japanese for now.

## Known issues

- **Sound effects in the music.** No clean copy of the radio audio exists. The only
  recordings were made by people playing along, so their typing, beeps and clicks are mixed
  into the music. Where a track repeats, playback pieces together a clean pass from every
  take in every recording, so have all three recordings if you can. Where a track plays only
  once, any part of it that repeats within itself stands in, and the rest is filtered as far as
  possible without harming the music. Either way you may still hear some bleed through, mostly
  faint beeps and typing.
  [docs/AUDIO_STATUS.md](docs/AUDIO_STATUS.md) lists where each track stands.
- **Voices stay as broadcast.** Dialogue is never cut, and on tracks that repeat it plays as
  recorded, so any effects under a line of dialogue remain. On tracks that play only once, the
  filter that takes off the player's sounds runs under dialogue too.
- **Soundtrack versions.** A few investigation tunes can be swapped for versions from the
  Famicom Detective Club albums (see below). They're different arrangements, so the music
  changes character when one comes in after the scene's opening dialogue.

## What you need

The three episode packs, matching these [No-Intro](https://no-intro.org) entries:

| Part | No-Intro entry | CRC32 |
|---|---|---|
| Zenpen | `BS Tantei Club - Yuki ni Kieta Kako - Zenpen (Japan) (1998-12-29) (SoundLink)` | `DC3354E8` |
| Chuuhen | `BS Tantei Club - Yuki ni Kieta Kako - Chuuhen (Japan) (1998-6-20) (SoundLink)` | `67568B74` |
| Kouhen | `BS Tantei Club - Yuki ni Kieta Kako - Kouhen (Japan) (Premiere) (1997-2-24) (SoundLink)` | `834055A7` |

The radio audio was never dumped, so the only source is three Stage6 captures of the
broadcast. They're optional. They're known by size, because the names of two are swapped:

| File | Size in bytes | Really holds |
|---|---|---|
| `1011014_-_BS_Tantei_Club_-_Yuki_ni_Kieta_Kako_-_Zenpen.divx` | 544397818 | Zenpen |
| `1011305_-_BS_Tantei_Club_-_Yuki_ni_Kieta_Kako_-_Kouhen.divx` | 973857624 | Chuuhen |
| `1017211_-_BS_Tantei_Club_-_Yuki_ni_Kieta_Kako_-_Chuuhen.divx` | 664082876 | Kouhen |

Put them in `inputs/`. At launch the audio is copied out of them
as it is, with no re-encoding, and they can be deleted after that. A background task then
renders a cleaned copy of each looping track into `audio/loops/`, once. That takes about ten
minutes and up to about a gigabyte of memory, and you can play meanwhile: each track uses its
cleaned copy as soon as it's ready.

Optionally, a few tunes behind the investigations are ones the composer, Kenji Yamamoto,
also wrote for the Famicom Detective Club games, and their albums have clean versions. Put
the Super Famicom album (disc 2 of the complete music collection) or the Switch remakes'
soundtracks anywhere under `soundtrack/`, and choose a version under Sound, Investigation
music. Files are found by name and length; [data/soundtrack/tracks.toml](data/soundtrack/tracks.toml)
lists them.

## Playing

Download the pack for your system from the [rolling release](https://github.com/oneup03/PastDisappearedInSnow/releases/tag/rolling).
It doesn't contain the game: it builds the game on your computer, from your own files.

1. Unzip it somewhere with about 2 GB free.
2. Make an `inputs` folder inside it, and put in the three packs, extracted to `.bs` files,
   and the recordings if you have them.
3. Run `BsTanteiClubSNESRecomp`. Let it download the build tools the first time, then choose
   any one of the `.bs` files and start the build. It takes a few minutes, then the game starts.

From then on, run the copy in `build/`. A Linux pack needs glibc 2.35 or newer.

## Building on Linux

You need Python 3, Rust, CMake, Ninja and a C compiler.

```sh
# in a clone of this repository
mkdir inputs                # then put the three packs here
python3 tools/setup.py
build/BsTanteiClubSNESRecomp --launcher --rom images/zen.sfc
```

Setup checks the packs, builds their images and generates the code from them. All of that
stays on your machine, and none of it may be shared. The launcher's Mods page has the game's
options: pacing, scaling, where to start, language and sound.

## Building on Windows

You need [Git for Windows](https://git-scm.com/downloads/win),
[Python](https://www.python.org/downloads/windows/) 3.11 or newer,
[Rust](https://rust-lang.org/tools/install/) with its default MSVC toolchain, and
[Visual Studio 2022](https://visualstudio.microsoft.com/vs/older-downloads/) with
**Desktop development with C++**, which brings CMake and Ninja. In the **Developer
PowerShell for VS 2022**:

```powershell
# in a clone of this repository
mkdir inputs                # then put the three packs here
python tools/setup.py
build\BsTanteiClubSNESRecomp.exe --launcher --rom images\zen.sfc
```

Setup doesn't need Git Bash. The Visual Studio build hasn't been tried yet; the MinGW
cross-build below has, and plays the same as on Linux.

### Cross-building from Linux

With Fedora's MinGW packages (`mingw64-gcc mingw64-gcc-c++ mingw64-winpthreads-static
mingw64-SDL3`), after `tools/setup.py --no-build` has generated the code:

```sh
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=/usr/share/mingw/toolchain-mingw64.cmake
cmake --build build-win
```

`build-win/` then holds `BsTanteiClubSNESRecomp.exe` and `SDL3.dll`, with the files it
needs beside them. It runs under Wine too. Wine drops `SDL_VIDEODRIVER` from the
environment, so for a headless run, set it inside `wine cmd /c`.

## Translating

A language is a folder under [translation](translation), with no code changes needed. One
whose script our Latin font lacks names a 12-pixel BDF font in its `meta.toml`, as
[zh-Hans](translation/zh-Hans/meta.toml) does; text with no spaces breaks between characters.
`python3 tools/script.py dump` writes the Japanese to `text-dump/ja/` from your own packs, and
`worksheet` and `merge` turn that into a side-by-side sheet for editing and back. `status`
and `lint` report what's left and what doesn't fit.

## Credits and license

Our own code is MIT. It builds on [snesrecomp](https://github.com/RetroPortingToolKit/snesrecomp)
and [recomp-ui](https://github.com/mstan/recomp-ui), so see [LICENSE](LICENSE) for which files
that covers: snesrecomp is PolyForm Noncommercial, which means builds are for noncommercial
use only. MP3 decoding is [minimp3](https://github.com/lieff/minimp3) (CC0). Japanese text
uses the public-domain Shinonome font, and Chinese text the
[Fusion Pixel](https://github.com/TakWolf/fusion-pixel-font) font (SIL Open Font License),
both kept in [assets/fonts](assets/fonts). The Chinese font is cut down to the characters
the translation uses: `tools/subset_font.py` cuts it again from the full font when that changes.

This is an unofficial fan project, not affiliated with or endorsed by Nintendo. The game, its
ROMs and its audio belong to their rights holders and are not part of this project.
