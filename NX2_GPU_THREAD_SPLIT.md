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
  transform feedback; its render targets need no upload, sparse binding, copy from a newer
  alias or feedback loop barrier (queries and conditional rendering need only the per-draw
  calls the light setup makes too);
- only vertex stream addresses, limits and sizes (the native class's, at 0x600), index buffer
  start/limit, draw parameters, topology_override (its effect is the topology, compared) and
  the constant buffer selection changed value since (Maxwell3D::LightChange; a state register
  written with the value it holds counts as unchanged; HLE macros on the back, texture pools,
  binds and any other register count as state);
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
indirect draw: p/r/m as above, c = macro call).

First run (45a27e1f1, open area): 0 light draws. Every draw was "not after one", and every
indirect draw that moved a vertex stream was "+ state": the native class sets a stream's size
at 0x600 + 8 * stream (Regs::NativeVertexStreamSize) along with its address, and those words
were state. And no full setup could be followed; which condition refused them was not counted.
Two of them were stricter than the argument needs and now go: pixel counting and host
conditional rendering (the light setup makes the per-draw query calls the full one makes) and
any alias of a render target (only an alias written later gets copied). The back's line now
also counts why a full setup could not be followed ("not followable: shape, memory writes,
transform feedback, render targets, cut short") and names the registers that first kept
draws off the light path, armed or not ("first blocking register"). Measured (a14f540ce, same
area): 29.7 FPS with light draws, 23.0 without, no visible difference; 100,563 of 187,178 draws a
second went light (59,841 indirect).

### Light draws with uniforms (EDEN_NX2_LIGHT_UNIFORM)

That run's direct draws mostly changed constant buffer data too ("+ uniforms" 68,693 a second),
and 61,883 a second were "not after one" because the draw before them wrote memory. Neither needs
the pipeline, image, render target or dynamic state setup again. `EDEN_NX2_LIGHT_UNIFORM` (on with
light draws; `=0` off, `=2` verify) sets up such a draw's buffers and descriptors only
(GraphicsPipeline::ConfigureUniforms): uniform buffers resolved and streamed or uploaded as the
full setup does, storage and texture buffers kept as last bound and synchronized, written ranges
and storage images marked, each stage's image descriptors as it last pushed them
(`EDEN_NX2_DESC_REUSE`, required), the descriptor set, and the draw. It applies where a plain light
draw does not, when besides the conditions above:
- the changes since the previous draw are also constant buffer data and binds; its pipeline may
  write memory (a full setup ending with one gives "memory writes (uniform way only)"); the images
  it samples or stores to need no upload or alias copy (also checked for plain light draws now);
- every cached write since was constant buffer data (at most 8 merged ranges, none in a lazily
  backed sparse region), none of it reached an image (TextureCache::WriteHits) or a shader
  (ShaderCache::InvalidationHits, also counting removals), none overlaps the texture handles or
  storage buffer descriptors the pipeline reads (at the bound constant buffers' addresses) or the
  texture header and sampler pools, and no constant buffer it reads them from was bound elsewhere;
- the image descriptors kept are still valid (no image view freed, no rescale change, no new
  sampler variant), nothing was uploaded and the render pass is still active.
`=2` decides as above, sets up the buffers the uniform way, then sets the draw up in full and
compares: the same pipeline, not bound again; the same descriptors (uniform buffer slices match
with `EDEN_NX2_UBO_REUSE` on, the default); the same push constants; no render pass begun; no
dynamic state recorded. Differences go to the log ("Light uniform draw differs from its full
setup", first 64). The back's profile line adds "light uniform draws N (M indirect); full: memory,
caches, binds, descriptors, fell back; verified V (W mismatches)" and the registers that first
kept draws off this way.

First run (75628f3c0, wide area): 3,000-4,100 light uniform draws a second; 35,000-54,000 a second
were refused as "descriptors" (about 92% of those that got that far): the constant buffer data
written overlapped what the pipeline reads descriptors from. NVN rewrites the block holding the
bound textures' handles (and storage buffer descriptors) with the data around it, mostly with the
values already there. `=2` found no difference but uniform buffer slices: the same buffer and
size 0x1C00 bytes further. Most likely the full setup after the light one streamed the range
again: the reuse slots of `EDEN_NX2_UBO_REUSE` are direct-mapped and hold one size per guest
range, and a range bound to two stages streams each stage's used size (or two ranges share a
slot).

Now the uniform way reads such words again instead of refusing: ConfigureImpl keeps the words it
reads descriptors from (descriptor_words / descriptor_values: per stage 4 words per storage buffer
descriptor, the address and size BufferCache::StorageBufferBinding reads, then every texture handle
word, a secondary one after its primary) and the texture header and sampler indices they gave.
ConfigureImpl's descriptor setup is a function of those values, the registers, the pools and the
images (read_handle -> TexturePair -> FillImageViews / GetSamplerId; the storage binding from the
16 bytes), so a word written over, or in a slot bound elsewhere, that holds what the last setup
read changes nothing (GraphicsPipeline::CheckDescriptorWords). A written pool refuses only where
the range meets an entry the draw uses (32 bytes each). The refusals are split: binds (a changed
word in a rebound slot), handles, storage descriptors, TIC, TSC; "rewritten with the same value"
counts the draws the comparison let through. `EDEN_NX2_LIGHT_UNIFORM_VALUES=0` refuses on any
overlap as before. `=2` compares a uniform buffer the full setup streamed elsewhere by its bytes
(StagingBufferPool::MappedBytes) and counts "uniform slices moved, same bytes". Measured
(75628f3c0, before this change): 30.25 / 29.92 FPS, as without it (the uniform way covered ~8% of
the candidates); `=2` showed only the moved slices. This change: built; not measured yet.

## Fewer writes and ring entries on the back (2026-11-01)

`EDEN_NX2_SAME_SKIP` (on; `=0` off): a constant buffer write IsLightSameWrite counts (the bytes
guest memory holds, outside sparse regions, on pages where no cache holds GPU-written buffer data
or images and no query result waits: RasterizerVulkan::HoldsGpuWrittenData) is not made. Memory
keeps the same bytes; its invalidation at the next FlushCaching only made the caches upload,
compare or rebuild from them (a query result in the range would have been cancelled: such pages
are excluded). It counts as neither a cached nor a same write (the light draw's count is
unchanged) nor a constant buffer write of the uniform way (NoteLightWrite: it changed no word the
uniform way checks). Mostly NVN's c[0][0x640] base vertex/instance resets; it applies only while
the last draw may be followed by a plain light draw (light.armed). The back's line adds
"constant buffer writes of held bytes skipped".

`EDEN_NX2_WRITE_BATCH` (on; `=0` off): the front holds consecutive macro writes to one engine
(at most 64) and sends them as one Batch entry before anything else enters the ring; every other
entry, Publish, Drain and WritePosition send what is held first. The back runs each as a Direct
Method entry would, in order (the sink consumed once: only non-Direct entries add to it; runs of
constant buffer data through CallMethodRun). The profile's "method" zone now counts batches.

Profile cost: zones read the time stamp counter without fences, converted to nanoseconds per
window by the wall clock (`EDEN_NX2_PROFILE_TSC=0`: the wall clock per zone, as before); the
indirect draw diagnostics (Nx2IndirectDiag, ~1-1.5 us per draw) run only with
`EDEN_NX2_INDIRECT_DIAG=1`. Built; not measured yet.

## Review fixes for the above (2026-11-01)

- Uniform way, samplers by header index (ViaHeaderBinding): the quick test of written sampler
  pool ranges takes the header pool's limit, as TextureCache::SynchronizeDescriptors does (it
  took tex_sampler.limit).
- Same-write skip. The skipped invalidation also used to refresh what caches held on pages
  written around them, and covered work between the write and the next FlushCaching. Writes
  around the caches now mark their device pages (Nx2MarkUntrackedWrite: MemoryManager::Write
  (semaphores), WriteBlockUnsafe unless the caller tells every cache (inline-to-memory), query
  results (CounterReport), buffer cache downloads and DMA copies, in-place texture swizzles).
  IsLightSameWrite counts no write on a marked page (made, no plain light draw after it). A
  skipped write (one 4 KiB page, at most 16 between flushes) is kept by the memory manager; at
  the next FlushCaching, if the rasterizer's WorkEpoch (RasterizerVulkan::light_epoch) or the
  FlushCaching count of every address space (g_nx2_flushes) moved since the first skip, all kept
  writes are invalidated as if made, otherwise those on pages marked since. Re-added ones are
  cached writes but no same writes: the light path refuses the next draw. The line adds
  "invalidated later", "made" (across pages / list full) and "not counted (pages written around
  the caches)".
