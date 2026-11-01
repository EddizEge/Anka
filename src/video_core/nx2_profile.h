// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <string>
#include <tuple>
#include <utility>
#include <unordered_set>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/cpu_features.h"

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
};

// Read once at startup: Enabled() runs several times per method write (no guard to check).
inline const bool g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr;

inline bool Enabled() {
    return g_enabled;
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

/// Values noted on the GPU thread, logged per window as the most frequent ones.
enum class Histogram : u32 {
    IndirectInterposer,  ///< First entry after an unmerged indirect draw (see FrontIndirectDraw)
    IndirectStep,        ///< Record address step of indirect draws that did not follow
    IndirectGroupStride, ///< Stride of the group a step did not match
    IndirectGapMethods,  ///< Methods written between two indirect draws (the first 16 of each gap)
    LightBlocking,       ///< First register that kept a draw off the light path (EDEN_NX2_LIGHT_DRAW)
    LightUniformBlocking, ///< First register outside what a light draw with uniforms sets up again
    Count,
};

struct HistogramData {
    std::array<std::pair<s64, u64>, 32> slots{}; ///< Value and times seen
    u32 used{};
    u64 dropped{}; ///< Notes of values that found no free slot
};

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, 96> counters{}; ///< See Counter
    std::array<HistogramData, static_cast<size_t>(Histogram::Count)> histograms{};
    u64 child_ns{};    ///< Time of the zones nested in the current one
    u64 window_start{};
};

inline thread_local ThreadData t_data;

class Scope {
public:
    explicit Scope(Zone zone_) : zone{zone_}, enabled{Enabled()} {
        if (!enabled) {
            return;
        }
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        start = Now();
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        const u64 elapsed = Now() - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(zone);
        data.exclusive_ns[index] += elapsed - data.child_ns;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone;
    bool enabled;
    u64 start{};
    u64 parent_child_ns{};
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
    EmptyDraws,       ///< Draws with no indices, vertices or instances
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
    Count,
};

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
    if (data.window_start == 0) {
        data.window_start = now;
        data.child_ns = 0;
        return;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    // Time in no zone at all is the wall time minus the zones' totals.
    u64 zoned{};
    for (size_t i = 1; i < data.exclusive_ns.size(); ++i) {
        zoned += data.exclusive_ns[i];
    }
    data.exclusive_ns[0] = window > zoned ? window - zoned : 0;
    std::string line;
    for (size_t i = 0; i < data.exclusive_ns.size(); ++i) {
        if (data.exclusive_ns[i] == 0 && data.count[i] == 0) {
            continue;
        }
        // Per second of wall time: milliseconds spent and calls made.
        const double ms = static_cast<double>(data.exclusive_ns[i]) * 1000.0 / window;
        const double calls = static_cast<double>(data.count[i]) * 1e9 / window;
        line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", zone_names[i], ms, calls);
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
    if (draws_classified != 0) {
        line += fmt::format(" | draw changes direct/indirect: nothing {}, vertex streams {}, + index "
                            "buffer {}, + uniforms {}, + textures {}, + state {}, other work {} "
                            "(first blocking register: {})",
                            changes(DrawChange::Nothing), changes(DrawChange::VertexStreams),
                            changes(DrawChange::IndexBuffer), changes(DrawChange::Uniforms),
                            changes(DrawChange::Textures), changes(DrawChange::State),
                            changes(DrawChange::OtherWork),
                            TopValues(histograms[static_cast<size_t>(Histogram::LightBlocking)],
                                      method, 6));
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
    }
    if (histograms[static_cast<size_t>(Histogram::IndirectGapMethods)].used != 0) {
        line += fmt::format(" | methods between indirect draws (first 16 of each): {}",
                            TopValues(histograms[static_cast<size_t>(Histogram::IndirectGapMethods)],
                                      interposer, 16));
    }
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    data = {};
    data.window_start = now;
}

} // namespace VideoCommon::Nx2Profile
