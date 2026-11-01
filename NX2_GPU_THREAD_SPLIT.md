# Splitting the GPU thread

Title 1 (Mario Kart World) is bound by one host thread, the GPU thread: ~100% busy while the
host GPU is 20-40% busy and the game's own CPU thread mostly polls the counter. Per second of
3D scenes (profiled, 2026-10-29): command processing and register writes ~335 ms, macros
~200 ms (1.3 million calls), draw setup ~345 ms (55,000 draws), compute ~70 ms. Speeding up the
pieces (NX2_NOTES rows 123-139) took frames from ~22-38 ms to ~18-26 ms; a locked 60 fps on the
heavy courses needs work on more than one core.

## Split point: method calls

- **Front thread** (the current GPU thread): takes the CPU's command queue, parses GPFIFO
  entries and command lists (DmaPusher), runs macros (compiled, macro_turing_jit.h), and keeps
  what macros read: a mirror of each engine's registers, its macro code and data RAM.
- **Back thread**: executes the resulting stream of method writes in order: Maxwell3D /
  KeplerCompute / Fermi2D / DMA engines' CallMethod and everything behind them (register
  state, dirty flags, constant buffer and inline uploads, draws, dispatches, queries, fences,
  the rasterizer and its caches). Only the back thread touches the rasterizer.
- The stream: an SPSC ring of {engine, method, argument} plus copied data for multi-word
  methods (command lists are often guest memory read in place, which the guest reuses).
  CPU-side commands (flush/invalidate requests, RunInOrder actions, presentation) are
  forwarded into the same ring so they keep their order.

## What the front needs without waiting

- Register values macros read (MME_READ): the front mirrors every register write it sends.
  Back-side changes to registers it must reproduce: constant buffer offset (+4 per
  const_buffer.buffer word), shadow_scratch[0] = 1 (falcon[4]), shadow RAM control (Track /
  Replay change the effective value), macro upload pointers (the front handles load_mme
  itself). Compute: native_registers (0xad is changed temporarily by the inline QMD path).
- Macro code uploads and binds (load_mme.*, compute 0x45-0x48): handled by the front.

## Where the front waits for the back (sync points)

- Macro DMA reads (MME_DMA_READ / _FIFOED, ~450 a second): read guest memory as of their
  position, so the back must have executed everything before them.
- Semaphore acquires that are not satisfied yet (~10,500 a second, cross-channel waits on QMD
  releases): the release is executed by the back, so the front waits until the semaphore
  holds (or parks the channel and runs another one, as the pending-channel code does now).
- Syncpoint waits, WaitForIdle-style methods, and anything else that reads guest memory or
  host state on the front.
- Expected cost: each wait drains the back only up to the dependency, then the back idles
  until the front sends the next method (a few microseconds), so even ~10,000 waits a second
  leave most of the overlap. Ceiling: front ~35-40% of the work, back ~60-65% -> ~1.4-1.6x.

## Stages

1. Instrument: count sync points per second and the front/back share precisely (zones).
2. Ring + back thread executing method writes; the front still does everything else.
   Correctness first: an EDEN_NX2_SPLIT=0 switch runs both on one thread.
3. Move macros to the front with the register mirrors; verify mirrors against the back's
   registers in a debug mode (compare on every MME_READ).
4. Sync points; crash and screenshot tests across courses; then title 4.

## Status (2026-10-29)

Stages 1-3 are in (babd395, `gpu_split.h`), off by default: `EDEN_NX2_SPLIT=1` runs engine methods
on the back, `EDEN_NX2_SPLIT=2` also runs the 3D and compute macros on the front against register
copies. Measured in title 1 (NX2_NOTES rows 142-144): ~44,000 draws a second on one thread, ~47,400
with stage 2, 50,000-66,500 with stage 3. The front waits ~4,000-5,000 times a second (semaphore
acquires, MME DMA reads); the back still spends ~200 ms a second outside its zones (ring dispatch,
method sink). Clean A/B (NX2_NOTES row 145): 1.46x throughput, frames 17% shorter; on by
default since. The back is ~92% busy, ~53% of it draw setup: the next step is there, not the ring.

## Fewer draw setups on the back (2026-11-01, Donkey Kong Bananza)

