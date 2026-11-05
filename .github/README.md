# NX2Perf

**NX2Perf** is an unofficial performance fork of **PokeDinner-Eglantine 2.0.0** (Franseven's
fork of the Eden Nintendo Switch emulator), made to play **Donkey Kong Bananza** more smoothly
on a desktop PC. It ships one program, `eden-cli-nx2perf.exe` (the command line / SDL front end),
which you put next to an existing PokeDinner-Eglantine 2.0.0 installation, and three launchers
that start it with the settings we measured.

**Tested on one machine with one game.** Every result comes from a Ryzen 7 5700X (8 cores),
64 GB RAM, an NVIDIA GeForce RTX 4060 Ti and a 1440p 170 Hz display, playing Donkey Kong
Bananza. Other games, other CPUs and AMD or Intel GPUs were not tried.

NX2Perf is not affiliated with the Eden project, with Franseven or with Nintendo. Please do not
report its problems to them.

The full documentation is [`README_NX2PERF.md`](../README_NX2PERF.md) (Turkish:
[`BENIOKU_NX2PERF.md`](../BENIOKU_NX2PERF.md)). The `README.md` at the top of this tree is the
upstream project's own.

## Requirements

- Windows 10 or 11, 64-bit, a GPU with a current Vulkan driver, and the Microsoft Visual C++
  2015-2022 x64 runtime (all as for 2.0.0).
- A **working PokeDinner-Eglantine 2.0.0 installation** with your own keys and firmware, and
  your own game dump. **NX2Perf provides no games, keys or firmware**, and never will.

## Quick install

Before the first start, **back up your saves**: the `nand` folder in your user folder (`user\`
next to `eglantine.exe` in a portable setup, otherwise `%APPDATA%\eden`). NX2Perf uses the same
saves as 2.0.0.

1. Unzip the `-win64.zip` from the release into your PokeDinner-Eglantine 2.0.0 folder, next to
   `eglantine.exe`. Nothing is overwritten; 2.0.0 keeps working as before.
2. Put your game file (`.nsp`, `.xci`, `.nsz` or `.xcz`) in the new `Games` folder, or drag it
   onto the launcher instead.
3. Double-click `start-recommended.bat`.

On the first start the launcher makes eden-cli's settings file (`config\sdl2-config.ini`) as a
copy of the menu program's `qt-config.ini`, so your controller setup comes along; it never
changes a settings file that already exists. This is done by `nx2perf-config.ps1` next to the
launchers, a small, readable PowerShell script. The game starts in full screen: **quit with
Alt+F4**.

## Which launcher?

| Launcher | When to use it |
|---|---|
| `start-recommended.bat` | **Recommended.** The fastest settings we measured (Donkey Kong Bananza). |
| `start-reactive-experimental.bat` | **Experimental.** Reactive flushing on: on our machine about 7% slower at the game's opening and about 15% faster in open areas. If you see image errors, go back to the recommended one. |
| `start-safe.bat` | Every switch this fork added turned off, the closest to 2.0.0: for troubleshooting and for games other than Bananza. |

## What changed

Most of the work is on the emulated GPU's command thread, which limits Bananza on a desktop CPU:
the GPU thread split into pipelined stages, cheaper per-draw work, Bananza's terrain drawn with
indirect draws on the host GPU, frame pacing that follows the game's own 60/30/20 fps choice, an
in-memory dynamic frame rate patch for Bananza (40-60 fps instead of dropping to 30), timed FSR3
generated frames, an RTSS on-screen display, accuracy fixes picked from upstream and other forks,
and a build with link-time and profile-guided optimization. Almost everything new is behind an
`EDEN_NX2_*` switch, so `start-safe.bat` can turn it off. The full list is in
[`CHANGELOG_NX2PERF.md`](../CHANGELOG_NX2PERF.md).

## Measured results (short)

On the machine above, Donkey Kong Bananza, frame generation off unless stated:

- 2.0.0's `eglantine.exe` measured 17.3 FPS in the heaviest view by hand; this fork's builds
  measured 39-53 FPS in the opening and open area. This is **not a controlled A/B** (different
  scenes, modes and runs), only a rough indication; the controlled steps are in the README.
- Single steps, each against the same build: back stages +7 to +9%, per-draw shortcuts +7 to
  +8%, the macro JIT filter +6%, profile-guided optimization +16 to +22%, the terrain's indirect
  draws on the host GPU about +48% in the far view.
- With DK-DYN and FSR3 frame generation aiming at 120, one 3.6 minute session of normal play had
  a median of 46 game FPS, 119 frames a second on screen and 99.8% game speed.

Details and the method: "Measured results" in [`README_NX2PERF.md`](../README_NX2PERF.md).

## Known limits

- Measured with **Donkey Kong Bananza only**; the Bananza switches are meant for that game.
- **No Qt (menu) version.** The release is the command line program `eden-cli`; keep 2.0.0's
  `eglantine.exe` for its menus.
- AMD and Intel GPUs and other CPUs were not tried. More in "Known issues" in the README.

## License and source

NX2Perf is licensed under the **GNU General Public License v3.0 or later**
([`LICENSE.txt`](../LICENSE.txt)), like Eden and PokeDinner-Eglantine. Each release has the
complete corresponding source next to the program:

- `-source.zip`: this repository at the release commit;
- `-deps-source.zip`: the third-party libraries built from source;
- `-prebuilt-source.zip`: the sources and build scripts of the four prebuilt libraries
  (FFmpeg, OpenSSL, SDL3, sirit).

How to build it: [`BUILDING_NX2PERF.md`](../BUILDING_NX2PERF.md). Third-party notices are in
`THIRD_PARTY_NOTICES_NX2PERF.txt` in the release zips.

## Credits

- The [Eden](https://git.eden-emu.dev/eden-emu/eden) emulator project and everyone behind it.
- Franseven's [PokeDinner-a-Eglantine-Fork](https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork),
  the base of this fork.
- Forks whose ideas were ported, credited in the code: Selevet, elBart0, Duck912 / moceure's
  eden-nx2-android, and ZendNevs (the `dk_dynamic_fps` patch that `DK_DYN` ports). The upstream
  commits picked into this fork are listed with their authors in the changelog.

## AI disclosure

Most of this fork's code, its commit messages and its documents were written by an AI coding
assistant (Claude Opus 5.5, by Anthropic) working under the direction of the human maintainer,
who decided what to try, played and tested the builds, and is responsible for the release.
Every commit made with the assistant says so with an `Assisted-by:` line, as the Eden project's
AI policy asks. This fork is published on its own and is not submitted upstream.

## Issues

Report problems here: https://github.com/EddizEge/NX2Perf/issues (please not to the Eden project
or to Franseven). Türkçe belge: [`BENIOKU_NX2PERF.md`](../BENIOKU_NX2PERF.md).

Nintendo Switch and Donkey Kong Bananza are trademarks of Nintendo. Use NX2Perf only with games
you own.
