<p align="center"><img src="../dist/qt_themes/default/icons/256x256/anka.png" width="128" alt="Anka"></p>

<p align="center"><b>Anka — a Nintendo Switch 2 emulator for Windows, focused on performance.</b></p>

<p align="center">Not affiliated with Nintendo, the Eden project or Franseven; please report problems in
this repository's <a href="https://github.com/EddizEge/Anka/issues">Issues</a>, not to them.</p>

## The name

Anka is the Zümrüdüanka of Turkish myth, the phoenix that rises from its ashes: an emulator born
from the ashes of yuzu, which closed.

The full documentation is [`README_ANKA.md`](../README_ANKA.md). The `README.md` at the top of
this tree is the upstream project's own.

## Downloads and setup

Download **`Anka-1.0.0-win64.zip`** from the
[Releases page](https://github.com/EddizEge/Anka/releases) (`SHA256SUMS.txt` there lets you
check it).

**Requirement: a working PokeDinner-Eglantine 2.0.0 folder.** Anka is unzipped into that folder,
next to `eglantine.exe`, and uses that folder's Qt DLLs; they are not in the zip. You also need
Windows 10 or 11 (64-bit), a GPU with a current Vulkan driver and the Microsoft Visual C++
2015-2022 x64 runtime.

1. Back up your saves: the `nand` folder in your user folder (see
   [Existing saves and settings](#existing-saves-and-settings)).
2. Unzip `Anka-1.0.0-win64.zip` into the PokeDinner-Eglantine 2.0.0 folder, next to
   `eglantine.exe`. Nothing is overwritten; 2.0.0 keeps working as before.
3. Start **`anka.exe`**. Your games are the same as in 2.0.0's game list.

Games, keys and firmware are not included; only use what you legally own.

To uninstall, delete what the zip added; the list is in "Install" in
[`README_ANKA.md`](../README_ANKA.md).

### Without menus: anka-cli and the launchers

`anka-cli.exe` is the program without menus. Put your game in the `Games` folder (or drag it
onto a launcher) and double-click a launcher:

| Launcher | When to use it |
|---|---|
| `start-recommended.bat` | **Recommended.** The fastest settings we measured (Donkey Kong Bananza). |
| `start-reactive-experimental.bat` | **Experimental.** Reactive Flushing on: on our PC about 7% slower at the game's opening and about 15% faster in open areas. If you see image errors, go back to the recommended one. |
| `start-safe.bat` | **For troubleshooting and for games other than Bananza.** The GPU shortcuts Anka keeps on by default turned off (`EDEN_NX2_LIGHT_DRAW=0` and the rest of its list) and the optional settings that change play cleared. Anka's fixes and its changes without a switch stay in. |

On the first start a launcher makes anka-cli's settings file (`config\sdl2-config.ini`) as a
copy of `qt-config.ini`, so your controller setup comes along; it never changes a settings file
that already exists (`anka-config.ps1`, a small, readable PowerShell script, does this). The
game starts in full screen: **quit with Alt+F4**.

## Settings

In `anka.exe`: **Emulation > Configure... > Anka**. The **Preset** box at the top has four
choices:

- **Recommended** (the first start's choice): all eight settings on, Reactive Flushing off
  (Graphics > Advanced). The setup we measured; the same as `start-recommended.bat`. On Anka's
  first start it also turns Reactive Flushing off in `qt-config.ini` by itself, so 2.0.0's
  `eglantine.exe` then sees it off too. Choosing Recommended later turns it off as well (only
  while no game is running; otherwise set it in Graphics > Advanced afterwards).
- **Recommended + Reactive Flushing**: all eight on, Reactive Flushing on, barrier feedback loops
  on (Graphics > Advanced; Reactive Flushing needs it) and `EDEN_NX2_RX_RT_FIT=3`. Experimental;
  the same as `start-reactive-experimental.bat`.
- **Safe**: all eight off, and the GPU shortcuts Anka keeps on by default off too
  (`EDEN_NX2_LIGHT_DRAW=0` and the rest of `start-safe.bat`'s list), in every game. Graphics
  settings such as Reactive Flushing stay as they are; Anka's fixes and its changes without a
  switch stay in.
- **Custom**: chosen by itself when you change a box by hand.

Recommended was measured in Donkey Kong Bananza only. If another game shows image or speed
problems, try Safe first and report it with the game's name.

Below the presets, six settings apply to **all games** and two to **Donkey Kong Bananza only**
(Dynamic frame rate and Frame pacing; off in other games). The settings are read once per
session and take effect at the first game you start. If you change them after a game was
already started, or start a game that needs other settings (Bananza after another game or the
other way round), Anka asks whether to restart and does it for you. A switch you set outside
the program (your environment, your own `.bat` or `nx2_switches.txt`) always wins; its box is
greyed out. Details: "The Anka tab" in [`README_ANKA.md`](../README_ANKA.md).

## Existing saves and settings

Anka uses the same user folder and settings files as Eglantine / Eden: `user\` next to the
program in a portable install, otherwise `%APPDATA%\eden`, with the same keys, firmware, saves
and `qt-config.ini`. Your saves are not moved or deleted, but both programs use them, so
**back up the `nand` folder** before you try Anka. A setting changed in `anka.exe` or in 2.0.0's
`eglantine.exe` (Reactive Flushing too) is used by the other; only the `[Anka]` group in
`qt-config.ini` is new, and `eglantine.exe` leaves it alone.

## What is faster

Measured in Donkey Kong Bananza only, on one PC only (Ryzen 7 5700X, 64 GB RAM, NVIDIA GeForce
RTX 4060 Ti, 1440p 170 Hz display), with the program without menus (then called eden-cli),
frame generation off unless stated. The menu program runs the same code with the same settings
but was not measured on its own.

**Only Donkey Kong Bananza was measured.** The general improvements run in every game, but other
games, other CPUs and AMD or Intel GPUs were not tried, so there is no claim of measured speed or
tested compatibility for them.

- 2.0.0's `eglantine.exe` measured 17.3 FPS in the heaviest view (far view, by hand); Anka's
  builds measured 42-53 FPS uncapped and 39-46 FPS with the dynamic frame rate patch in the
  opening and open area. This is **not a controlled A/B** (different scenes, modes and runs),
  only a rough indication.
- Single steps, each against the same build: split GPU thread stages +7.3 to +8.7%, per-draw
  shortcuts +6.8 to +7.5%, the macro JIT filter +6% (39.6 -> 42.1 FPS), profile-guided
  optimization +15.8 to +21.5%, the terrain's indirect draws on the host GPU about +48% in the
  far view (~20.8 -> ~30.8 FPS).
- With the dynamic frame rate patch and FSR3 frame generation aiming at 120, one 3.6 minute
  session of normal play had a median of 46 game FPS, 119 frames a second on screen and 99.8%
  game speed. One session, not a benchmark.

The method and every measurement: "Measured results" in [`README_ANKA.md`](../README_ANKA.md);
every change since PokeDinner-Eglantine 2.0.0: [`CHANGELOG_ANKA.md`](../CHANGELOG_ANKA.md).

## Roadmap

From now on every update goes into one emulator, Anka. The next release: Zelda support and
per-game presets. No dates.

## Building

See [`BUILDING_ANKA.md`](../BUILDING_ANKA.md).

## License

Anka is licensed under the **GNU General Public License v3.0 or later**
([`LICENSE.txt`](../LICENSE.txt)), like Eden and Eglantine. Each release has the complete
corresponding source next to the program: `Anka-1.0.0-source.zip` (this repository at the
release commit, tag `anka-v1.0.0`), `Anka-1.0.0-deps-source.zip` (the third-party libraries built
from source) and `Anka-1.0.0-prebuilt-source.zip` (the sources and build scripts of the four
prebuilt libraries: FFmpeg, OpenSSL, SDL3, sirit). `anka.exe` links Qt 6.8.3 dynamically
(LGPL-3.0; Qt Charts GPL-3.0); its Qt DLLs are those of your 2.0.0 folder. Third-party notices
are in `THIRD_PARTY_NOTICES_ANKA.txt` in the release zips and in [`LICENSES`](../LICENSES).

## Credits

**Built on.** Anka builds on Eden, Eglantine (the PokeDinner fork by Franseven), yuzu and
Sudachi; their copyrights and credits are kept. It is not an official release of those
projects. Please report Anka's problems here, not to them.

- Franseven's [Eglantine](https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork)
  (PokeDinner-a-Eglantine-Fork).
- The [Eden](https://git.eden-emu.dev/eden-emu/eden), yuzu and Sudachi teams and all their
  contributors.
- Forks whose ideas were ported, credited in the code: Selevet, elBart0, Duck912 / moceure's
  eden-nx2-android, and ZendNevs (the `dk_dynamic_fps` patch that the dynamic frame rate setting
  ports). The upstream commits picked into Anka are listed with their authors in the changelog.

Most of Anka's code, its commit messages and its documents were written by an AI coding
assistant (Claude Opus 5.5, by Anthropic) under the direction of the human maintainer, who
decided what to try, played and tested the builds, and is responsible for the release. Every
commit made with the assistant says so with an `Assisted-by:` line, as the Eden project's AI
policy asks.

## Türkçe

Türkçe belge: [`BENIOKU_ANKA.md`](../BENIOKU_ANKA.md).

## Issues

Report problems at https://github.com/EddizEge/Anka/issues (please not to Franseven, the Eden
project or the other upstream projects). Say which program (`anka.exe` or `anka-cli.exe`) and
which preset or launcher you used, the game's version, and attach the log file (`log\eden_log.txt` in
the user folder).

Nintendo Switch and Donkey Kong Bananza are trademarks of Nintendo. Use Anka only with games
you own.