- Write batches: with batching on, publishing counts words as the ring without batching would
  hold them (a word per macro write, its skips at the ring's end, no batch headers): held writes
  are published where their own entries were. Front reads of the back's results that do not
  drain (Nx2IsGpuWritten behind DmaPusher's indirect record take, macro parameter flush and
  command list flush) see them no later than before. Built; not measured yet.

## Uniform reuse by stage slot, batched reset uploads (2026-11-01)

`EDEN_NX2_UBO_STAGE` (on; `=0` off; with `EDEN_NX2_UBO_REUSE`): a streamed uniform bind first
tries the slice its graphics stage's slot was bound to last, whatever range it was streamed for,
then the range's own; either serves when streamed in this scheduler tick, unchanged since the
slot noted it (serial) and its record starts with the guest's `size` bytes (a longer slice serves
a shorter bind). Catches constants written to a new address with the same values each draw,
slices the 128-slot direct-mapped table lost, one range bound with two sizes. The light uniform
way binds through the same calls; `=2` compares moved slices by their bytes. The reuse log line
adds "(N only through EDEN_NX2_UBO_STAGE)"; the profile's back line "uniform binds reused through
the stage slot only".

`EDEN_NX2_RESET_BATCH` (on; `=0` off; not with accurate GPU fences): the indirect draw reset
(RESETFIX, ~1,400-2,000 records a frame, each its own inline write) goes through
AccelerateInlineRuns: every run takes AccelerateInlineToMemory's steps in the same order, but the
buffer cache leaves its uploads into GPU-written buffer data in a batch
(BufferCache::InlineMemoryBatched) and records consecutive ones into one buffer as one copy from
one staging buffer between two barriers. The batch is recorded before AccelerateInlineRuns
returns (also on unwinding), before FindBuffer may make or join a buffer, when the next copy goes
to another buffer, does not start after the previous one's end or would pass 256 KiB; what runs
meanwhile only writes guest memory, keeps the caches' books and uploads guest memory over the same
bytes, with the buffer cache locked. Log: "Indirect draw reset uploads batched: N uploads in M copy
commands over K resets" (first reset, then every 1024); profile: "indirect draw reset uploads N in
M copy commands". Built; not measured yet.

