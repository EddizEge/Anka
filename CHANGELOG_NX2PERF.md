# NX2Perf changelog

Changes since **PokeDinner-Eglantine 2.0.0** (commit `70ecb39ef0`). The full history is in git
(`git log 70ecb39ef0..`); this file groups it by topic. Most changes are behind `EDEN_NX2_*`
switches (`nx2_switches.txt`): the opt-in ones are off unless a launcher sets them, the
per-draw shortcuts are on and `=0` brings back the 2.0.0 code path.

## NX2Perf 1 (`2.0.0-nx2perf1`)

Base: PokeDinner-Eglantine 2.0.0. Program: `eden-cli` (SDL front end), built with LTO and
profile-guided optimization. About 390 commits, made between 31 October and 5 November 2026
(modified by the NX2Perf maintainer; see `NOTICE_NX2PERF.txt` in the release zips).

### Donkey Kong Bananza terrain and the GPU command stream

- NVN's indexed indirect draw records are drawn on the host GPU (`vkCmdDrawIndexedIndirect`)
  instead of being read back on the CPU each frame (`HLE_INDIRECT`, on).
- Consecutive terrain indirect draws merge into one multi-draw on the split back
  (`INDIRECT_BATCH`, on); the indirect draw reset's uploads are batched, sorted and written in
  runs (`RESET_BATCH`, `RESET_SORT`, `RESET_RUNS`, `RESET_LAST`, on).
- GPU-written macro parameters are flushed with the 256 KiB window around them, and no second
  drain follows a flush check (`PBFLUSH_WINDOW_KIB`).
- Macro parameters are executed without a second FIFO copy (snowstormxd, picked from upstream).

### Split GPU thread

- The back can run as a pipeline of stages (`BACK_STAGES=1`, `BACK_CUTS=a`, off by default): the
  engines on stage A, memory and recording on BC, with register replicas, rings and a join
  protocol. A three-stage cut (`BACK_CUTS=c`) is in as an experiment, not for play.
- Macro writes travel as one ring entry (`WRITE_BATCH`), batch entries run in the engine's own
  loop (`BATCH_DIRECT`), constant buffer data words are written in runs (`CB_RUNS`); the GPU tick
  skips its lock when nothing is pending (`TICK_SKIP`). All on.
- Front: a semaphore acquire that memory already meets no longer drains the back
  (`ACQUIRE_PEEK=1`, opt-in); the macro JIT drops state writes that change nothing and the front
  macro calls are leaner (`JIT_SAME=1`, `FRONT_MACRO_LEAN=1`, opt-in).
- Optional physical cores of their own for the back and the front (`BACK_CPU`, off).

### Per-draw work on the GPU thread (on by default; `=0` turns each off)

- Light draws: draws that change only buffer addresses, uniforms or storage buffers set up only
  what changed (`LIGHT_DRAW`, `LIGHT_UNIFORM`, `LIGHT_UNIFORM_VALUES`, `LIGHT_STORAGE`,
  `UNIFORM_SYNC_SKIP`, `STORAGE_KEEP`, `LIGHT_SYNC_SKIP`).
- Buffers: uniform buffers reused by content and by stage slot (`UBO_REUSE`, `UBO_DIRECT`,
  `UBO_STAGE`, `UBO_BIND_SKIP`), remembered storage buffer bindings (`SSBO_MEMO`, `SSBO_FAST`),
  synchronizations that would find nothing skipped and remembered (`SYNC_SKIP`, `SYNC_EXACT`,
  `SYNC_MEMO`, `SYNC_SETS`), cheaper vertex and index buffer updates (`VERTEX_SKIP`,
  `VERTEX_FAST`, `VERTEX_CLEAN`, `INDEX_SKIP`, `LAYOUT_MEMO`), GPU-written pages marked a word at
  a time and answered from a summary (`MARK_WORDS`, `GPU_WRITTEN_FILTER`).
- Memory and caches: constant buffer writes of bytes memory already holds skipped
  (`SAME_SKIP`), small guest writes through remembered page pointers (`SMALL_WRITE`), cheaper
  invalidations, range sets, slot vectors, LRU touches, usage trackers, query and shader lookups
  (`INVAL_HULL`, `INVAL_SKIP`, `RANGE_SKIP`, `SLOT_SKIP`, `TOUCH_SKIP`, `TEX_TOUCH_SKIP`,
  `ALIAS_LAZY`, `USAGE_SKIP`, `USAGE_RESET_SKIP`, `QUERY_BOUNDS`, `SHADER_BOUNDS`,
  `DIRTY_THUNK_SKIP`, `DESC_XLAT`).
