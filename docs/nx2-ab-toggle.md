# A/B within one run: `EDEN_NX2_AB_TOGGLE`

Run-to-run noise in Anka's FPS measurements is large (about 6 % between runs of the same build,
mostly a per-run offset), while consecutive 600-present blocks of one run differ by only ~4.7 %
and are independent. Comparing a feature off and on **inside one run**, in alternating blocks,
removes the per-run offset: 11-16 block pairs (7-11 minutes, one or two launches) resolve a 3 %
difference that would take 40+ runs per arm otherwise.

## Switches

| Variable | Meaning |
|---|---|
| `EDEN_NX2_AB_TOGGLE=<N>` | Every `N` presents the global phase flips A ↔ B. Unset or 0: off (nothing counted, "ab" switches stay in the start phase). |
| `EDEN_NX2_AB_DISCARD=<K>` | The first `K` presents after a flip belong to no block (default 60): the frames in flight from the other phase drain. Held below `N`. |
| `EDEN_NX2_AB_START=B` | The first phase is B (a second launch with the order reversed; the mean of an A-first and a B-first run cancels any order bias). |

Each phase ends with one log line (`Service <Info>`), the first five fields fixed in this order:

```
NX2_AB blok=<n> faz=<A|B> sunum=<presents> sure_ms=<ms> fps=<x.xx> max_ms=<ms> ust100=<count>
```

`sunum` = `N - K` frame intervals, `sure_ms` their total, `fps = sunum / sure`. `max_ms` is the
longest interval, `ust100` the intervals over 100 ms (loading, hitches; tools may drop such
blocks). A start line `NX2_AB on (EDEN_NX2_AB_TOGGLE): ...` gives the settings.

The counting is on the vi2 present thread (`core/hle/service/nx2_gsv.h`, next to the
`nx2:vi2 last 600 presents` statistics) and flips the phase between two of the game's frames:
the game's next frame runs in the new phase. Off, the present path reads one static bool.

## Binding a feature switch to the phase

`common/nx2_ab.h`:

```cpp
#include "common/nx2_ab.h"

// Where the switch is read today, e.g. static const bool on = Parse(getenv("EDEN_NX2_FOO"));
static const Common::Nx2Ab::Switch foo = Common::Nx2Ab::Switch::Parse(
    std::getenv("EDEN_NX2_FOO"), ParseFooTheOldWay(std::getenv("EDEN_NX2_FOO")));

if (foo.Now() != 0) { ... }          // the phase now: one relaxed atomic load
```

- `EDEN_NX2_FOO=ab` → 0 in A, 1 in B. `EDEN_NX2_FOO=ab:2` → 0 in A, 2 in B (switches with several
  on values). Any other value is the feature's own reading, fixed for the run.
- `foo.EverOn()`: whether the feature can be on at all in this run. Set up what the on path needs
  (buffers, threads) when `EverOn()`, not when `Now()` is true at start.
- **Safe point:** `Now()` can change between two calls. A feature that must not mix the two modes
  inside one unit of work (a frame, a command buffer, a batch) keeps a `Common::Nx2Ab::Latch` on
  its own thread and calls `Update()` only at that boundary; `Update()` returns true when the phase
  changed, which is the place to flush or reset mode-specific state. `latch.Value(foo)` gives the
  switch's value in the latched phase.
- The presents counted are the guest's. A GPU-thread or render-thread feature sees the flip a frame
  or two later than the guest: that is what the discard covers. Keep `K` at least the depth of the
  pipeline the feature sits in (60 presents is ~1.5-2 s at 30-40 FPS).
- Log the binding once ("EDEN_NX2_FOO follows the A/B phase") so a run's log shows it.

### What to check when flipping

1. **Both directions must be correct at any boundary.** Switching on must not rely on state the
   off path never built; switching off must not leave work only the on path would finish (pending
   writes, deferred flushes, held fences). Flush or drain at the latch change if so.
2. **No state that encodes the mode.** Caches whose entries were built under one mode must be
   valid under the other, or must be keyed / invalidated at the flip.
