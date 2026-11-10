// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <tuple>
#include <utility>
#include <unordered_set>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/cpu_features.h"

#ifdef ARCHITECTURE_x86_64
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

// EDEN_NX2_GPU_PROFILE: time where the GPU thread spends its time. Each zone counts its own
// (exclusive) time, nested zones excluded; every 10 s the thread logs each zone's share.
namespace VideoCommon::Nx2Profile {

enum class Zone : u32 {
    Other, ///< GPU thread time outside every zone below (register writes, command parsing)
    Idle,
    PendingSleep,
    ResumePending,
    Push,
    Tick,
    Run,
    FlushInvalidate,
    Macro,
    Draw,
    Clear,
    Compute,
    FlushWork,
    PipelineLookup,
    Configure,
    SyncDescriptors,
    ReadHandles,
    FillImageViews,
    TextureBuffers,
    UpdateBuffers,
    DescriptorAcquire,
    BindStageBuffers,
    PushDescriptors,
    RenderTargets,
    ConfigureDraw,
    DynamicState,
    DrawRecord,
    TextureUpload,
    Fence,        ///< Fences made (SignalFence)
    FenceSkipped, ///< Empty fences not made
    Method,       ///< Engine method calls (what a split GPU thread's back half would run)
    SplitDrain,   ///< Split GPU thread: the front waits for the back (gpu_split.h)
    SplitWait,    ///< Split GPU thread: the back waits for work
    ParameterFlush, ///< Macro parameters / command lists downloaded from the GPU (full GPU wait)
    LightDraw,      ///< Draws set up the light way (EDEN_NX2_LIGHT_DRAW, vk_rasterizer.cpp)
    // Back stages (EDEN_NX2_BACK_STAGES, nx2_stages.h):
    StageInWait,  ///< A stage waits for records
    StageOutWait, ///< A stage waits for room in the ring to the next one
    JoinWait,     ///< A (or B) waits for the stages below to park
    ParkWait,     ///< A parked stage waits to go on; C waits for B to park (ParkB)
    ReplicaApply, ///< Register deltas and side states applied to a replica
    PacketBuild,  ///< A draw record built (side state, arguments)
    ListReplay,   ///< C replays B's record list (cut c)
    LightRepair,  ///< C repairs a light draw B decided (cut c)
    // Cut c, B's draw path in finer parts (EDEN_NX2_PROFILE_FINE, on with the profile; FineScope):
    LightGeometry, ///< A plain light draw's geometry buffers (UpdateGeometryBuffers, binds)
    LightUniform,  ///< A light draw's uniform buffers (ConfigureUniformsBuffers)
    MarkStreams,   ///< Every vertex stream marked used (MarkVertexStreamsUsed)
    LightDecide,   ///< The light decision on B's replica (LightUniformOkB included)
    StagePresets,  ///< The page summary's presets and the pools in flight (T8: PreSetTargets)
    PacketSend,    ///< A draw packet written to ring 3 (SendStageDraw)
    JoinOp,        ///< A join's op run on B (JoinFromB, JoinFromBLocked)
    CbData,        ///< A ring 2 constant buffer data record on B (its pool check included)
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait", "parameter flush", "light draw",
    "stage in wait", "stage out wait", "join wait", "park wait", "replica apply",
    "packet build", "list replay", "light repair", "light geometry", "light uniform",
    "mark streams", "light decide", "stage presets", "packet send", "join op", "cb data",
};

/// Report's thread names for the back stages (nx2_stages.h): A, B (BC with cut a alone), C.
inline constexpr std::array<const char*, 3> stage_report_names{
    "GPU back thread",
    "GPU memory stage",
    "GPU record stage",
};

// EDEN_NX2_GPU_PROFILE (any value but 0) is read at the first call: after main loads
// nx2_switches.txt into the environment (yuzu main.cpp; read at static initialization, the file
// could not turn it on), and with no function-local static's thread-safe initialization check:
// Enabled() runs several times per method write. Threads racing on the first call compute the
// same value.
inline std::atomic<s8> g_enabled{-1};

YUZU_NO_INLINE inline bool ReadEnabled() {
    const char* value = std::getenv("EDEN_NX2_GPU_PROFILE");
    const bool on = value != nullptr && value[0] != '0';
    g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

inline bool Enabled() {
    const s8 value = g_enabled.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        return ReadEnabled();
    }
    return value != 0;
}

// EDEN_NX2_PROFILE_FINE=0: the finer zones of cut c's draw path off (FineScope; each costs two
// counter reads, ~10-20 ns, a few times a draw). On with the profile otherwise. Read at the first
// call as EDEN_NX2_GPU_PROFILE is (Enabled()): at static initialization the profile's state is
// not known yet (g_enabled is -1, which a bool reads as on) and nx2_switches.txt is not loaded.
inline std::atomic<s8> g_fine{-1};

YUZU_NO_INLINE inline bool ReadFine() {
    const char* value = std::getenv("EDEN_NX2_PROFILE_FINE");
    const bool on = Enabled() && (value == nullptr || value[0] != '0');
    g_fine.store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

inline bool FineEnabled() {
    const s8 value = g_fine.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        return ReadFine();
    }
    return value != 0;
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

// What zones are timed in: the time stamp counter, read without fences or conversion (Now's
// fenced read and multiply cost several times as much, at ~150,000 zones a second and more on
// the split's back). Report turns them into nanoseconds by the wall clock over its window; the
// counter is invariant and shared by the cores where the wall clock reads it. Without it, or with
// EDEN_NX2_PROFILE_TSC=0: Now().
inline const bool g_tsc = [] {
    const char* value = std::getenv("EDEN_NX2_PROFILE_TSC");
    return value == nullptr || value[0] != '0';
}();

inline u64 Ticks() {
#ifdef ARCHITECTURE_x86_64
    if (g_tsc) {
        return __rdtsc();
    }
#endif
    return Now();
}

/// Values noted on the GPU thread, logged per window as the most frequent ones.
enum class Histogram : u32 {
    IndirectInterposer,  ///< First entry after an unmerged indirect draw (see FrontIndirectDraw)
    IndirectStep,        ///< Record address step of indirect draws that did not follow
    IndirectGroupStride, ///< Stride of the group a step did not match
    IndirectGapMethods,  ///< Methods written between two indirect draws (the first 16 of each gap)
    LightBlocking,       ///< First register that kept a draw off the light path (EDEN_NX2_LIGHT_DRAW)
    LightUniformBlocking, ///< First register outside what a light draw with uniforms sets up again
    LightPipelineFirst,   ///< The same, for draws that kept only it from that way (counted only)
    /// First storage buffer descriptor a light draw with uniforms found changed: its stage << 24 |
    /// constant buffer slot << 16 | offset (EDEN_NX2_LIGHT_STORAGE)
    LightStorageFirst,
    Count,
};

struct HistogramData {
    std::array<std::pair<s64, u64>, 32> slots{}; ///< Value and times seen
    u32 used{};
    u64 dropped{}; ///< Notes of values that found no free slot
};

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ticks{}; ///< See Ticks
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, 192> counters{}; ///< See Counter
    std::array<std::array<u64, 3>, 9> drains{}; ///< See DrainSite: calls, waits, wait ticks
    std::array<HistogramData, static_cast<size_t>(Histogram::Count)> histograms{};
    u64 child_ticks{}; ///< Time of the zones nested in the current one
    u64 window_start{};       ///< Now() when the window began
    u64 window_start_ticks{}; ///< Ticks() then
};

inline thread_local ThreadData t_data;

class Scope {
public:
    explicit Scope(Zone zone_) : Scope{zone_, Enabled()} {}