- Descriptors and pipelines: image descriptors reused when a stage draws with the same views
  (`DESC_REUSE`, `DESC_HULL`, `DESC_SECONDARY`), pipeline keys compared hashes first
  (`KEY_CMP`), the Vulkan worker woken less often (`NOTIFY_LAZY`).
- The kernel's priority queue finds the first set priority with one instruction
  (`FAST_PRIORITY_SCAN`). Per-draw switches are read once into atomics.

### Frame pacing (Donkey Kong Bananza; all opt-in)

- vi2 present diagnostics, a present interval taken from the game's request
  (`PRESENT_INTERVAL_WORD=12:16:4`: the game's own 60/30/20 fps choice at the right speed),
  a fixed interval, a 60 Hz vblank grid, presents paced by the request's refresh rate
  (`PRESENT_INTERVAL`, `VBLANK_GRID`, `PRESENT_RATE`).
- A controller that steers the game's 60/30/20 fps mode by holding presents (`MODE_PACE=1`, and
  v2.1 with `MODE_PACE=2`), with tuning knobs, a spin before each wake (`PACE_SPIN_US`) and
  sleeps in the guest kernel (`PACE_GUEST_SLEEP`). An earlier governor through the GPU timer
  (`MODE_GOV`), a busy-time GPU clock and a GPU timer divisor (`GPU_BUSY_CLOCK`,
  `GPU_TICK_DIV`).
- vi2 experiments for the VRR path: reply tables, kind events, notifications, VRR vsync events
  (`VI2_*`, `VRR_VSYNC`).

### Donkey Kong Bananza dynamic frame rate (DK-DYN, opt-in)

- `DK_DYN=1`: a port of the ZendNevs fork's `dk_dynamic_fps` patch (v2.0.5). While the game's
  main module loads, its frame rate controller is patched in memory to run anywhere from 40 to
  60 fps with the clock stepped by the real frame time. vi2 then reports VRR support and paces
  presents to every vblank (`PACE_ONE`). The check words are read inside the text segment only.
- `DK_DYN_MIN_FPS=<1-60>` moves the controller's 40 fps limit.

### Frame generation

- The built-in optical flow frame is interpolated between two real frames; its motion image
  stays in `GENERAL` layout and its center term is blended forward.
- `FG_TIMED=1`: FSR3 generated frames are blended for the time they are shown (the display
  refresh asked from Windows, or `FG_REFRESH_HZ`); `FG_DIAG` logs the timing.
- float16 stays on for Ampere and newer when frame generation starts with LSFG.
- The frame counters count the images sent to the screen, generated ones included.

### Reactive flushing (all off by default)

For `use_reactive_flushing=true`; none of these was measured with frame generation (README,
"Reactive flushing").

- `RX_RT_FIT=1`: a render target lookup takes the narrower image with the target's memory when
  the guest's surface clip is exactly that image (Bananza's opening renders into a 2112x1179
  target and samples the same memory as a 2095x1179 texture: one image instead of a whole copy
  per draw); a color feedback loop ends the render pass first (`barrier_feedback_loops=true`).
  `=2` (diagnostic) leaves the barrier out.
- Experiments without a measured gain: `FLUSH_FIRST_READ` (a buffer page the GPU wrote is
  flushed at the guest's first CPU read), `READ_LATCH_AGE` (what CPU reads latched is dropped
  after that many frames unread), `RX_ONE_SUBMIT` (no empty second submission after a fence's
  own), `RX_COALESCE` (buffer copy backs ride along with fences made anyway), `RX_PITCH_FORCED`
  (pitch linear images stay downloaded at every bind).
- `RX_MASKED_TARGETS`: a draw marks only the targets its masks let it write. Known defect (a
  texture blit ignores the mask): do not turn it on.
- Diagnostics: `REACTIVE_STATS` (counters and a log line about every 10 s), `RX_NO_COPYBACK`,
  `RX_NO_RT_LATCH`.

### On-screen display

- `OSD=1` (eden-cli on Windows): game FPS, display FPS with frame generation's multiplier, and
  the game's speed in RivaTuner Statistics Server's on-screen display every 0.5 s; the slot is
  emptied on every exit.

### Shaders and accuracy

- Ampere: HSETP2 with a register operand is lowered (`HSETP2_REG`); TXD with any channel mask;
  FRND with a constant buffer source (Takoroka); Bananza's shifted QMD layout accepted.