Review fixes (2026-11-01): a reset batch holds copies of one CanReorderUpload answer only (taken
per copy when batched; recorded in order, a copy that would have gone to the upload command buffer
could land after a later upload there over its bytes); with `EDEN_NX2_INLINE_DEFER=0` the batch is
recorded before a run's immediate CPU-modified mark. Log "Indirect draw reset list ...: records
share bytes (N lists with records overlapping at another offset, M naming one record twice, of K)"
at each power of two: absent, those overlaps do not occur. Built; not measured yet.

## Light pipeline candidates, counted only (2026-11-01)

After the uniform way, the draws set up in full for changed fixed function state alone (13,359 a
second in the wide area, a14f540ce: "+ state") are what a light draw that sets up its pipeline and
dynamic state again (EDEN_NX2_LIGHT_PIPELINE, designed, not built) could take. Whether that pays
depends on how many of them keep their programs, render targets and descriptor words, which the
profile did not tell. With `EDEN_NX2_GPU_PROFILE` the back's line now adds "light pipeline
candidates (counted only) N: programs changed, render targets, descriptors; same pipeline (without
uniforms), same programs (first register: ...)" (RasterizerVulkan::NoteLightPipeline, reads only):
of the full setups after a draw the uniform way may follow, with nothing between but constant
buffer data and fixed function state, the first reason that applies (the four reasons and the two
"same" counts add up to N). "Same pipeline" draws changed dynamic state only; "same programs"
ones another pipeline key (with dynamic vertex input off, vertex attribute formats are part of
it). Decision rule (rapor_boru): 3.0·E + 1.4·(D−E) + 1.15·F ≥ ~10 ms/s with D same pipeline, E
without uniforms, F same programs (per second). Built; not measured yet.

