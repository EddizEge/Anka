// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

/// Reactive flushing (use_reactive_flushing: guest CPU reads of GPU-cached pages trap and ask the
/// GPU for its data, GPU::OnCPURead) diagnostics and switches.
///
/// EDEN_NX2_REACTIVE_STATS=1: counts the trapped reads, their flushes and what they latch, and
/// logs one "nx2 reactive:" line about every 10 s (from the rasterizer's TickFrame; from a CPU read
/// when no frame came for 20 s). Unset: no counter is touched and no line is written; each hook
/// costs a load and a branch.
namespace Common::Nx2Reactive {

/// A switch is on when set to anything but "" or "0".
[[nodiscard]] inline bool ParseFlag(const char* value) noexcept {
    return value != nullptr && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
}

/// A frame count; 0 (off) when unset or not a number.
[[nodiscard]] inline u64 ParseFrames(const char* value) noexcept {
    if (value == nullptr) {
        return 0;
    }
    char* end = nullptr;
    const unsigned long long frames = std::strtoull(value, &end, 10);
    return end == value || *end != '\0' ? 0 : static_cast<u64>(frames);
}

/// std::getenv (dynarmic, which includes this, builds without _CRT_SECURE_NO_WARNINGS).
[[nodiscard]] inline const char* GetEnv(const char* name) noexcept {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

/// EDEN_NX2_REACTIVE_STATS
[[nodiscard]] inline bool StatsEnabled() noexcept {
    static const bool enabled = ParseFlag(GetEnv("EDEN_NX2_REACTIVE_STATS"));
    return enabled;
}

enum class Counter : u32 {
    CpuReads,               ///< GPU::OnCPURead calls
    SyncFlushes,            ///< Of them, the ones that flushed and waited for it (not preemptive)
    SyncFlushUs,            ///< Their wait (request, GPU tick, flush), microseconds
    SyncFlushMaxUs,         ///< The longest one in the window
    FlushAreaCalls,         ///< RasterizerVulkan::GetFlushArea calls
    FlushAreaUs,            ///< OnCPURead's GetFlushArea time (with a stage join, if any)
    BufferLockUs,           ///< GetFlushArea's wait for BufferCache::mutex
    TextureLockUs,          ///< GetFlushArea's wait for TextureCache::mutex
    RtDownloadsForced,      ///< BindRenderTarget queued a download: pitch linear from the start
    RtDownloadBytesForced,  ///< Their unswizzled bytes
    RtDownloadsLatched,     ///< BindRenderTarget queued a download: latched by a CPU read
    RtDownloadBytesLatched, ///< Their unswizzled bytes
    BufferDownloads,        ///< Buffer copies back to guest memory queued at fences
    BufferDownloadBytes,    ///< Their bytes
    FirstReadFlushes,       ///< EDEN_NX2_FLUSH_FIRST_READ: reads made to flush their buffer pages
    LatchPagesDropped,      ///< EDEN_NX2_READ_LATCH_AGE: CPU-read buffer pages dropped
    LatchImagesCleared,     ///< EDEN_NX2_READ_LATCH_AGE: images whose read latch was cleared
    Frames,                 ///< Rasterizer TickFrame calls
    Count,
};

enum class Gauge : u32 {
    ReadPages,             ///< 4 KiB pages in the buffer cache's CPU-read ranges
    StagingBytes,          ///< The rasterizer staging pool's cached buffers, all uses
    StagingDownloadBytes,  ///< Of them, download (host memory) buffers
    LatchedImages,         ///< Images latched by a CPU read (tracked by EDEN_NX2_READ_LATCH_AGE)
    Count,
};

/// Slots of the per core slow read counters (memory's HostCoreSlot; more go to the last).
inline constexpr std::size_t SLOW_READ_SLOTS = 8;

struct alignas(64) PaddedCounter {
    std::atomic<u64> value{};
};

struct State {
    /// Memory::HandleRasterizerDownload calls (reads of GPU-cached pages), per core slot
    std::array<PaddedCounter, SLOW_READ_SLOTS> slow_reads{};
    alignas(64) std::array<std::atomic<u64>, static_cast<std::size_t>(Counter::Count)> counters{};
    std::array<std::atomic<u64>, static_cast<std::size_t>(Gauge::Count)> gauges{};
    /// Guest instructions dynarmic took off fastmem after a fault (do_not_fastmem inserts). The
    /// fault path counts them always (rare, and no other cost); the line shows the change.
    alignas(64) std::atomic<u64> fastmem_fallbacks{};
    u64 fastmem_fallbacks_logged{};
    /// Render target images logged at their first queued download (at most MAX_IMAGE_LOGS)
    std::atomic<u32> image_logs{};
    /// steady_clock time (ns) of the next line; 0 before the first call
    std::atomic<s64> next_report_ns{};
    s64 window_start_ns{};
};

inline constexpr u32 MAX_IMAGE_LOGS = 16;
inline constexpr s64 REPORT_PERIOD_NS = 10'000'000'000;

[[nodiscard]] inline State& GetState() noexcept {
    static State state;
    return state;
}

inline void Add(Counter counter, u64 value) noexcept {
    GetState().counters[static_cast<std::size_t>(counter)].fetch_add(value,
                                                                     std::memory_order_relaxed);
}

inline void Max(Counter counter, u64 value) noexcept {
    auto& slot = GetState().counters[static_cast<std::size_t>(counter)];
    u64 seen = slot.load(std::memory_order_relaxed);
    while (seen < value &&
           !slot.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

inline void SetGauge(Gauge gauge, u64 value) noexcept {
    GetState().gauges[static_cast<std::size_t>(gauge)].store(value, std::memory_order_relaxed);
}

inline void CountSlowRead(std::size_t slot) noexcept {
    const std::size_t index = slot < SLOW_READ_SLOTS ? slot : SLOW_READ_SLOTS - 1;
    GetState().slow_reads[index].value.fetch_add(1, std::memory_order_relaxed);
}

inline void CountFastmemFallback() noexcept {
    GetState().fastmem_fallbacks.fetch_add(1, std::memory_order_relaxed);
}

/// Whether this render target image gets its one log line (the first MAX_IMAGE_LOGS do).
[[nodiscard]] inline bool TakeImageLog() noexcept {
    return GetState().image_logs.fetch_add(1, std::memory_order_relaxed) < MAX_IMAGE_LOGS;
}

[[nodiscard]] inline s64 NowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] inline u64 MicrosecondsSince(s64 start_ns) noexcept {
    const s64 elapsed = NowNs() - start_ns;
    return elapsed > 0 ? static_cast<u64>(elapsed) / 1000 : 0;
}

/// One window's numbers.
struct Window {
    double seconds{};
    std::array<u64, SLOW_READ_SLOTS> slow_reads{};
    std::array<u64, static_cast<std::size_t>(Counter::Count)> counters{};
    std::array<u64, static_cast<std::size_t>(Gauge::Count)> gauges{};
    u64 fastmem_fallbacks{};       ///< In the window
    u64 fastmem_fallbacks_total{}; ///< Since the start

    [[nodiscard]] u64 Get(Counter counter) const noexcept {
        return counters[static_cast<std::size_t>(counter)];
    }
    [[nodiscard]] u64 Get(Gauge gauge) const noexcept {
        return gauges[static_cast<std::size_t>(gauge)];
    }
};

/// Takes the counters since the last window (and zeroes them); the gauges as last set.
[[nodiscard]] inline Window TakeWindow(State& state, s64 now_ns) noexcept {
    Window window;
    window.seconds = state.window_start_ns == 0
                         ? 0.0
                         : static_cast<double>(now_ns - state.window_start_ns) / 1e9;
    state.window_start_ns = now_ns;
    for (std::size_t i = 0; i < SLOW_READ_SLOTS; ++i) {
        window.slow_reads[i] = state.slow_reads[i].value.exchange(0, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < window.counters.size(); ++i) {
        window.counters[i] = state.counters[i].exchange(0, std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < window.gauges.size(); ++i) {
        window.gauges[i] = state.gauges[i].load(std::memory_order_relaxed);
    }
    window.fastmem_fallbacks_total = state.fastmem_fallbacks.load(std::memory_order_relaxed);
    window.fastmem_fallbacks = window.fastmem_fallbacks_total - state.fastmem_fallbacks_logged;
    state.fastmem_fallbacks_logged = window.fastmem_fallbacks_total;
    return window;
}

[[nodiscard]] inline std::string FormatWindow(const Window& w) {
    u64 slow_reads = 0;
    std::size_t last_slot = 0;
    for (std::size_t i = 0; i < SLOW_READ_SLOTS; ++i) {
        slow_reads += w.slow_reads[i];
        if (w.slow_reads[i] != 0) {
            last_slot = i;
        }
    }
    std::string per_slot;
    for (std::size_t i = 0; i <= last_slot; ++i) {
        per_slot += fmt::format("{}{}", i == 0 ? "" : "/", w.slow_reads[i]);
    }
    const auto kib = [](u64 bytes) { return (bytes + 1023) / 1024; };
    const auto mib = [](u64 bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
    using C = Counter;
    using G = Gauge;
    return fmt::format(
        "nx2 reactive: {:.1f} s, {} frames; slow reads {} (cores {}); cpu reads {}; sync flushes {} "
        "wait {} us max {} us; flush area {} calls {} us, buffer lock {} us, texture lock {} us; "
        "rt downloads forced {} ({} KiB) latched {} ({} KiB); buffer copy back {} ({} KiB); read "
        "pages {}; staging {:.1f} MiB download {:.1f} MiB; fastmem fallbacks {} (total {}); first "
        "read flushes {}; latch age dropped {} pages {} images, latched images {}",
        w.seconds, w.Get(C::Frames), slow_reads, per_slot, w.Get(C::CpuReads),
        w.Get(C::SyncFlushes), w.Get(C::SyncFlushUs), w.Get(C::SyncFlushMaxUs),
        w.Get(C::FlushAreaCalls), w.Get(C::FlushAreaUs), w.Get(C::BufferLockUs),
        w.Get(C::TextureLockUs), w.Get(C::RtDownloadsForced), kib(w.Get(C::RtDownloadBytesForced)),
        w.Get(C::RtDownloadsLatched), kib(w.Get(C::RtDownloadBytesLatched)),
        w.Get(C::BufferDownloads), kib(w.Get(C::BufferDownloadBytes)), w.Get(G::ReadPages),
        mib(w.Get(G::StagingBytes)), mib(w.Get(G::StagingDownloadBytes)), w.fastmem_fallbacks,
        w.fastmem_fallbacks_total, w.Get(C::FirstReadFlushes), w.Get(C::LatchPagesDropped),
        w.Get(C::LatchImagesCleared), w.Get(G::LatchedImages));
}

/// Whether a line is due at now_ns, `slack_ns` past its time; only one caller gets true for it
/// (and the next one is set). The first call starts the first window.
[[nodiscard]] inline bool TakeReportDue(State& state, s64 now_ns, s64 slack_ns) noexcept {
    s64 next = state.next_report_ns.load(std::memory_order_relaxed);
    if (next == 0) {
        if (state.next_report_ns.compare_exchange_strong(next, now_ns + REPORT_PERIOD_NS,
                                                         std::memory_order_relaxed)) {
            state.window_start_ns = now_ns;
        }
        return false;
    }
    if (now_ns < next + slack_ns) {
        return false;
    }
    return state.next_report_ns.compare_exchange_strong(next, now_ns + REPORT_PERIOD_NS,
                                                        std::memory_order_relaxed);
}

/// Writes the window's line.
inline void Report(s64 now_ns) {
    LOG_INFO(HW_GPU, "{}", FormatWindow(TakeWindow(GetState(), now_ns)));
}

} // namespace Common::Nx2Reactive