- HMMA: emulated by default instead of rejecting the program, f32 accumulation rounded every
  eight products, FP16 texture results packed with c0 in the low half (Franseven, picked from
  2.5.0); programs that use R240-R245 stay rejected, and a stand-in keeps a program whose
  pipeline fails to build; pipeline cache versions bumped for the new lowering.
- A stand-in for Bananza's tensor-core upscalers, found by size (`UPSCALE_*`).
- Register write history is opt-in in release builds (snowstormxd; `3D_HISTORY`).
- The core index of rasterizer downloads is clamped; invalid sampler and image view indices are
  logged at trace level; a failed GPU wait is logged before it ends the process.
- Video decoder: end of stream is marked only on a picture of the last input; post-stream
  release fences stay pending (Javier; `VDEC_EOS_RELEASE_NOW` releases them at once).

### Picked from upstream and other forks (authors kept)

| Commit | Author | Change |
|---|---|---|
| `c4bcb5c6b6` | gabrielskaminski | shader_recompiler: include all components in IsGeneric upper bound |
| `cddcc29e19` | Dee1 | audio_core: unregister audio in/out managers before they are destroyed |
| `c30d1fe6f2` | Dee1 | audio_core: keep audio in/out managers alive while their sessions are open |
| `bad8b8be6e` | Franseven | shader: pack FP16 texture results with c0 in the low half |
| `e92f58640b` | Franseven | ampere: emulate HMMA by default instead of rejecting the program |
| `8fcd545413` | Franseven | shader: accumulate HMMA.1688/.16816 in f32, round every eight products |
| `9e68d71c54` | snowstormxd | gpu: make register-write history opt-in in release builds |
| `9fd333951b` | snowstormxd | macro: execute native parameters without a second FIFO copy |
| `b1f345637d` | Takoroka | nx2: lower FRND with a constant buffer source |
| `f41201ea87` | Franz | Prevent Memory Block ID Overflow and Validate Page Contiguity |
| `50f66e2d1b` | Duck912 | video_core: read the framebuffer before recording the upscaled-output blit |
| `9ccbf431d4` | rdenno | vk_texture_cache: give framebuffers the fewest layers of their attachments |
| `47ef44ccc8` | rdenno | texture_cache: look up X8Z24 with UNORM, UINT, UNORM, UNORM components |
| `85b0547f38` | ZnOw01 | video_core: bound-check macro position writes |
| `5b7f569115` | Javier (Xenon-Hacks) | nx2: video decoder: keep post-stream release fences pending |
| `5eebf45ab7` | Zoe L. | fix: avoid variable shadowing |

Ideas ported from other forks, credited in the code: Selevet (`SLOT_SKIP`, `USAGE_RESET_SKIP`,
`ASYNC_SKIP`), elBart0 (`UBO_BIND_SKIP`, `KEY_CMP`), Duck912 / moceure's eden-nx2-android
(`INLINE_FIX` and others), ZendNevs (`DK_DYN`, `PACE_ONE`).

### Diagnostics and tools (logging only)

- `EDEN_NX2_GPU_PROFILE` zones and counters for the split thread, its front and the back
  stages; `MODE_DIAG`, `PRESENT_DIAG`, `VI2_DIAG`, `HID_DIAG`, `INDIRECT_DIAG`, `FG_DIAG`,
  `VDEC_TRACE`, `EDEN_NX2_IPC_TRACE` (service request backtraces), root table diagnostics.
- eden-cli: a timed key script on a virtual gamepad and an automatic exit for unattended
  measuring (`AUTO_INPUT_FILE`, `AUTO_EXIT`).
- Unit tests for the new logic (`src/tests/`).

### Build

- `/QIntel-jcc-erratum` is a build option (`YUZU_MSVC_JCC_ERRATUM`).
- Release program built with link-time code generation and profile-guided optimization
  (`BUILDING_NX2PERF.md`).

### Not in this release

- The Qt program (`eglantine.exe`).
- A reactive flushing launcher that is recommended: `start-reactive-draft.bat` stays a draft
  until reactive flushing with `RX_RT_FIT` is measured in the open area and with frame
  generation.
- The "M" stage of the split GPU thread work.

### Documentation

- `NX2_NOTES.md` and `NX2_GPU_THREAD_SPLIT.md`: what was tried, measured and why.
- `nx2_switches.txt`: every switch with its default.
- `README_NX2PERF.md` / `BENIOKU_NX2PERF.md`, this file, `BUILDING_NX2PERF.md`, the launchers in
  `dist/nx2perf/`.
