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
the game's next frame runs in the new phase. A block's `blok=` and `faz=` come from the run's
global flip count (what the features read), not from the present session's own count. Off, the
present path calls `Enabled()` once a present: a function-local static (a guarded load) and a
compare.

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
- The presents counted are the guest's. The flip happens while the guest waits in its present
  call, but the GPU thread may then be in the middle of the previous frame (`frames_ahead` waits
  for an older fence): a GPU-thread or render-thread feature reading `Now()` sees the flip **in
  the middle of a frame**. That is fine for a feature correct at any call (all bound so far are);
  one that must not mix modes within a frame needs a `Latch` taken at its own boundary (no bound
  feature uses one yet: the class is tested, not yet run in a game). Either way the frames in
  flight from the other phase fall in the discard: keep `K` at least the depth of the pipeline
  the feature sits in (60 presents is ~1.5-2 s at 30-40 FPS).
- Log the binding once with `Common::Nx2Ab::LogBinding("EDEN_NX2_FOO", foo)` where it is
  parsed: "EDEN_NX2_FOO=ab: follows the A/B phase (0 in A, 1 in B)", and a warning if
  `EDEN_NX2_AB_TOGGLE` is off (the phase never flips: `=ab` alone, with START=A, is the feature
  off for the whole run).
- **Cost off.** A switch not set to `ab` costs one more load and branch per call than a plain
  `static const bool` (`Now()` looks at `follows`); with `ab`, a guarded static (`GetConfig`) and
  a relaxed atomic load as well, in both phases alike. So A under `=ab` is the old way plus that
  check, not byte for byte the `=0` build path; the difference between A and B is the feature.

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
- **Primary estimate: neighbours.** Compare each B block with the mean of its two A neighbours
  (an ABA triple): that removes a linear drift within the run. Two consecutive differences share
  an A block (correlation 1/6 for independent blocks), so widen the t-based 90 % interval by
  √(1 + 2ρ(n−1)/n) ≈ 1.15. Adjacent pairs (2k, 2k+1) are a secondary estimate: the pair's first
  phase is always the same, so the warm-up's rise biases them by half a block's drift.
- **Window after the warm-up:** blocks starting at album + 240 s. In the first A/A runs the
  blocks from album + 60 to 240 s were ~7 % below the later mean (and rising); from 240 s on the
  slope is near 0. With album + 90 the two launches' pair means differed by 1.98 points; from 240,
  by 1.18.
- Always two launches, `AB_START=A` and `AB_START=B`, and the result over both.

## Bound so far

- `EDEN_NX2_TICK_SKIP=ab` (`video_core/gpu.cpp`, `TickWork`): A looks at the GPU tick queue under
  its lock after every draw (the old way, `=0`), B skips it when the queue is empty (the default).
  Both are correct at any tick; each call takes the phase itself.
- `EDEN_NX2_FAST_PRIORITY_SCAN=ab` (`core/hle/kernel/k_priority_queue.h`, `FindSet`): A scans
  the priority bitset bit by bit (`=0`), B with one count-trailing-zeros (the default). The same
  bit either way (`=2` checks that), so any scan may take either phase.
- `EDEN_NX2_BATCH_DIRECT=ab` (`video_core/gpu_split.cpp`, the back's Batch entries): A goes
  write by write through the engine's virtual calls (`=0`), B through `CallMethodBatch` (the
  default). The same writes in the same order, nothing carried between entries: each Batch entry
  takes the phase on its own.
