# NX2Perf

NX2Perf is an unofficial performance fork of **PokeDinner-Eglantine 2.0.0** (Franseven's fork of
the Eden emulator), made to play **Donkey Kong Bananza** more smoothly on a desktop PC. It ships
one program, `eden-cli-nx2perf.exe` (the command line / SDL front end), that you put next to an
existing PokeDinner-Eglantine 2.0.0 installation, and launchers that start it with the settings
we measured.

It is not affiliated with the Eden project or with Franseven. Please do not report its problems
to them; use this fork's issue tracker (`<KAYNAK_URL>`).

- Base: PokeDinner-a-Eglantine-Fork 2.0.0, commit `70ecb39ef0`
  (https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork)
- This release: see `VERSION_NX2PERF.txt` in the package (tag, commit, build date).
- Source: `<KAYNAK_URL>` (the same commit as the program; GPLv3, see "License").
- Changes: `CHANGELOG_NX2PERF.md`. Every switch: `nx2_switches_NX2PERF.txt` (in the source:
  `dist/nx2_switches.txt`).

**Tested on one machine with one game.** Every number below comes from a Ryzen 7 5700X (8
cores), 64 GB RAM, an NVIDIA GeForce RTX 4060 Ti and a 1440p 170 Hz display, playing Donkey
Kong Bananza. Other games, other CPUs and AMD or Intel GPUs were not tried.

## What you need

- Windows 10 or 11, 64-bit.
- A working **PokeDinner-Eglantine 2.0.0** setup: your own keys, firmware and game dump,
  set up as for 2.0.0. NX2Perf provides none of these and never will.
- The **Microsoft Visual C++ 2015-2022 x64** runtime (2.0.0 needs it too). The program imports
  exactly the same DLLs as 2.0.0's `eden-cli.exe`; nothing else is to be installed.
- A GPU with a current **Vulkan** driver (as for 2.0.0). Only an NVIDIA RTX 4060 Ti was tried.
- An x86-64 CPU. The build asks for no AVX2 (the only AVX2 code is in the Opus audio codec,
  which checks the CPU at run time), but only a Ryzen 7 5700X was tried. Many changes speed up
  the GPU thread on a fast desktop CPU; on a slow CPU expect less.
- Multicore CPU emulation on (the default). The frame pacing switches need it.
- Optional: RivaTuner Statistics Server (RTSS) for the on-screen FPS / speed display.

## Install

1. Open the folder of your PokeDinner-Eglantine 2.0.0 (the one with `eglantine.exe` and
   `eden-cli.exe`).
2. Copy everything from the NX2Perf zip into it. Nothing is overwritten: the program has its
   own name (`eden-cli-nx2perf.exe`) and 2.0.0 keeps working as before.
3. NX2Perf uses the same user folder as 2.0.0's `eden-cli.exe` (`user\` next to the program
   in a portable setup, otherwise `%APPDATA%\eden`): same keys, saves and settings.
   eden-cli keeps its settings in `config\sdl2-config.ini` there (the Qt program
   `eglantine.exe` uses `qt-config.ini`; the two are separate). Run the program once and close
   it if that file does not exist yet.

To uninstall, delete the files the zip added. Shader caches the new program built stay in the
user folder; they are safe to keep.

## Launchers

Drag your game file onto a launcher, or open the `.bat` in a text editor and write the path in
the `GAME=` line. The top of each launcher has the lines meant to be changed:

| Line | Meaning |
|---|---|
| `GAME=` | the game to start (a dragged file wins) |
| `CONFIG=` | another `sdl2-config` .ini for this launcher; empty = eden-cli's own |
| `FULLSCREEN=-f` | full screen; empty = window |
| `DK_DYN=1` | Bananza dynamic frame rate patch (below); `0` = off |
| `OSD=1` | FPS / speed display through RTSS; `0` = off |

(`DK_DYN=` and `OSD=` are in the recommended and reactive launchers only.) If the path of your
game contains `&`, `^`, `,`, `;` or `=`, write it in the `GAME=` line instead of dragging the file.
A launcher stops with a message when its `CONFIG=` file does not exist (eden-cli would quietly
make a new one with default settings), and keeps its window open when the emulator exits with an
error. Take the launchers from the release zip: they need Windows line endings (CRLF).