3. **Cost of the flip itself** belongs to the discard window, not to a block. A flip that
   invalidates a cache makes every block start cold; that is part of the measured difference.
4. **Pacing features** (anything that caps or holds frames) cannot be compared for throughput this
   way: a block measures FPS, which they set by design.

### What cannot be bound

- Compiler, optimisation level, PGO / LTO, `#if` code paths: one binary per run.
- Structures built at start (allocations sized by the switch, threads created or not, Vulkan
  device features / extensions, queue setup).
- Choices that enter the **shader or pipeline cache key** (shader translation options, specialisation
  constants, pipeline state): the two phases would compile different pipelines, the disk cache mixes
  them, and the first blocks of each phase measure compilation.
- Switches read once into a `static const` deep in a path that cannot reach a safe point, unless
  that path is reworked to read a `Switch` / `Latch`.
- Features whose effect outlasts a phase (e.g. memory layout decisions that stay for the run): the
  B phase would leak into later A blocks.

## Measuring

- Throughput profile (no `DK_DYN`, `MODE_PACE`, `PRESENT_INTERVAL_WORD`): pacing makes blocks
  identical and hides the difference.
- `N=600`, `K=60`: one block ≈ 540 presents ≈ 15 s at 36 FPS; a 10 minute window gives ~18 pairs.
- First an A/A run (no switch bound): the A−B mean should be zero within its confidence interval;
  its pair SD is the method's noise.
- Pair adjacent blocks (2k, 2k+1) and report the mean A−B difference with a t-based 90 %
  interval; run once with `AB_START=A` and once with `AB_START=B` to cancel any order effect.
  Comparing each B block with the mean of its two A neighbours also removes a linear drift
  within a run.

## Bound so far

- `EDEN_NX2_TICK_SKIP=ab` (`video_core/gpu.cpp`, `TickWork`): A looks at the GPU tick queue under
  its lock after every draw (the old way, `=0`), B skips it when the queue is empty (the default).
  Both are correct at any tick; each call takes the phase itself.

## Candidates (Anka branches, as of 2026-11-10)

| Switch | Branch | Bindable | What the flip needs | Effort |
|---|---|---|---|---|
| `EDEN_NX2_B_PAD_NS` (deliberate per-draw delay) | anka/b-hiz-2 | yes | calibrate at start whatever the phase; `Pad()` spins only in B | easy; a positive control for the method |
| `EDEN_NX2_UBO_KEEP` | anka/b-hiz-2 | yes, with care | the per-stage "last bind streamed" bits are only kept while on: clear them at off→on (draw boundary, latch) or keep them always; read the phase once per draw (rasterizer and buffer cache both look) | medium; verify with `=2` across flips |
| `EDEN_NX2_READ_TRAPS` | anka/read-trap | no | correctness mechanism (A would be known-wrong); page protections, trap maps and Dynarmic's recompiled reads persist | — |
| `EDEN_NX2_NULL_AUDIO_PACE` | anka/qt-hiz-2 | technically easy, not useful | pacing: it sets how often the game runs, not how fast the emulator is | — |
| `EDEN_NX2_TEX_FLUSH_HELD` | anka/dogru | not useful | correctness fix (A known-wrong), needs the run-fixed `TEX_FLUSH_KEEP_CPU` | medium |
| `EDEN_NX2_FAST_PRIORITY_SCAN` | anka/110-dkb | yes | pure function (same result either way) | easy |
| `EDEN_NX2_BATCH_DIRECT` | anka/110-dkb | yes | dispatch choice, no state | easy |
| `EDEN_NX2_DIRTY_THUNK_SKIP` | anka/110-dkb | yes | skips a thunk nobody reads; front thread | easy |

Not bindable on anka/110-dkb: `JIT_SAME`, `FRONT_MACRO_LEAN` (in JIT-compiled code), `CB_RUNS`
(set up in the Maxwell3D constructor), `KEY_CMP` (pipeline cache), `BACK_STAGES` / `SPLIT`
(threads built at start); `LAYOUT_MEMO`, `UBO_REUSE` / `UBO_STAGE`, `LIGHT_*`, `INDEX_SKIP` keep
memos that would need invalidating at a latch change.