    /// Timed only when `on` (FineScope: FineEnabled()).
    Scope(Zone zone_, bool on) : zone{zone_}, enabled{on} {
        if (!enabled) {
            return;
        }
        parent_child_ticks = t_data.child_ticks;
        t_data.child_ticks = 0;
        start = Ticks();
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        const u64 elapsed = Ticks() - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(zone);
        data.exclusive_ticks[index] += elapsed - data.child_ticks;
        ++data.count[index];
        data.child_ticks = parent_child_ticks + elapsed;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone;
    bool enabled;
    u64 start{};
    u64 parent_child_ticks{};
};

/// A finer zone (EDEN_NX2_PROFILE_FINE): timed with the profile unless the switch is 0.
class FineScope : public Scope {
public:
    explicit FineScope(Zone zone_) : Scope{zone_, FineEnabled()} {}
};

/// Amounts counted on the GPU thread, logged per second with the zones.
enum class Counter : u32 {
    DownloadBytes,  ///< Buffer data copied back from the host GPU at fences
    DownloadCopies, ///< Buffer ranges copied back
    DmaReadBytes,   ///< Guest memory the native MME read with DMA
    DownloadBytesOnDmaPages, ///< Of DownloadBytes: on pages the MME has DMA-read
    MethodWrites,   ///< 3D register writes (Maxwell3D::CallMethod below the macro range)
    SameValueWrites, ///< Of MethodWrites: the register already held the value
    AcquireChecks,  ///< Semaphore acquires evaluated
    AcquireWaits,   ///< Of AcquireChecks: not met yet (the channel waits)
    ParameterBlocks, ///< Small macro/inline parameter blocks starting a GP entry
    ParameterFlushChecks, ///< Of ParameterBlocks: on GPU-written pages (the front drains)
    ParameterFlushes, ///< Of ParameterFlushChecks: dirty in the buffer cache (downloaded)
    ParameterDrainWaits, ///< Of ParameterFlushChecks: the drain had to wait for the back
    CommandListChecks, ///< GP entries on GPU-written pages (FlushGpuWrittenCommands drains)
    CommandListFlushes, ///< Of CommandListChecks: dirty in the buffer cache (downloaded)
    CommandListDrainWaits, ///< Of CommandListChecks: the drain had to wait for the back
    HleIndirectDraws, ///< NVN indexed indirect draws whose record the host GPU reads
    IndirectBarriers, ///< Barriers before indirect draws (buffers written since the last one)
    EmptyDraws,       ///< Direct draws with no indices, vertices or instances, or quads under 4 vertices
                      ///< (after the topology; before 10.11.2026 the quads were not counted)
    HleIndirectMerged, ///< Of HleIndirectDraws: joined the previous call's multi-draw
    DescriptorStages, ///< Graphics stages with image descriptors (EDEN_NX2_DESC_REUSE)
    DescriptorStagesReused, ///< Of DescriptorStages: copied from the stage's last draw
    // Of HleIndirectDraws, why one did not join the previous call (EDEN_NX2_INDIRECT_BATCH):
    IndirectNoPrevious,   ///< No earlier call to join
    IndirectRestarted,    ///< The split ring started over since the previous call
    IndirectInterposed,   ///< Other ring entries came after the previous call
    IndirectPublished,    ///< The previous call's entry was already visible to the back
    IndirectTopology,     ///< Another topology than the previous call
    IndirectGroupFull,    ///< The previous call already holds the most records
    IndirectNotAfter,     ///< The record is not after the previous one
    IndirectStepRange,    ///< Step to a single previous record outside 20-256 bytes or unaligned
    IndirectStepMismatch, ///< Step other than the previous group's stride
    // EDEN_NX2_LIGHT_DRAW (split back): what changed since the previous draw, by draw kind, one of
    // the DrawChange categories (vk_rasterizer.cpp).
    DrawChangeDirect,
    DrawChangeIndirect = DrawChangeDirect + 7,
    LightDraws = DrawChangeIndirect + 7, ///< Draws set up the light way
    LightIndirectDraws,  ///< Of LightDraws: indirect draws
    // Draws set up in full, why (the first reason that applies):
    LightNotArmed,       ///< The previous draw was not one a light draw can follow
    LightStateChanged,   ///< Registers other than vertex/index buffers and draw parameters changed
    LightOtherWork,      ///< Other work in between: commands recorded, caches invalidated, memory written
    LightShape,          ///< Another kind of draw (indexed, indirect, topology, index format)
    LightFellBack,       ///< Buffers deleted, uploaded or the render pass ended on the way
    // Draws set up in full that a light draw may not follow, why (the first reason that applies):
    LightRefusedShape,    ///< Instances, inline indices, quads, 8-bit indices, count buffer, byte count
    LightRefusedWrites,   ///< Its pipeline writes memory: only a light draw with uniforms may follow
    LightRefusedFeedback, ///< Transform feedback on
    LightRefusedTargets,  ///< Render targets or sampled images: feedback loop, newer alias, CPU-modified
    LightRefusedCutShort, ///< The setup ended early (no pipeline, configure failed, reset fix)
    // EDEN_NX2_LIGHT_UNIFORM: draws set up the light way with their uniform buffers and descriptors
    LightUniformDraws,
    LightUniformIndirectDraws, ///< Of LightUniformDraws: indirect draws
    // Draws that could only go the uniform way but were set up in full, why:
    LightUniformMemory,      ///< Cached writes other than constant buffer data, or too many ranges
    LightUniformCaches,      ///< A write reached an image or a shader
    LightUniformBinds,       ///< A descriptor word changed in a slot bound elsewhere
    LightUniformHandles,     ///< A texture handle written with another value
    LightUniformStorage,     ///< A storage buffer descriptor (address, size) written with another value
    LightUniformTic,         ///< A texture header pool entry the draw uses was written
    LightUniformTsc,         ///< A sampler pool entry the draw uses was written
    LightUniformFellBack,    ///< Image descriptors not kept, buffers uploaded or the render pass ended
    // Draws that passed the descriptor checks with words written or rebound, same value:
    LightUniformSameHandles,
    LightUniformSameStorage,
    LightUniformVerified,    ///< EDEN_NX2_LIGHT_UNIFORM=2: decided light, set up in full and compared
    LightUniformMismatches,  ///< Of LightUniformVerified: the full setup differed (logged)
    LightUniformSlicesSame,  ///< Uniform buffers the full setup streamed elsewhere, same bytes
    SameWritesSkipped, ///< Constant buffer writes of held bytes not made (EDEN_NX2_SAME_SKIP)
    SameWritesMade,    ///< Counted ones made all the same: across pages, or too many held
    SameWritesInvalidatedLater, ///< Skipped, then invalidated at FlushCaching (work in between)
    SameWritesUntracked, ///< Held bytes not counted: a page written around the caches
    UniformStageReuses,  ///< Uniform binds EDEN_NX2_UBO_STAGE alone reused
    ResetUploads,      ///< Indirect draw reset uploads batched (EDEN_NX2_RESET_BATCH)
    ResetCopyCommands, ///< The copy commands they were recorded in
    // Draws set up in full that only fixed function state kept off the uniform way, for the
    // EDEN_NX2_LIGHT_PIPELINE design (counted only, RasterizerVulkan::NoteLightPipeline); the
    // first of these that applies:
    LightPipelineCandidates,
    LightPipelinePrograms,    ///< The pipeline found has other programs than the previous one
    LightPipelineTargets,     ///< Render targets marked dirty (their registers written)
    LightPipelineDescriptors, ///< A descriptor word the previous pipeline reads changed
    LightPipelineSame,        ///< The previous pipeline: only dynamic state changed
    LightPipelineSamePlain,   ///< Of LightPipelineSame: no uniform data or binds changed either
    LightPipelineSwitch,      ///< Another pipeline with the same programs
    // EDEN_NX2_LIGHT_STORAGE: draws whose storage buffer descriptors (address and size words) the
    // uniform way found written with other values (whether that way took them or not), and of them:
    LightStorageChanged,
    LightStorageBuffers,     ///< Descriptors that changed, summed (per draw: / LightStorageChanged)
    LightStorageAddress,     ///< An address changed (the others changed size words only)
    LightStorageWritten,     ///< One the pipeline writes through changed
    LightStorageWithHandles, ///< A texture handle changed too (set up in full, as "handles")
    LightStorageDraws,       ///< Of LightUniformDraws: bound their storage buffers again
    LightStorageFellBack,    ///< Bound them again, then fell back (uploads, render pass ended)
    // Vertex streams per draw (BindHostVertexBuffers, once a draw), summed:
    VertexDraws,          ///< Draws whose vertex buffers were bound
    VertexEnabled,        ///< Enabled streams
    VertexBuffers,        ///< Different buffers they bind
    VertexDirtyEnabled,   ///< Enabled streams flagged dirty (bound anew)
    VertexSyncSkipped,    ///< Clean streams of plain light draws not synchronized (LIGHT_SYNC_SKIP)
    VertexSyncWouldSkip,  ///< The same at EDEN_NX2_LIGHT_SYNC_SKIP=2 (synchronized and checked)
    VertexSyncGuarded,    ///< Clean ones synchronized as their range may have changed since
    // UpdateVertexBuffers with Dirty::VertexBuffers set, and the streams it resolved again:
    VertexUpdates,
    VertexResolved,
    VertexResolvedDisabled, ///< Of VertexResolved: bound nothing (disabled, empty or unmapped)
    // EDEN_NX2_SYNC_MEMO: synchronizations of a range the memo was asked about, and of them:
    SyncMemoHits,
    SyncMemoMissEmpty,    ///< The slot held no range
    SyncMemoMissOther,    ///< The slot held another range, apart from this one (a collision)
    SyncMemoMissPartial,  ///< The slot held one overlapping or touching it, not covering it
    SyncMemoMissStamped,  ///< The slot covered it, but a 4 MiB region of it was stamped since
    SyncMemoMissUploaded, ///< Of the misses: the walk uploaded something
    // Back stages (EDEN_NX2_BACK_STAGES, nx2_stages.h), where they happen:
    JoinsA,          ///< Joins from A (JoinFromA); of them by reason, in JoinReason's order:
    JoinsAEngine,
    JoinsAChannel,
    JoinsAValue,
    JoinsAFlush,
    JoinsACondition,
    JoinsAUpload,
    JoinsATagged,
    JoinsAOther,
    JoinCandidates,        ///< Ring 1 entries with an executable method of an engine but Maxwell3D
    JoinCandidatesCompute, ///< Of JoinCandidates: KeplerCompute's
    JoinsB,                ///< Joins from B (cut c)
    Parks,                 ///< B parked for C (ParkB, cut c)
    RingBytesB,            ///< Bytes written to ring 2 (A to B)
    RingBytesC,            ///< Bytes written to ring 3 (B to C)
    DeltaWords,            ///< Register delta words sent
    SideStates,            ///< Side states sent
    PassThrough,           ///< =3: ring 1 words run from A's ranges
    LightRepairsState,     ///< Light draws C repaired: command state (cut c)
    LightRepairsTexture,   ///< ... textures set up again
    GeometryReplays,       ///< The geometry mirror recorded in full again (cut c)
    // EDEN_NX2_STAGE_CHECK and =2:
    HazardPages,      ///< The page summary said no GPU-written data, the caches said some (D6)
    HazardUntracked,  ///< A write around the caches B did not see yet (S12)
    HazardEpoch,      ///< Work C did that B's light epoch did not count yet (S12)
    StageViolations,  ///< CheckStage reports
    StageUnproxied,   ///< A reached the real rasterizer outside a join (S3)
    VerifyChecks,     ///< =2: replica comparisons
    VerifyMismatches, ///< Of VerifyChecks: differed
    // Diagnostics choosing the cut c classes (T4; counted unstaged too, where draws are set up):
    TextureBufferDraws,    ///< Draws whose pipeline has texture buffers
    ImageBufferDraws,      ///< ... image buffers
    TransformFeedbackDraws,
    ResetFixLists,         ///< Indirect draw reset fix lists set up (RESETFIX)
    ResetFixFlushes,       ///< Of ResetFixLists: with a FlushRegion
    InlineWrites,          ///< Inline-to-memory writes (AccelerateInlineToMemory)
    InlineOnImages,        ///< Of InlineWrites: on a page an image holds
    InlineOnPools,         ///< ... on a texture header or sampler pool page
    DmaWrites,             ///< MaxwellDMA copies and fills writing guest memory
    DmaOnImages,
    DmaOnPools,
    CbDataOnImages,        ///< Constant buffer data writes on a page an image holds
    CbDataOnPools,         ///< ... on a texture header or sampler pool page
    HoldsGpuWrittenQuery,  ///< HoldsGpuWrittenData true for: the query cache
    HoldsGpuWrittenBuffer, ///< ... the buffer cache
    HoldsGpuWrittenTexture, ///< ... the texture cache
    LightRecordedSubmit,   ///< Light candidates after recorded commands, made by: a submit
    LightRecordedTickWork, ///< ... TickWork (presents, CPU requests)
    LightRecordedEvent,    ///< ... another event (clear, query, fence, ...)
    TickWorkRequests,      ///< Requests TickWork ran
    TickWorkFlushes,       ///< Of them: CPU flush requests
    TickWorkComposites,    ///< ... presents
    ComputeDispatches,
    Clears,
    FencesSignaled,
    MemoryDirtyQueries,    ///< MemoryManager::IsMemoryDirty calls
    MemoryDirtyThunks,     ///< Of them: DmaPusher's deferred ones
    MemoryDirtyThunksSkipped, ///< DmaPusher's skipped ones: a macro the front runs
                              ///< (EDEN_NX2_DIRTY_THUNK_SKIP)
    ConditionalRenderings, ///< AccelerateConditionalRendering calls (S1)
    // Cut c, what B's and C's draw paths took the short way for (stage costs):
    RepairsNotFollowable, ///< Of LightRepairsState: C's anchor could not be followed (C's part)
    RepairsPipeline,      ///< ... C's anchor had another pipeline (or none)
    RepairsRecords,       ///< ... commands recorded since C's anchor (a submit, other work)
    PresetsKept,          ///< PreSetTargets of a light draw: the targets' ranges reused
    StreamMarks,          ///< Vertex streams MarkVertexStreamsUsed saw
    StreamMarksStamped,   ///< Of them: tracked before, only stamped (the range and its buffer's
                          ///< usage generation unchanged)
    Count,
};

/// Split GPU thread drains by caller (EDEN_NX2_GPU_PROFILE; counted where they are called, see
/// CountDrain): calls, calls that waited for the back, and the time they waited.
enum class DrainSite : u32 {
    PullerResume,      ///< Puller::CanResumeAcquire (a pending semaphore acquire checked again)
    PullerAcquire,     ///< Semaphore acquires (semaphore trigger method, method 0x1A)
    PullerExecute,     ///< SEM_EXECUTE acquires (method 0x1B)
    FrontDmaRead,      ///< Maxwell3D::FrontDmaRead (native MME DMA reads on the front)
    Front3DOther,      ///< Maxwell3D::Front (first use) and FrontVerify
    PusherParameters,  ///< DmaPusher: macro / inline parameter blocks on GPU-written pages
    PusherSync,        ///< DmaPusher: command lists with the sync bit (sync_memory_operations)
    PusherCommandList, ///< DmaPusher: command lists on GPU-written pages
    FrontCompute,      ///< KeplerCompute::Front (first use)
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(DrainSite::Count)> drain_site_names{
    "acquire resume", "semaphore acquire", "sem execute", "MME DMA read",
    "3D front other", "parameter blocks",  "sync lists",  "written lists",
    "compute front",
};
static_assert(static_cast<size_t>(DrainSite::Count) <=
              std::tuple_size_v<decltype(ThreadData::drains)>);

/// Categories of DrawChangeDirect / DrawChangeIndirect, the widest change that applies.
enum class DrawChange : u32 {
    Nothing,       ///< Draw parameters at most
    VertexStreams, ///< Vertex stream addresses and limits
    IndexBuffer,   ///< + index buffer start and limit
    Uniforms,      ///< + constant buffer data or binds
    Textures,      ///< + texture and sampler pools
    State,         ///< + any other register (pipeline, fixed function state)
    OtherWork,     ///< Memory written in between (inline, DMA, semaphores), other commands or engines
    Count,
};
static_assert(static_cast<size_t>(Counter::Count) <= std::tuple_size_v<decltype(ThreadData::counters)>);

/// Device pages (4 KiB) the native MME has DMA-read since start (GPU thread only; filled only
/// while profiling): which buffer downloads anything on the GPU thread reads back.
inline std::unordered_set<u64>& DmaReadPages() {
    static std::unordered_set<u64> pages;
    return pages;
}

inline void Add(Counter counter, u64 amount) {
    if (Enabled()) {
        t_data.counters[static_cast<size_t>(counter)] += amount;
    }
}

/// Runs split.Drain() (true when it waited for the back) and, while profiling, counts it for
/// its caller. For callers outside gpu_split.cpp (its own drains are the split drain zone).
template <typename Split>
inline bool CountDrain(Split& split, DrainSite site) {
    if (!Enabled()) {
        return split.Drain();
    }
    const u64 start = Ticks();
    const bool waited = split.Drain();
    auto& entry = t_data.drains[static_cast<size_t>(site)];
    ++entry[0];
    if (waited) {
        ++entry[1];
        entry[2] += Ticks() - start;
    }
    return waited;
}

inline void Note(Histogram histogram, s64 value) {
    if (!Enabled()) {
        return;
    }
    auto& data = t_data.histograms[static_cast<size_t>(histogram)];
    for (u32 i = 0; i < data.used; ++i) {
        if (data.slots[i].first == value) {
            ++data.slots[i].second;
            return;
        }
    }
    if (data.used < data.slots.size()) {
        data.slots[data.used++] = {value, 1};
    } else {
        ++data.dropped;
    }
}

/// The `shown` most frequent values of a histogram, "value x times, ...".
template <typename Format>
std::string TopValues(const HistogramData& data, Format&& format, u32 shown = 3) {
    auto slots = data.slots;
    const auto end = slots.begin() + data.used;
    std::sort(slots.begin(), end, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (auto it = slots.begin(); it != end && it != slots.begin() + shown; ++it) {
        text += fmt::format("{}{} x{}", text.empty() ? "" : ", ", format(it->first), it->second);
    }
    if (data.used > shown || data.dropped != 0) {
        text += ", ...";
    }
    return text.empty() ? "-" : text;
}

/// Called by the GPU thread (and the split's back thread) between commands: logs and resets
/// the totals every 10 s.
inline void Report(const char* thread_name = "GPU thread") {
    if (!Enabled()) {
        return;
    }
    auto& data = t_data;
    const u64 now = Now();
    const u64 now_ticks = Ticks();
    if (data.window_start == 0) {
        data.window_start = now;
        data.window_start_ticks = now_ticks;
        data.child_ticks = 0;
        return;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    // The zones' ticks in nanoseconds, by the window measured both ways.
    const u64 window_ticks = now_ticks - data.window_start_ticks;
    const double ns_per_tick =
        window_ticks != 0 ? static_cast<double>(window) / static_cast<double>(window_ticks) : 0.0;
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    for (size_t i = 1; i < exclusive_ns.size(); ++i) {
        exclusive_ns[i] =
            static_cast<u64>(static_cast<double>(data.exclusive_ticks[i]) * ns_per_tick);
    }
    // Time in no zone at all is the wall time minus the zones' totals.
    u64 zoned{};
    for (size_t i = 1; i < exclusive_ns.size(); ++i) {
        zoned += exclusive_ns[i];
    }
    exclusive_ns[0] = window > zoned ? window - zoned : 0;
    std::string line;
    for (size_t i = 0; i < exclusive_ns.size(); ++i) {
        if (exclusive_ns[i] == 0 && data.count[i] == 0) {
            continue;
        }
        // Per second of wall time: milliseconds spent and calls made.
        const double ms = static_cast<double>(exclusive_ns[i]) * 1000.0 / window;
        const double calls = static_cast<double>(data.count[i]) * 1e9 / window;
        line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", zone_names[i], ms, calls);
    }
    // Where draws are configured (the split's back): the thread's busy time (wall time but
    // waiting for work) by its draws, steadier than frame times across views.
    if (const u64 draws = data.count[static_cast<size_t>(Zone::Draw)]; draws != 0) {
        const u64 waiting = exclusive_ns[static_cast<size_t>(Zone::SplitWait)] +
                            exclusive_ns[static_cast<size_t>(Zone::Idle)] +
                            exclusive_ns[static_cast<size_t>(Zone::PendingSleep)] +
                            exclusive_ns[static_cast<size_t>(Zone::StageInWait)] +
                            exclusive_ns[static_cast<size_t>(Zone::StageOutWait)] +
                            exclusive_ns[static_cast<size_t>(Zone::JoinWait)] +
                            exclusive_ns[static_cast<size_t>(Zone::ParkWait)];
        const u64 busy = window > waiting ? window - waiting : 0;
        line += fmt::format(" | busy {:.1f} ms over {:.0f} draws: {:.3f} us/draw",
                            static_cast<double>(busy) * 1000.0 / window,
                            static_cast<double>(draws) * 1e9 / window,
                            static_cast<double>(busy) / 1000.0 / static_cast<double>(draws));
    }
    line += fmt::format(" | buffer downloads {:.0f} KiB in {:.0f} copies ({:.0f} KiB on pages "
                        "MME DMA reads) | MME DMA reads {:.0f} KiB",
                        static_cast<double>(data.counters[0]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[1]) * 1e9 / window,
                        static_cast<double>(data.counters[3]) * 1e9 / 1024.0 / window,
                        static_cast<double>(data.counters[2]) * 1e9 / 1024.0 / window);
    line += fmt::format(" | 3D register writes {:.0f} ({:.0f} same value)",
                        static_cast<double>(data.counters[4]) * 1e9 / window,
                        static_cast<double>(data.counters[5]) * 1e9 / window);
    line += fmt::format(" | acquires {:.0f} ({:.0f} waited)",
                        static_cast<double>(data.counters[6]) * 1e9 / window,
                        static_cast<double>(data.counters[7]) * 1e9 / window);
    const auto rate = [&](size_t i) { return static_cast<double>(data.counters[i]) * 1e9 / window; };
    line += fmt::format(" | parameter blocks {:.0f} ({:.0f} on written pages, {:.0f} waited, {:.0f} flushed)",
                        rate(8), rate(9), rate(11), rate(10));
    line += fmt::format(" | memory dirty thunks {:.0f} ({:.0f} skipped)",
                        rate(static_cast<size_t>(Counter::MemoryDirtyThunks)),
                        rate(static_cast<size_t>(Counter::MemoryDirtyThunksSkipped)));
    line += fmt::format(" | written command lists {:.0f} ({:.0f} waited, {:.0f} flushed)", rate(12),
                        rate(14), rate(13));
    line += fmt::format(" | indirect draws from GPU records {:.0f} ({:.0f} merged, {:.0f} barriers) | empty draws {:.0f}",
                        rate(15), rate(18), rate(16), rate(17));
    const auto& histograms = data.histograms;
    const auto interposer = [](s64 key) {
        // FrontIndirectDraw's key: kind << 16 | method (p: pushbuffer method, r: pushbuffer
        // run, m: macro write, c: macro call, ?: an entry of another kind or engine).
        constexpr std::array<const char*, 8> kinds{"?", "p", "r", "m", "c", "?", "?", "?"};
        return fmt::format("{}{:#x}", kinds[static_cast<size_t>(key >> 16) & 7], key & 0xffff);
    };
    const auto number = [](s64 value) { return fmt::format("{}", value); };
    line += fmt::format(" | indirect not merged: {:.0f} no previous, {:.0f} ring restarted, "
                        "{:.0f} entries between (first: {}), {:.0f} published, {:.0f} topology, "
                        "{:.0f} group full, {:.0f} not after, {:.0f} step out of range, "
                        "{:.0f} step mismatch (steps: {}; group strides: {})",
                        rate(21), rate(22), rate(23),
                        TopValues(histograms[static_cast<size_t>(Histogram::IndirectInterposer)], interposer),
                        rate(24), rate(25), rate(26), rate(27), rate(28), rate(29),
                        TopValues(histograms[static_cast<size_t>(Histogram::IndirectStep)], number),
                        TopValues(histograms[static_cast<size_t>(Histogram::IndirectGroupStride)], number));
    // Counted where draws are configured: with the split on, the back thread's line only (the
    // front's would always read 0).
    if (data.count[static_cast<size_t>(Zone::PushDescriptors)] != 0) {
        line += fmt::format(" | image descriptor stages {:.0f} ({:.0f} reused)", rate(19), rate(20));
    }
    // EDEN_NX2_LIGHT_DRAW: per draw kind (direct/indirect), the widest change since the previous
    // draw and the registers that most often kept draws off the light path; then how many were
    // set up the light way, why the others were not and why a full setup could not be followed.
    // Counted where draws are configured only.
    u64 draws_classified = 0;
    for (u32 i = 0; i < 2 * static_cast<u32>(DrawChange::Count); ++i) {
        draws_classified += data.counters[static_cast<size_t>(Counter::DrawChangeDirect) + i];
    }
    const auto changes = [&](DrawChange change) {
        const size_t index = static_cast<size_t>(change);
        return fmt::format("{:.0f}/{:.0f}",
                           rate(static_cast<size_t>(Counter::DrawChangeDirect) + index),
                           rate(static_cast<size_t>(Counter::DrawChangeIndirect) + index));
    };
    const auto counter = [&](Counter c) { return rate(static_cast<size_t>(c)); };
    const auto method = [](s64 value) { return fmt::format("{:#x}", value); };
    // Cut c: B decides light draws without classifying them (PrepareDrawB): its light counters
    // alone.
    const bool light_counted =
        draws_classified != 0 ||
        data.counters[static_cast<size_t>(Counter::LightDraws)] != 0 ||
        data.counters[static_cast<size_t>(Counter::LightUniformDraws)] != 0 ||
        data.counters[static_cast<size_t>(Counter::LightNotArmed)] != 0 ||
        data.counters[static_cast<size_t>(Counter::LightStateChanged)] != 0;
    if (light_counted) {
        if (draws_classified != 0) {
            line += fmt::format(" | draw changes direct/indirect: nothing {}, vertex streams {}, "
                                "+ index buffer {}, + uniforms {}, + textures {}, + state {}, "
                                "other work {} (first blocking register: {})",
                                changes(DrawChange::Nothing), changes(DrawChange::VertexStreams),
                                changes(DrawChange::IndexBuffer), changes(DrawChange::Uniforms),
                                changes(DrawChange::Textures), changes(DrawChange::State),
                                changes(DrawChange::OtherWork),
                                TopValues(histograms[static_cast<size_t>(Histogram::LightBlocking)],
                                          method, 6));
        }
        line += fmt::format(" | light draws {:.0f} ({:.0f} indirect); full: {:.0f} not after one, "
                            "{:.0f} state, {:.0f} other work, {:.0f} draw kind, {:.0f} fell back; "
                            "not followable: {:.0f} shape, {:.0f} memory writes (uniform way only), "
                            "{:.0f} transform feedback, {:.0f} render targets or images, {:.0f} cut "
                            "short",
                            counter(Counter::LightDraws), counter(Counter::LightIndirectDraws),
                            counter(Counter::LightNotArmed), counter(Counter::LightStateChanged),
                            counter(Counter::LightOtherWork), counter(Counter::LightShape),
                            counter(Counter::LightFellBack), counter(Counter::LightRefusedShape),
                            counter(Counter::LightRefusedWrites),
                            counter(Counter::LightRefusedFeedback),
                            counter(Counter::LightRefusedTargets),
                            counter(Counter::LightRefusedCutShort));
        line += fmt::format(" | light uniform draws {:.0f} ({:.0f} indirect); full: {:.0f} memory, "
                            "{:.0f} caches, {:.0f} binds, {:.0f} handles, {:.0f} storage "
                            "descriptors, {:.0f} TIC, {:.0f} TSC, {:.0f} fell back; rewritten with "
                            "the same value: {:.0f} handles, {:.0f} storage descriptors; verified "
                            "{:.0f} ({:.0f} mismatches; {:.0f} uniform slices moved, same bytes) "
                            "(first register outside: {})",
                            counter(Counter::LightUniformDraws),
                            counter(Counter::LightUniformIndirectDraws),
                            counter(Counter::LightUniformMemory), counter(Counter::LightUniformCaches),
                            counter(Counter::LightUniformBinds),
                            counter(Counter::LightUniformHandles),
                            counter(Counter::LightUniformStorage), counter(Counter::LightUniformTic),
                            counter(Counter::LightUniformTsc),
                            counter(Counter::LightUniformFellBack),
                            counter(Counter::LightUniformSameHandles),
                            counter(Counter::LightUniformSameStorage),
                            counter(Counter::LightUniformVerified),
                            counter(Counter::LightUniformMismatches),
                            counter(Counter::LightUniformSlicesSame),
                            TopValues(histograms[static_cast<size_t>(Histogram::LightUniformBlocking)],
                                      method, 6));
        if (data.counters[static_cast<size_t>(Counter::LightStorageChanged)] != 0) {
            line += fmt::format(" | storage descriptors changed in {:.0f} draws ({:.0f} buffers; "
                                "{:.0f} addresses, {:.0f} written, {:.0f} with handles too); "
                                "bound again {:.0f}, fell back {:.0f} (first: {})",
                                counter(Counter::LightStorageChanged),
                                counter(Counter::LightStorageBuffers),
                                counter(Counter::LightStorageAddress),
                                counter(Counter::LightStorageWritten),
                                counter(Counter::LightStorageWithHandles),
                                counter(Counter::LightStorageDraws),
                                counter(Counter::LightStorageFellBack),
                                TopValues(histograms[static_cast<size_t>(Histogram::LightStorageFirst)],
                                          method, 6));
        }
        line += fmt::format(" | constant buffer writes of held bytes skipped {:.0f} (invalidated "
                            "later {:.0f}), made {:.0f}, not counted (pages written around the "
                            "caches) {:.0f}",
                            counter(Counter::SameWritesSkipped),
                            counter(Counter::SameWritesInvalidatedLater),
                            counter(Counter::SameWritesMade),
                            counter(Counter::SameWritesUntracked));
        if (data.counters[static_cast<size_t>(Counter::LightPipelineCandidates)] != 0) {
            line += fmt::format(" | light pipeline candidates (counted only) {:.0f}: {:.0f} "
                                "programs changed, {:.0f} render targets, {:.0f} descriptors; "
                                "{:.0f} same pipeline ({:.0f} without uniforms), {:.0f} same "
                                "programs (first register: {})",
                                counter(Counter::LightPipelineCandidates),
                                counter(Counter::LightPipelinePrograms),
                                counter(Counter::LightPipelineTargets),
                                counter(Counter::LightPipelineDescriptors),
                                counter(Counter::LightPipelineSame),
                                counter(Counter::LightPipelineSamePlain),
                                counter(Counter::LightPipelineSwitch),
                                TopValues(histograms[static_cast<size_t>(
                                              Histogram::LightPipelineFirst)],
                                          method, 8));
        }
    }
    // Counted where uniform buffers are bound: the back's line with the split on.
    if (data.counters[static_cast<size_t>(Counter::UniformStageReuses)] != 0) {
        line += fmt::format(" | uniform binds reused through the stage slot only {:.0f}",
                            rate(static_cast<size_t>(Counter::UniformStageReuses)));
    }
    if (data.counters[static_cast<size_t>(Counter::ResetUploads)] != 0) {
        line += fmt::format(" | indirect draw reset uploads {:.0f} in {:.0f} copy commands",
                            rate(static_cast<size_t>(Counter::ResetUploads)),
                            rate(static_cast<size_t>(Counter::ResetCopyCommands)));
    }
    // Counted where vertex buffers are bound: the back's line with the split on.
    if (const u64 draws = data.counters[static_cast<size_t>(Counter::VertexDraws)]; draws != 0) {
        const u64 updates = data.counters[static_cast<size_t>(Counter::VertexUpdates)];
        const auto per = [&](Counter c, u64 base) {
            return base != 0 ? static_cast<double>(data.counters[static_cast<size_t>(c)]) /
                                   static_cast<double>(base)
                             : 0.0;
        };
        line += fmt::format(" | vertex streams per draw: {:.2f} enabled, {:.2f} buffers, {:.2f} "
                            "dirty, {:.2f} sync skipped ({:.2f} would skip, {:.2f} guarded); per "
                            "update {:.2f} resolved ({:.2f} to nothing) in {:.0f} updates/s",
                            per(Counter::VertexEnabled, draws), per(Counter::VertexBuffers, draws),
                            per(Counter::VertexDirtyEnabled, draws),
                            per(Counter::VertexSyncSkipped, draws),
                            per(Counter::VertexSyncWouldSkip, draws),
                            per(Counter::VertexSyncGuarded, draws),
                            per(Counter::VertexResolved, updates),
                            per(Counter::VertexResolvedDisabled, updates),
                            counter(Counter::VertexUpdates));
    }
    if (const u64 sync_total = data.counters[static_cast<size_t>(Counter::SyncMemoHits)] +
                               data.counters[static_cast<size_t>(Counter::SyncMemoMissEmpty)] +
                               data.counters[static_cast<size_t>(Counter::SyncMemoMissOther)] +
                               data.counters[static_cast<size_t>(Counter::SyncMemoMissPartial)] +
                               data.counters[static_cast<size_t>(Counter::SyncMemoMissStamped)];
        sync_total != 0) {
        line += fmt::format(
            " | buffer sync memo {:.0f}: hits {:.1f}%, misses empty {:.0f}, other range {:.0f}, "
            "partial {:.0f}, stamped {:.0f} ({:.0f} uploaded)",
            static_cast<double>(sync_total) * 1e9 / window,
            100.0 * static_cast<double>(data.counters[static_cast<size_t>(Counter::SyncMemoHits)]) /
                static_cast<double>(sync_total),
            counter(Counter::SyncMemoMissEmpty), counter(Counter::SyncMemoMissOther),
            counter(Counter::SyncMemoMissPartial), counter(Counter::SyncMemoMissStamped),
            counter(Counter::SyncMemoMissUploaded));
    }
    // Back stages and the diagnostics for them: each group only when it counted anything.
    const auto any = [&](Counter first, Counter last) {
        for (auto i = static_cast<size_t>(first); i <= static_cast<size_t>(last); ++i) {
            if (data.counters[i] != 0) {
                return true;
            }
        }
        return false;
    };
    if (any(Counter::JoinsA, Counter::GeometryReplays)) {
        line += fmt::format(
            " | stages: joins from A {:.0f} (engine {:.0f}, channel {:.0f}, value {:.0f}, flush "
            "{:.0f}, condition {:.0f}, upload {:.0f}, tagged {:.0f}, other {:.0f}; candidates "
            "{:.0f}, compute {:.0f}), joins from B {:.0f}, parks {:.0f}, ring KiB B {:.0f} C "
            "{:.0f}, delta words {:.0f}, side states {:.0f}, pass-through words {:.0f}, light "
            "repairs state {:.0f} textures {:.0f}, geometry replays {:.0f}",
            counter(Counter::JoinsA), counter(Counter::JoinsAEngine),
            counter(Counter::JoinsAChannel), counter(Counter::JoinsAValue),
            counter(Counter::JoinsAFlush), counter(Counter::JoinsACondition),
            counter(Counter::JoinsAUpload), counter(Counter::JoinsATagged),
            counter(Counter::JoinsAOther), counter(Counter::JoinCandidates),
            counter(Counter::JoinCandidatesCompute), counter(Counter::JoinsB),
            counter(Counter::Parks), counter(Counter::RingBytesB) / 1024.0,
            counter(Counter::RingBytesC) / 1024.0, counter(Counter::DeltaWords),
            counter(Counter::SideStates), counter(Counter::PassThrough),
            counter(Counter::LightRepairsState), counter(Counter::LightRepairsTexture),
            counter(Counter::GeometryReplays));
    }
    if (any(Counter::HazardPages, Counter::VerifyMismatches)) {
        line += fmt::format(" | stage check: violations {:.0f}, unproxied {:.0f}, hazards pages "
                            "{:.0f} untracked {:.0f} epoch {:.0f}; verified {:.0f} ({:.0f} "
                            "mismatches)",
                            counter(Counter::StageViolations), counter(Counter::StageUnproxied),
                            counter(Counter::HazardPages), counter(Counter::HazardUntracked),
                            counter(Counter::HazardEpoch), counter(Counter::VerifyChecks),
                            counter(Counter::VerifyMismatches));
    }
    if (any(Counter::RepairsNotFollowable, Counter::StreamMarksStamped)) {
        line += fmt::format(" | stage costs: light repairs not followable {:.0f}, pipeline "
                            "{:.0f}, records {:.0f}; presets kept {:.0f}; stream marks {:.0f} "
                            "({:.0f} stamped only)",
                            counter(Counter::RepairsNotFollowable),
                            counter(Counter::RepairsPipeline), counter(Counter::RepairsRecords),
                            counter(Counter::PresetsKept), counter(Counter::StreamMarks),
                            counter(Counter::StreamMarksStamped));
    }
    if (any(Counter::TextureBufferDraws, Counter::ConditionalRenderings)) {
        line += fmt::format(
            " | stage diagnostics: texture buffer draws {:.0f}, image buffer draws {:.0f}, "
            "transform feedback draws {:.0f}, reset fix lists {:.0f} ({:.0f} flushed), inline "
            "writes {:.0f} ({:.0f} on images, {:.0f} on pools), DMA writes {:.0f} ({:.0f} on "
            "images, {:.0f} on pools), cb data on images {:.0f} on pools {:.0f}, GPU-written data "
            "held: query {:.0f} buffer {:.0f} texture {:.0f}, light candidates after recording: "
            "submit {:.0f} tick work {:.0f} event {:.0f}, tick work requests {:.0f} ({:.0f} "
            "flushes, {:.0f} presents), compute {:.0f}, clears {:.0f}, fences {:.0f}, memory "
            "dirty queries {:.0f} ({:.0f} deferred), conditional renderings {:.0f}",
            counter(Counter::TextureBufferDraws), counter(Counter::ImageBufferDraws),
            counter(Counter::TransformFeedbackDraws), counter(Counter::ResetFixLists),
            counter(Counter::ResetFixFlushes), counter(Counter::InlineWrites),
            counter(Counter::InlineOnImages), counter(Counter::InlineOnPools),
            counter(Counter::DmaWrites), counter(Counter::DmaOnImages),
            counter(Counter::DmaOnPools), counter(Counter::CbDataOnImages),
            counter(Counter::CbDataOnPools), counter(Counter::HoldsGpuWrittenQuery),
            counter(Counter::HoldsGpuWrittenBuffer), counter(Counter::HoldsGpuWrittenTexture),
            counter(Counter::LightRecordedSubmit), counter(Counter::LightRecordedTickWork),
            counter(Counter::LightRecordedEvent), counter(Counter::TickWorkRequests),
            counter(Counter::TickWorkFlushes), counter(Counter::TickWorkComposites),
            counter(Counter::ComputeDispatches), counter(Counter::Clears),
            counter(Counter::FencesSignaled), counter(Counter::MemoryDirtyQueries),
            counter(Counter::MemoryDirtyThunks), counter(Counter::ConditionalRenderings));
    }
    // Counted where drains are called: the front's line with the split on.
    std::string drains;
    for (size_t i = 0; i < drain_site_names.size(); ++i) {
        const auto& entry = data.drains[i];
        if (entry[0] == 0) {
            continue;
        }
        drains += fmt::format("{}{} {:.0f}/{:.0f}/{:.2f}", drains.empty() ? "" : ", ",
                              drain_site_names[i], static_cast<double>(entry[0]) * 1e9 / window,
                              static_cast<double>(entry[1]) * 1e9 / window,
                              static_cast<double>(entry[2]) * ns_per_tick * 1000.0 / window);
    }
    if (!drains.empty()) {
        line += fmt::format(" | drains by caller (calls/waited/wait ms per second): {}", drains);
    }
    if (histograms[static_cast<size_t>(Histogram::IndirectGapMethods)].used != 0) {
        line += fmt::format(" | methods between indirect draws (first 16 of each): {}",
                            TopValues(histograms[static_cast<size_t>(Histogram::IndirectGapMethods)],
                                      interposer, 16));
    }
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    data = {};
    data.window_start = now;
    data.window_start_ticks = now_ticks;
}

} // namespace VideoCommon::Nx2Profile