- **`start-recommended.bat`** - the settings we play with (Donkey Kong Bananza):
  the GPU thread's back as stages (`BACK_STAGES=1`, `BACK_CUTS=a`), `ACQUIRE_PEEK=1`,
  `JIT_SAME=1`, `FRONT_MACRO_LEAN=1`; Bananza's own 60/30/20 fps choice followed at the right
  speed (`PRESENT_INTERVAL_WORD=12:16:4`, `MODE_PACE=2`, `PACE_SPIN_US=1500`); the dynamic
  frame rate patch (`DK_DYN=1`; while it is applied, the two pacing lines above are set aside:
  they act with `DK_DYN=0` or when the patch does not apply); timed FSR3 generated frames (`FG_TIMED=1`, only does something
  with frame generation on); the RTSS display (`OSD=1`). The Bananza-specific lines are marked;
  for other games use the safe launcher, or remove them.
- **`start-safe.bat`** - every switch this fork added turned off (the shortcuts that are on by
  default set to `0`, the opt-in ones cleared). This is as close to 2.0.0 as the program gets;
  use it to see whether a problem comes from the new switches. Changes without a switch (the
  accuracy fixes and upstream picks in the changelog) stay in.
- **`start-reactive-experimental.bat`** - EXPERIMENTAL, the second choice next to the
  recommended one: the recommended settings with reactive flushing on, plus `RX_RT_FIT=3` (one
  image for Bananza's opening render target instead of a copy per draw). On our machine it trades
  the opening for open areas: -7% FPS at the game's opening, +15% in the open area, against the
  same build with reactive flushing off. Not checked yet: together with frame generation, and the
  image with `RX_RT_FIT=3`. Reactive flushing is a config setting, not a switch: see "Reactive
  flushing" below.

eden-cli reads the switches only from its environment (the launchers).
`nx2_switches_NX2PERF.txt` lists every switch with its default and what it does. (2.0.0's own
`nx2_switches.txt` is read by the Qt program only; the package does not replace it.)

## Main switches

All are environment variables named `EDEN_NX2_<NAME>`. The full list (about 230 new names, most
of them diagnostics or tuning knobs) is in `nx2_switches_NX2PERF.txt`.

| Switch | Default | What it does |
|---|---|---|
| `BACK_STAGES=1`, `BACK_CUTS=a` | off | the split GPU thread's back runs as a pipeline of stages |
| `ACQUIRE_PEEK=1` | off | a GPU semaphore wait already met goes on without draining the back |
| `JIT_SAME=1`, `FRONT_MACRO_LEAN=1` | off | the front's macro JIT drops writes that change nothing |
| `PRESENT_INTERVAL_WORD=12:16:4` | off | presents follow the game's own frame interval (Bananza) |
| `MODE_PACE=2`, `PACE_SPIN_US=1500` | off | steers Bananza's 60/30/20 fps mode by frame timing |
| `DK_DYN=1` | off | Bananza dynamic frame rate patch, in memory (below) |
| `DK_DYN_MIN_FPS=<n>` | 40 | the patch's lower limit (30 measured worse) |
| `FG_TIMED=1` | off | FSR3 generated frames blended for the time they are shown |
| `OSD=1` | off | game FPS, display FPS (with frame generation's multiplier) and speed in RTSS |
| `LIGHT_DRAW`, `LIGHT_UNIFORM`, `UBO_*`, `SYNC_*`, `VERTEX_*`, `RESET_*`, ... (about 60) | **on** | per-draw shortcuts on the GPU thread; `=0` is the 2.0.0 code path, many accept `=2` to check themselves and log |
| `HLE_INDIRECT`, `INDIRECT_BATCH`, `PBFLUSH_WINDOW_KIB` | **on** | Bananza's terrain: indirect draws on the host GPU, merged; GPU-written parameters flushed in 256 KiB windows |
| `RX_RT_FIT=3` | off | with reactive flushing: Bananza's opening renders into one image instead of copying it per draw, ordered by a barrier inside the render pass (experimental; `=1` ends the render pass instead, slower; both need `barrier_feedback_loops=true`) |
| `FLUSH_FIRST_READ`, `READ_LATCH_AGE`, `RX_ONE_SUBMIT`, `RX_COALESCE`, `RX_PITCH_FORCED` | off | reactive flushing experiments; no gain measured, not recommended |
| `RX_MASKED_TARGETS` | off | **known defect, do not turn on** |
| `RX_COALESCE_LOOSE` | off | diagnostic only (it weakens the ordering reactive flushing gives): not for play |

## Donkey Kong Bananza dynamic frame rate (DK-DYN)

`EDEN_NX2_DK_DYN=1` applies a game-specific patch: when Bananza's main module is loaded, a few
instructions of the game's frame rate controller are rewritten **in memory** so that the game
may run at any rate from 40 to 60 fps instead of dropping to 30, with its clock stepped by the
real frame time (the game's speed stays right down to 24 fps). It is a port of the `dk_dynamic_fps` patch of
the ZendNevs fork (v2.0.5). Nothing is written to disk, no game file is changed, and with the
switch unset no module is touched. It only applies to Bananza (title 040074A01BF12000) and
only if the expected instructions are found. `DK_DYN_MIN_FPS=<1-60>` moves the 40 fps limit.

It is off by default in the program; `start-recommended.bat` turns it on (`DK_DYN=1` at its
top). With it the game's frame time varies by design (any rate between 40 and 60, in steps of
2); below 40 fps the game still drops to its 30 fps rule. While it is applied,
`PRESENT_INTERVAL_WORD` and `MODE_PACE` are set aside and presents are paced to every vblank.

## Frame generation (optional)

PokeDinner-Eglantine 2.0.0 already has frame generation; NX2Perf adds `FG_TIMED` and shows the
multiplier on the OSD. Two kinds exist:

- **FSR3** (built in, the default kind). This is what we measured.
- **LSFG** (Lossless Scaling frame generation). It needs `Lossless.dll` from the paid
  *Lossless Scaling* app. **NX2Perf does not include it**; put your own copy in the user
  folder's `lossless\` folder if you want this kind.

To turn FSR3 frame generation on for eden-cli, close the program and add these lines under
`[Renderer]` in `config\sdl2-config.ini` (the `\default=false` lines make eden-cli read the value):

```ini
frame_gen\default=false
frame_gen=true
frame_gen_target_rate\default=false
frame_gen_target_rate=120
```

`frame_gen_target_rate` is the frame rate frame generation aims for (we use 120 on a 120 Hz+
display; `0` uses `frame_gen_multiplier`, 2-4). `frame_gen_type=0` would pick LSFG.

## Reactive flushing

`use_reactive_flushing` (`[Renderer]`) copies data the GPU wrote back to game memory when the
game reads it. **The program's default on desktop is on.** Our test setup had it **off**, and
the recommended numbers below were measured that way. On our machine, with earlier builds and
frame generation off, turning it on gained +13 to +23% FPS in open areas and lost 23 to 30% at
the game's opening. Look at your `sdl2-config.ini`:

```ini
use_reactive_flushing\default=false
use_reactive_flushing=false
```

matches our recommended setup; `=true` (or no such lines) is reactive flushing on, the setting
`start-reactive-experimental.bat` is meant for. To keep both, copy the .ini (for example to
`sdl2-config-reactive.ini` in the same folder), change the copy, and write the copy's full path
in a launcher's `CONFIG=` line.

This release adds switches for reactive flushing (all off by default; `nx2_switches_NX2PERF.txt`,
"reactive flushing"). `RX_RT_FIT` targets the cost found at the game's opening: there Bananza
renders into a 2112x1179 image while it samples the same memory as a 2095x1179 one, and with
reactive flushing every draw copied one into the other. With the switch the two are one image,
and a draw that reads it has to come after the draws that wrote it: `=1` ends the render pass
before such a draw; `=3` records a barrier inside the render pass instead (color attachment
writes before fragment shader reads, by region), so the render pass goes on. Both need
`barrier_feedback_loops=true` under `[Renderer]` (the switch does nothing without it).

Measured on 5 November (DK-DYN on, frame generation off, LTO + PGO builds, game FPS of two runs
per arm; each row compares one build with reactive flushing off and on):

| Arms (one build per row) | Opening | Open area |
|---|---|---|
| reactive off -> on + `RX_RT_FIT=1` | 43.4 / 42.9 -> 32.9 / 33.8 (-23%) | 46.4 / 46.8 -> 52.6 / 52.9 (+13%) |
| reactive off -> on + `RX_RT_FIT=3` | 42.7 / 40.8 -> 38.4 / 39.0 (-7%) | 45.4 / 46.2 -> 52.4 / 52.5 (+15%) |

`RX_RT_FIT=3` is clearly better than `=1` at the opening (+16%) and the same in the open area.
Against reactive flushing off it is a trade that depends on the scene: a little slower at the
opening, faster in open areas. That is why the release has two launchers for play:
`start-recommended.bat` (reactive flushing off) and `start-reactive-experimental.bat` (on, with
`RX_RT_FIT=3`). **Experimental:** reactive flushing was **never measured together with frame
generation**, and the image with `RX_RT_FIT=3` has not been checked by eye yet (its barrier
inside the render pass covers a fragment reading the pixel it renders, which is what the Vulkan
spec defines for feedback loops). If you see wrong images with it, set `RX_RT_FIT=1` in the
launcher or use the recommended one. Do not turn on `RX_MASKED_TARGETS` (known defect: a texture
blit ignores the mask) or `RX_COALESCE_LOOSE` (diagnostic only).

## Measured results

Measured on the machine above with Donkey Kong Bananza only. FPS is the game's own frame rate
(frame generation off unless stated), read from RTSS's shared memory or from the emulator log.
"Same build" rows switch one thing in one program. Scenes: **opening** (the save's starting
area), **open area** (a wide view of the first level), **far view** (looking over the level by
hand; camera not exactly the same between runs).

| Change | Arms (A -> B) | Scene, runs | Result |
|---|---|---|---|
| Everything, PokeDinner 2.0.0 -> NX2Perf | 2.0.0's `eglantine.exe` (Qt; handheld mode, GPU accuracy Fast, 1x) / this fork's `eden-cli` builds of 3-5 Nov (mostly TV mode) | 2.0.0: far view by hand, 25 s (31 Oct). Fork: automated runs of the opening and open area | 2.0.0: 17.3 FPS (median frame 58 ms). Fork: 42-53 FPS uncapped (3-4 Nov); 39-46 FPS with DK-DYN, frame generation off (5 Nov, build of commit `b5a994e3eb`). **Not a controlled A/B**: program, mode, scenes and runs differ, and the 2.0.0 scene is the heaviest one; in the same far view the fork's builds of 31 Oct - 1 Nov measured 19.7 and ~30.8 FPS (rows below). A rough indication only |
| GPU-written parameter flush window | same build (31 Oct, before most other changes), `PBFLUSH_WINDOW_KIB=0` -> default | far view, 2 x 25 s each | 18.0 -> 19.7 FPS (+9%; measured before `HLE_INDIRECT`, so it matters mostly with `HLE_INDIRECT=0`) |
| Terrain indirect draws on the host GPU | same build (1 Nov), `HLE_INDIRECT=0` -> default | far view by hand, 2-4 runs each | ~20.8 -> ~30.8 FPS (~+48%; camera not identical) |
| Back stages (cut a) | `BACK_STAGES=0` -> `1` | opening, 2 rounds x 2 x 25 s | +8.7% / +7.3% |
| Per-draw shortcuts (the first set, 2 Nov; not all of them) | off -> on | opening, same runs (one sample of round 2 left out) | +7.5% / +6.8% |
| Acquire peek | `ACQUIRE_PEEK=0` -> `1` | opening, same runs | +1.4% / +3.9% |
| Profile-guided optimization (PGO) | LTO build -> LTO + PGO | opening, same runs | +21.5% / +15.8% (3 Nov build and profile; this release's program has its own profile, not measured this way) |
| Front macro JIT filter | off -> `JIT_SAME=1` + `FRONT_MACRO_LEAN=1` | opening, 2 rounds x 2 | 39.6 -> 42.1 FPS (+6%) |
| Reactive flushing | off -> on | open area / opening, nightly automated runs (3-4 Nov builds) | +13 to +23% / -23 to -30% |
| Reactive flushing with `RX_RT_FIT=1` | same build, reactive off -> on + `RX_RT_FIT=1` (DK-DYN on) | opening / open area, 2 runs each (5 Nov) | -23% / +13% |
| Reactive flushing with `RX_RT_FIT=3` | same build, reactive off -> on + `RX_RT_FIT=3` (DK-DYN on) | opening / open area, 2 runs each (5 Nov) | -7% / +15% (`=3` against `=1`: +16% at the opening, the same in the open area) |
| `DK_DYN_MIN_FPS=30` | unset (40) -> 30 | opening and open area, 2 runs per arm in each scene | -4% / -5% (FPS ranges overlap at the opening; more frames near 30 is the clearer sign): not recommended |

With DK-DYN and FSR3 frame generation aiming at 120 (an earlier build of the recommended
setup), a 3.6 minute session of normal play logged a median of 46 game FPS (lowest 35), 119
frames a second on screen (lowest 97) and a median game speed of 99.8% (lowest 93%). That is one session, not a
benchmark.

## Known issues

- **Only the command line program.** The Qt program (`eglantine.exe`) is not part of this
  release: the build and the PGO profile are for `eden-cli`. Keep using 2.0.0's `eglantine.exe`
  for its menus; settings changed there do not reach eden-cli (`qt-config.ini` vs
  `sdl2-config.ini`).
- **Black blocks with a bright edge** appear in some places. They were seen in a session with
  frame generation and, before that, without it; the likely cause is an older rendering issue of
  this game (also reported for upstream), not frame generation, but it is not settled. If you
  see them, note whether they stay in place in the world, try without frame generation and with
  `start-safe.bat`, and report where.
- **Reactive flushing** (on by default) costs FPS at the game's opening (above); its new
  switches were not measured with frame generation.
- **AMD and Intel GPUs were not tried**; neither were other games. The Bananza switches
  (`PRESENT_INTERVAL_WORD`, `MODE_PACE`, `DK_DYN`) are meant for Bananza only.
- The OSD writes to RTSS's shared memory; if the program is killed (Task Manager), the text can
  stay on screen until RTSS is restarted.
- A program built with profile-guided optimization is fastest on the paths it was trained on
  (Bananza, the recommended switches). Other settings work, maybe a little slower.

## AI disclosure

Most of this fork's code, its commit messages and these documents were written by an AI coding
assistant (Claude Opus 5.5, by Anthropic) working under the direction of the human maintainer,
who decided what to try, played and tested the builds, and is responsible for the release. The
measurements above were taken on the maintainer's PC, many of them by automated runs. Every
commit made with the assistant says so with an `Assisted-by:` line, as the Eden project's AI
policy asks (`docs/policies/AI.md`). This fork is published on its own and is not submitted
upstream. The upstream commits picked into it are listed with their authors in the changelog.

## License

Like Eden and PokeDinner-Eglantine, NX2Perf is licensed under the **GNU General Public License
v3.0 or later** (`LICENSE.txt`). It is a modified version of PokeDinner-Eglantine 2.0.0; what
was changed and when is in `NOTICE_NX2PERF.txt` (in the release zips) and
`CHANGELOG_NX2PERF.md`. The complete
corresponding source code of this release is at `<KAYNAK_URL>` and in the `-source.zip` next to
the program, with notes on how the third-party libraries are fetched (`BUILDING_NX2PERF.md`).
Third-party components keep their own licenses: their notices and license texts are in
`THIRD_PARTY_NOTICES_NX2PERF.txt` and `LICENSES\`; the source of the libraries built into the
program is in the `-deps-source.zip`.

Nintendo Switch and Donkey Kong Bananza are trademarks of Nintendo. This project has no
affiliation with Nintendo. Use it only with games you own.
