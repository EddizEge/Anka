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
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait", "parameter flush",
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
    Count,
};

struct HistogramData {
    std::array<std::pair<s64, u64>, 16> slots{}; ///< Value and times seen
    u32 used{};
    u64 dropped{}; ///< Notes of values that found no free slot
};

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, 40> counters{}; ///< See Counter
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

/// The three most frequent values of a histogram, "value x times, ...".
template <typename Format>
std::string TopValues(const HistogramData& data, Format&& format) {
    auto slots = data.slots;
    const auto end = slots.begin() + data.used;
    std::sort(slots.begin(), end, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string text;
    for (auto it = slots.begin(); it != end && it != slots.begin() + 3; ++it) {
        text += fmt::format("{}{} x{}", text.empty() ? "" : ", ", format(it->first), it->second);
    }
    if (data.used > 3 || data.dropped != 0) {
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
        // run, m: macro write, ?: an entry of another kind or engine).
        constexpr std::array<const char*, 4> kinds{"?", "p", "r", "m"};
        return fmt::format("{}{:#x}", kinds[static_cast<size_t>(key >> 16) & 3], key & 0xffff);
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
    line += fmt::format(" | image descriptor stages {:.0f} ({:.0f} reused)", rate(19), rate(20));
    LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
    data = {};
    data.window_start = now;
}

} // namespace VideoCommon::Nx2Profile