## Light draws that bind storage buffers again (EDEN_NX2_LIGHT_STORAGE, 2026-11-01)

Wide area, evening profile (30b853451): of the draws the uniform way looked at, ~16,600 a second
went light and ~81,700 were refused as "storage descriptors": constant buffer data written since
the previous draw changed the address or size words of a storage buffer descriptor the pipeline
reads (NVN keeps them in the driver constant buffer, c0 bytes 0x100-0x6FF, 16 each: the
recompiler's NVN bias; most likely it binds a storage buffer per draw by rewriting one, the
indirect draw reset's list at c0[0x110] being one). Such a draw keeps its pipeline (no state
register changed: the same programs, descriptor layout, storage buffer count, stages, slots and
written flags) and, where they hold their values, its texture handles; only its storage bindings
differ. A full setup makes those from the 16 bytes at each descriptor
(BufferCache::BindGraphicsStorageBuffer -> StorageBufferBinding:
the words, the page table and kind map, the device's alignment), then UpdateGraphicsBuffers finds
their buffers and multi-range segments and BindHostStageBuffers synchronizes them, marks their
usage and written ranges (GPU-modified, Nx2MarkGpuWritten) and pushes their descriptors; the
uniform way already makes those two calls for every draw.

`EDEN_NX2_LIGHT_STORAGE` (on with the uniform way; `=0` refuses as before; `=2` sets the draws
that bound storage buffers again up in full after all and compares, as `EDEN_NX2_LIGHT_UNIFORM=2`
does for every light draw with uniforms): where the only descriptor words that changed are
storage buffer ones (handles written over must still hold their values), ConfigureUniforms first
does config_stage's storage part for every stage the pipeline configures: unbind, then bind each
descriptor from its words at the bound constant buffer's address now (a rebound slot is no
different), in order (the dynamic storage buffer limit counts the same), keeping the words read in
descriptor_values, which later draws compare with (the values the bound descriptors were made of).
The rest is as before; handles, pool entries, uploads (a new range over CPU-written pages) and an
ended render pass still refuse or fall back. CheckDescriptorWords scans past changed storage words
(with the switch or the profile on), so the refusals count "handles" whenever a handle changed.

`=2` (either switch) also compares the descriptor words the light way left with those the full
setup read ("descriptor word N: light X full Y"). The back's profile line adds "storage
descriptors changed in N draws (B buffers; A addresses, W written, H with handles too); bound
again R, fell back F (first: ...)", counted whether this way took them or not (also with `=0`);
first: stage << 24 | constant buffer slot << 16 | offset of the first descriptor that changed.
With `=2` the draws that bound them again are verified instead of drawn: "bound again" stays 0 and
"verified" counts them. `EDEN_NX2_LIGHT_UNIFORM_VALUES=0` treats every storage descriptor word
written over as changed, so with this switch on those draws bind again rather than refuse.
Built; not measured yet.

## A core of its own for the back (2026-11-01)

The back is the thread a frame waits on (~98% busy). On Windows nothing places it
(`Common::SetCurrentThreadToPerformanceCores` acts on Android only), so the other logical
processor of its core may run another busy thread of the process meanwhile (the front spinning
in Drain, HostTiming, a guest CPU core), and the two share that core. `EDEN_NX2_BACK_CPU=1` (off
by default until an A/B shows a gain; unset or `=0`: nothing is called): the back runs on one
physical core (CPU Sets, both of its logical processors) and the process's other threads on the
other cores (the process's default CPU Sets, set back when the split stops; each new back thread
pins itself again); `EDEN_NX2_BACK_CORE=<n>` picks the core by Windows' CoreIndex, as the log line
names it (default: the second core of group 0). `=2` pins and logs where the back and the front
ran, every ~32,768 samples (a sample per back wake and per 4,096 ring words: "GPU thread split
placement (N samples, back pinned): back on logical processors ...; front last waiting on ...;
back on the front's core X% of samples"; pinned, X should be 0); `=3` only logs (where Windows
puts them unpinned). The switch is read at first use. Built; not measured yet
(src/video_core/nx2_back_cpu.h).