A sampling profile of the back (far view, before fbedc9a07) put ~80% of it in draw setup, spread
over a dozen steps of a few percent each; no single step left to cache. The terrain's indexed
indirect draws (Maxwell3D::FrontIndirectDraw, one per 5-word record) were ~22% of the back.
NVN draws several records with one macro call each and nothing in between, so the front now
merges a call into the previous one's entry while that entry is unpublished and only its own
resets followed it: the back sets up once and records one vkCmdDrawIndexedIndirect with a draw
count and stride (multiDrawIndirect is mandatory). Records must follow at one stride (20-256
bytes), up to 256 a batch. `EDEN_NX2_INDIRECT_BATCH=0` turns it off; the profile line counts
merged calls. The reset fix's records (vk_rasterizer.cpp) are written in runs of adjacent
records within a page instead of one inline write each (`EDEN_NX2_RESET_RUNS=0`).

In the game the merge never happened (0 merged of ~35,000 a second). With `EDEN_NX2_GPU_PROFILE`
the front's line now says why per call ("indirect not merged: ..."): no previous call, ring
restarted, other entries between (with the most frequent first entry after the held call: p =
pushbuffer method, r = pushbuffer run, m = macro write, ? = another entry kind or engine),
already published, other topology, group full, record not after the previous one, step out of
range, step mismatch (with the most frequent steps and group strides).

More skips of repeated per-draw work, each with its own switch (`=0` turns it off):
`EDEN_NX2_DESC_XLAT` (small reads remember their page's host pointer until the GPU page table
or the device mapping changes), `EDEN_NX2_TEX_TOUCH_SKIP` (one LRU touch per image per frame),
`EDEN_NX2_USAGE_SKIP` / `EDEN_NX2_INDEX_SKIP` (usage tracker page fills, index buffer lookups),
`EDEN_NX2_DESC_REUSE` (a stage drawn with the same views and samplers copies its last image
descriptors; the profile line counts reused stages), `EDEN_NX2_INVAL_HULL`,
`EDEN_NX2_SHADER_BOUNDS`, `EDEN_NX2_QUERY_BOUNDS` (pre-draw invalidations skip lookups and locks
outside what the caches hold). Built; not measured yet.

## Light draws (2026-11-01, Donkey Kong Bananza)

Most of the ~34,000 indirect draws a second change only vertex stream addresses between them
(the merge profile's first entry after a call: `p0x702`, stream 0's address). A draw then needs
none of the pipeline, descriptor, uniform, render target and dynamic state setup the last one
recorded: the command buffer still holds it. `EDEN_NX2_LIGHT_DRAW` (on; `=0` off) sets up such a
draw by updating and binding its vertex, index and indirect buffers only, then records the draw
(RasterizerVulkan::TryLightDraw). It applies when all of these hold, else the full setup runs:
- the previous draw was set up (in full, or light after one) with the same draw kind (direct or
  indirect, indexed or not), topology and index format; one instance, no inline indices, no
  quads, no 8-bit indices, no count buffer or byte count; its pipeline writes no memory; no
  transform feedback, pixel counting or host conditional rendering; its render targets have no
  aliased images and no feedback loop barrier;
- only vertex stream addresses/limits, index buffer start/limit, draw parameters and the
  constant buffer selection were written since (Maxwell3D::LightChange; HLE macros on the back,
  texture pools, binds and any other register count as state);
- nothing was recorded since (Scheduler::RecordCount: barriers, uploads, render pass changes,
  submissions), nothing invalidated, flushed, unmapped, fenced, cleared, dispatched, copied or
  ticked a frame (light_epoch), no inline data, DMA or semaphore, and each constant buffer write
  stored the bytes memory already held, over pages no cache holds GPU-written data for (NVN's
  resets of c[0][0x640]);
- binding the buffers deleted, uploaded nothing and left the render pass active.
With `EDEN_NX2_GPU_PROFILE` the back's line adds "draw changes direct/indirect" (what changed
since the previous draw, widest first) and "light draws N (M indirect); full: ..." (why the others
were set up in full, with the most frequent first blocking register); the front's line adds
"methods between indirect draws" (the 16 most frequent of the first 16 methods after each
indirect draw: p/r/m as above, c = macro call). Built; not run or measured yet.