- `EDEN_NX2_DIRTY_THUNK_SKIP=ab` (`video_core/dma_pusher.cpp`, front macros on a native Turing
  Maxwell3D): A defers the `current_dirty` thunk to the back (`=0`), B skips it (the default).
  For these macros nobody reads the value (the back's ProcessMacro never sees them), so an entry
  may take either phase; A's thunk still costs its deferral, which is what is measured.
- `EDEN_NX2_B_PAD_NS=ab:<n>` (`video_core/nx2_b_pad.h`, PrepareDraw; taken from anka/b-hiz-2):
  no pad in A, n ns a draw in B. The positive control: a known slowdown the method must see.

## Candidates (Anka branches, as of 2026-11-10)

| Switch | Branch | Bindable | What the flip needs | Effort |
|---|---|---|---|---|
| `EDEN_NX2_B_PAD_NS` (deliberate per-draw delay) | anka/b-hiz-2 | bound (anka/ab-2, `=ab:<ns>`) | calibrate at start whatever the phase; `Pad()` spins only in B | easy; a positive control for the method |
| `EDEN_NX2_UBO_KEEP` | anka/b-hiz-2 | yes, with care | the per-stage "last bind streamed" bits are only kept while on: clear them at off→on (draw boundary, latch) or keep them always; read the phase once per draw (rasterizer and buffer cache both look) | medium; verify with `=2` across flips |
| `EDEN_NX2_READ_TRAPS` | anka/read-trap | no | correctness mechanism (A would be known-wrong); page protections, trap maps and Dynarmic's recompiled reads persist | — |
| `EDEN_NX2_NULL_AUDIO_PACE` | anka/qt-hiz-2 | technically easy, not useful | pacing: it sets how often the game runs, not how fast the emulator is | — |
| `EDEN_NX2_TEX_FLUSH_HELD` | anka/dogru | not useful | correctness fix (A known-wrong), needs the run-fixed `TEX_FLUSH_KEEP_CPU` | medium |
| `EDEN_NX2_FAST_PRIORITY_SCAN` | anka/110-dkb | bound (anka/ab-2) | pure function (same result either way) | easy |
| `EDEN_NX2_BATCH_DIRECT` | anka/110-dkb | bound (anka/ab-2) | dispatch choice, no state | easy |
| `EDEN_NX2_DIRTY_THUNK_SKIP` | anka/110-dkb | bound (anka/ab-2) | skips a thunk nobody reads; front thread | easy |

Not bindable on anka/110-dkb: `JIT_SAME`, `FRONT_MACRO_LEAN` (in JIT-compiled code), `CB_RUNS`
(set up in the Maxwell3D constructor), `KEY_CMP` (pipeline cache), `BACK_STAGES` / `SPLIT`
(threads built at start); `LAYOUT_MEMO`, `UBO_REUSE` / `UBO_STAGE`, `LIGHT_*`, `INDEX_SKIP` keep
memos that would need invalidating at a latch change.

## Results (anka/110-dkb 895bfd0264 + anka/ab-2, -O2, wide scene, Null audio, throughput profile)

`N=600`, `K=60`, blocks from album + 240 s, neighbour estimate (interval ×1.15), A − B in % of A
(negative: B, the switch on, is faster). One launch START=A and one START=B each, ~12 neighbour
differences per launch.

| Switch (A: off, B: on) | START=A | START=B | Both | Verdict |
|---|---|---|---|---|
| none (A/A) | +0.42 ± 2.69 | +1.40 ± 1.54 | **+0.91 ± 1.47** | no bias seen |
| `B_PAD_NS=ab:300` (positive control: 300 ns a draw) | +7.98 ± 1.18 | +6.95 ± 1.51 | **+7.49 ± 0.92** | B slower, as built |
| `TICK_SKIP=ab` | −3.72 ± 2.77 | −2.20 ± 1.33 | **−2.99 ± 1.52** | B faster: the default (on) gains ~3 % |
| `BATCH_DIRECT=ab` | −0.56 ± 1.13 | — | (one launch) | undecided, within ±1.7 % |
| `DIRTY_THUNK_SKIP=ab` | — | (not reached) | — | the path never ran in this scene: no binding line in 690 s |
| `FAST_PRIORITY_SCAN=ab` | — | — | — | not run yet |

- The control is resolved to ±0.9 % with two launches; b-hiz-2's run-to-run measurement of the
  same pad (−13.3 %, one pair of runs) had an uncertainty of about ±10 %.
- `DIRTY_THUNK_SKIP`: its static (and binding line) is set up the first time the pusher reaches
  the split branch for a separate macro parameter entry; in 690 s of the wide scene it never
  did, so A and B run the same code there. The one run (START=B) is A/A in effect: it had one
  12.8 s GPU semaphore wait ("Channel 2 waits on semaphore", block 35), not from the binding.

## First results (anka/110-dkb 18aac47131 + anka/ab-toggle, blocks from album + 90 s)

- A/A (nothing bound), `N=600`, `K=60`, blocks from album+90 s: START=A 17 pairs, A−B −0.43 ±
  2.36 % (90 %); START=B 17 pairs, +1.55 ± 1.28 %. Both launches: **+0.56 ± 1.31 %** (34 pairs),
  pair SD 4.5 %: no bias seen at this resolution. The later block of a pair was ~1 % faster in
  both launches (the warm-up; from album + 240 the gap shrinks): always run both START=A and
  START=B.
- `TICK_SKIP=ab`, one launch (START=A, 18 pairs): A (off) − B (on) −2.2 ± 2.3 %; with one
  launch, order and effect can't be told apart: a preview, not a result.