## Smaller trims on the back (2026-11-01)

Each has its own switch (default on, `=0` as before; NX2_NOTES.md) so a profile run per switch
shows what it took: `EDEN_NX2_MARK_WORDS` (GPU-written page bits 64 at a time: written storage
buffers are marked whole on every draw that binds them, up to 2,048 pages; profile "bind stage
buffers" and "light draw"; `=2` compares with page by page), `EDEN_NX2_BATCH_DIRECT` (Batch
entries through Maxwell3D::CallMethodBatch, CallMethod inlined; "method"), `EDEN_NX2_TICK_SKIP`
(TickWork without its lock when no sync request is pending; "draw"), `EDEN_NX2_LAYOUT_MEMO`
(indirect draws' index buffer layout size kept until MappingEpoch changes; "other"),
`EDEN_NX2_RESET_LAST` (last reset program answer kept; "draw", below a millisecond a second). The
per-write and per-draw switch functions read their variable into an atomic at first use instead
of a guarded static checked on each call (no switch; "method", "draw", "bind stage buffers").
Built; not measured yet.

## Back stages (EDEN_NX2_BACK_STAGES, 2026-11-02; cut 1 built, not measured yet)

The back above is the thread a frame waits on. Back stages split it again, in a pipeline
behind the front: **A** ("GPUBack", the back thread itself) runs ring 1's engine methods against
the primary registers; the stages below get what A did as records over rings of their own and
run the rasterizer and its caches on replicas of the registers. Two cuts:

- **Cut 1 (`EDEN_NX2_BACK_CUTS=a`, default): A | BC.** BC ("GPU memory stage", "GPUMem") runs
  every cache and the Vulkan recording, as the unstaged back did. The register work (~15% of
  the back) goes to A. Expected x1.05-1.12 (an estimate; PGO-free build, relative).
- **Cut 2 (`=ac`): A | B | C** with C ("GPU record stage", "GPURec") for the texture cache and
  recording. Only plumbing until the events needing both B and C are classified (T9): Start
  logs "cut c is a plumbing trial only until T9" once. Not for play or measuring.

Switches (read once at the split's start, `Nx2Stages::Get`):

- `EDEN_NX2_BACK_STAGES`: `0`/unset off (default: nothing is made, the back runs as before);
  `1` on; `2` lockstep verification: A waits for each record to run, and after it the replica's
  registers and light changes are compared with the primary's (log line below); `3` pass-through:
  A sends ring 1's ranges (ExecA) and BC runs them with `GpuSplit::Execute` on the primaries, no
  proxy, no replica: only the stage cost (rings, wakes, marks).
- `EDEN_NX2_BACK_STAGES_EVERY=<n>`: `=2` compares every n-th record (Joins included).
- `EDEN_NX2_STAGE_CHECK=1`: owner checks at full speed: the scheduler's user against the record
  owner (`RecordOwner`: BC with cut 1, C with cut 2, A inside a join), cache paths reached from A
  outside a join (`CheckStage`, the memory manager's cache paths); the first 20 reports go to
  stderr with the stack and to the log in short (where, thread id and name, stage, first six
  frames as module+offset). Counted in the profile's " | stage check:" line.
- Threads: BC (B) runs time critical, A and C highest. `EDEN_NX2_BACK_CPU=1`/`2` (above) pins
  BC to the back's core with A, C and the process's other threads off it, and with the stages
  the front gets a core of its own too (`EDEN_NX2_FRONT_CORE=<n>`).

How work is placed (cut 1):

- **A** runs Maxwell3D's methods (`stage_methods_on_a`): register writes are captured
  (`RegCapture`) and ride as delta words at the head of A's next record to BC, which applies them
  to its replica first (dirty flags and light changes as the primary's). Writes outside
  `ProcessDirtyRegisters` (macro HLE, inline data, firmware call 4) and constant buffer data
  (`CbData` records, adjacent writes merged) go the same way; side state the rasterizer reads
  (constant buffer binds, rebound bits, engine state, replace table, draw state, draw texture
  state) goes with the draw that reads it, only the parts that changed.
- **The proxy** (`Nx2StageProxy`) is every staged channel's rasterizer (engines and memory
  manager; DmaPusher keeps the real one). From A outside a join a member is posted to BC as a
  record (draws, clears, uniform binds, fences, queries, invalidations, inline writes with their
  data) or runs on A with the stages parked (a join): members returning a value, FlushRegion /
  FlushAll / FlushAndInvalidateRegion, conditional rendering, channel init / bind / release, and
  a record too long for the ring (K9). From any other thread it calls the real rasterizer.
- **Ring 1's Call entries** (`DeferTarget`): `Owner` (the default) is forwarded to BC as is and
  runs there in order; `OnA` (register and draw manager work: FinishFrontMacro, DeferEditable,
  the front's sink handover) runs on A; `Join` (RunCommand, the memory dirty thunk when the
  macro runs on the back) runs on A in a join.
- **Joins at ring level:** executable methods of the other engines (KeplerCompute, MaxwellDMA,
  Fermi2D, KeplerMemory) run on A with the stages parked, consecutive ones in one join.
  BindChannel: forwarded (Owner) for the same staged channel and address space, a join for a
  session's first bind, a channel change or a new address space (it stages the channel).
- **consumed:** A posts Marks (every 4,096 words and at each batch end); the last stage stores
  `consumed` (D1), so the front's Drain, WaitForSpace and BackCaughtUp work unchanged.
- **Start / Stop:** `GpuSplit::MaybeStart` starts the stages before the back thread; `=1`/`=2`
  install the proxy and stage every initialized channel (later ones at their first bind). Stop
  after the back joined; the proxy outlives it (reused for the same rasterizer).

`=2`'s mismatch line (`Maxwell3D::StageVerify`, the first 16 differences; all counted in the
profile's "verified N (M mismatches)"):
`Back stages (=2): replica <what> <index:#x> holds <replica:#x>, primary <primary:#x>`, `<what>`
one of `register`, `constant buffer bind (stage << 8 | slot)`, `engine_state`, `replace_table
(entries)`, `execute_on`, `draw_state (instance count)`, `indirect_state (address)`,
`draw_texture_state (texture)`, `unsent light.rebound (stage)`, `light changes`. A record out of
order logs `GPU stages: <stage> read record <seen> where <expected> was due` (critical).

Profile lines (`EDEN_NX2_GPU_PROFILE=1`): one per stage ("GPU back thread" = A, "GPU memory
stage" = BC / B, "GPU record stage" = C); " | stages:" joins from A by reason, ring KiB, delta
words, side states; " | stage diagnostics:" (also at `=0`) what decides cut 2: texture / image
buffer and transform feedback draws, inline / DMA / constant buffer writes landing on images or
texture pools, GPU-written data held by query / buffer / texture, tick work, memory dirty
queries.

At `=0` the behaviour is the same; the code added on that path is listed with its switch (one
`stage_capture` test per Maxwell3D hook, an `Active()` test at BackLoop's start and in the owner
check, a `Proxy()` test in BindChannel, thread-local checks on the memory manager's cache paths,
profile-only counters). A gate checks its cost: the `=0` build against perf/hafif's e0488ae90
with the same flags, back microseconds per draw within 1%.

Known limits:

- Cut 2 is plumbing until T9 (two-part events, ring 3's classification); its launchers are
  locked. T3's choice for K3 (holding the buffer cache lock across B's joins vs. its release /
  retake, `Nx2Record::CacheLock`) is open and decided in T9.
- Mid-game settings changes stop and start the stages (Drain, Stop); not tried in a game yet.
- `GPU::Impl::InitChannel` initializes a channel on a guest CPU thread with the real
  rasterizer's caches unlocked: as before the stages, they do not change it.
- `=2` checks registers and light changes on every compared record, side state only after a
  record that carried it; whether a rebound bit was lost is left to a sequential mode (`=4`, not
  built).
- Built and unit-tested (rings, replicas, binds: a shim of Catch2); not run in a game yet.
