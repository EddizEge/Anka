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
