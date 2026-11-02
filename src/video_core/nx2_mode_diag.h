// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string>
#include <utility>

#include "common/common_types.h"
#include "common/cpu_features.h"
#include "common/logging.h"
#include "video_core/nx2_busy_clock.h"

namespace Core {
extern std::atomic<u64> g_nx2_present_count; // core/core.h: vi2 229 presents so far
}

namespace VideoCommon::Nx2ModeDiag {

// EDEN_NX2_MODE_DIAG=1 (off by default): what the game can time its frames with, to find how
// Donkey Kong Bananza picks its 60 / 30 / 20 fps mode (vi2 229 word 12 >> 16) and why a real
// GPU time turns the screen purple. Logs:
//  - every second a "nx2 mode diag" line: presents per mode (the interval
//    EDEN_NX2_PRESENT_INTERVAL_WORD takes from the request, else 1), timestamp writes
//    (GPU::GetTicks values in query / semaphore reports) and their span within a frame (written
//    ticks, host wall time, busy time of the GPU pipeline), the age of 229 word 10 (the CNTPCT of
//    the game)
//    when the present arrives, nvhost-ctrl-gpu GET_GPU_TIME calls, Ampere SEM_EXECUTE releases
//    that ask for a timestamp (dropped today), rejected native compute dispatches per program
//    and the render target / viewport sizes; then a "nx2 mode diag addresses" line: the report
//    addresses written most this second, with the average time between two writes of each;
//  - at each of the first 48 mode changes (EDEN_NX2_MODE_DIAG_MAX=<1-100000>): the 16 frames
//    before it (one line each) and the timestamp writes of the last 3 frames before it (at most
//    96), then the writes of the 6 frames after it (at most 400 lines); the same 6-frame window
//    at present 300 and every 1800th present after it;
//  - the first time a report semaphore word, QMD release flag word, SEM_EXECUTE timestamp
//    release or large render target / viewport size is seen.
// "ticks" are the values written (614.4 MHz GPU ticks over the divisor in effect: fast_gpu_time,
// EDEN_NX2_GPU_TICK_DIV or the busy clock); "t614" converts them at 614.4 MHz, "x" is t614 per
// wall millisecond (1/256 = 0.0039 by default). "busy" needs EDEN_NX2_PRESENT_DIAG=1 or
// EDEN_NX2_GPU_BUSY_CLOCK=1 (multicore), else it reads 0. Off: one static bool per call site.

inline bool Enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("EDEN_NX2_MODE_DIAG");
        return text != nullptr && text[0] != '0';
    }();
    return enabled;
}

/// EDEN_NX2_MODE_DIAG_MAX: how many mode changes get their frames dumped (default 48).
inline u32 MaxTransitions() {
    static const u32 max = [] {
        const char* text = std::getenv("EDEN_NX2_MODE_DIAG_MAX");
        const long value = text != nullptr ? std::strtol(text, nullptr, 10) : 48;
        if (value < 1 || value > 100000) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_DIAG_MAX={} out of 1-100000: 48", text);
            return 48U;
        }
        if (value != 48) {
            LOG_INFO(HW_GPU,
                     "nx2 mode diag: the first {} mode changes dumped (EDEN_NX2_MODE_DIAG_MAX)",
                     value);
        }
        return static_cast<u32>(value);
    }();
    return max;
}

enum class Source : u8 { Other, Report3D, PullerLong, ComputeReport, DmaFourWord };

inline const char* SourceName(Source source) {
    switch (source) {
    case Source::Report3D:
        return "3d";
    case Source::PullerLong:
        return "puller";
    case Source::ComputeReport:
        return "compute";
    case Source::DmaFourWord:
        return "dma";
    default:
        return "other";
    }
}

/// The engine whose report the query cache is handling on this thread.
inline thread_local Source g_source = Source::Other;

struct SourceScope {
    explicit SourceScope(Source source) : previous{g_source} {
        g_source = source;
    }
    ~SourceScope() {
        g_source = previous;
    }
    SourceScope(const SourceScope&) = delete;
    SourceScope& operator=(const SourceScope&) = delete;
    Source previous;
};

inline u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

constexpr double TicksPerMs = static_cast<double>(Common::WallClock::GPUTickFreq) / 1000.0;

struct Write {
    u64 address{};
    u64 value{};   ///< ticks written
    u64 wall_ns{}; ///< NowNs() then
    u64 busy{};    ///< Nx2BusyClock::Ticks() then (0: not tracked)
    u64 present{}; ///< vi2 presents so far then
    u64 late_ns{}; ///< since the report command reached the query cache
    u32 payload{};
    Source source{};
    bool fence{};
};

struct AddressStats {
    u64 address{};
    u64 total{};
    u64 last_value{}, last_wall{}, last_busy{};
    double sum_value{}, sum_wall{}, sum_busy{}; ///< deltas between writes, this second
    u32 writes{}; ///< this second
    u32 deltas{};
    Source source{};
    bool fence{};
    bool has_last{};
};

struct Frame {
    u64 present{};
    u32 mode_word{};
    u32 interval{};
    double gap_ms{}, wait_ms{}, composite_ms{}, pace_ms{}, age_ms{}, busy_ms{};
    u32 writes{};
    double span_t614{}, span_wall{}, span_busy{};
};

struct Target {
    u32 rt_w{}, rt_h{}, vp_w{}, vp_h{};
    u32 count{};
};

struct State {
    std::mutex mutex;
    std::array<AddressStats, 128> addresses{};
    size_t address_count{};
    u64 addresses_dropped{};
    std::array<Write, 1024> ring{}; ///< the last writes
    u64 ring_next{};
    // Writes since the previous present.
    u32 slice_writes{};
    u64 slice_first_value{}, slice_last_value{}, slice_first_wall{}, slice_last_wall{};
    u64 slice_first_busy{}, slice_last_busy{};
    std::array<Frame, 16> frames{}; ///< the last presents
    u64 frames_total{};
    u64 last_entered_ns{};
    u64 last_busy{};
    u32 last_mode_word{};
    bool have_mode{};
    u32 transitions{};
    u64 detail_until{}; ///< log every write while present <= this
    u32 detail_lines{};
    u32 window_lines{}; ///< logged in the current window (at most 400)
    // This second.
    u64 second_start_ns{};
    std::array<u32, 5> modes{};
    u32 presents{};
    u32 writes{}, fence_writes{}, late_writes{};
    double frame_span_t614{}, frame_span_wall{}, frame_span_busy{};
    u32 frames_with_writes{};
    double age_min{}, age_max{}, age_sum{};
    u32 sem_exec_releases{}, sem_exec_timestamp{}, sem_exec_wide{};
    std::array<std::pair<u64, u32>, 16> rejected{};
    size_t rejected_count{};
    std::array<Target, 24> targets{};
    size_t target_count{};
    // Seen so far (each logged once).
    std::array<u32, 32> report_words{};
    size_t report_word_count{};
    std::array<u32, 16> qmd_flags{};
    size_t qmd_flag_count{};
    std::array<std::array<u32, 4>, 64> sizes_seen{};
    size_t sizes_seen_count{};
    u32 sem_exec_logged{};
    std::atomic<u32> gpu_time_calls{};
    std::atomic<u64> gpu_time_last{};
};

inline State g_state;

inline u64 BusyNow() {
    return Nx2BusyClock::Tracked() ? Nx2BusyClock::Ticks() : 0;
}

/// b - a as a signed difference (writes from two threads may arrive out of order).
inline double Delta(u64 b, u64 a) {
    return static_cast<double>(static_cast<s64>(b - a));
}

inline void LogWrite(const Write& write, const AddressStats* stats, u64 slice_first_wall) {
    std::string same = "first";
    if (stats != nullptr && stats->has_last) {
        // Signed: another thread may have written a later timestamp first.
        const s64 ticks = static_cast<s64>(write.value - stats->last_value);
        same = fmt::format("{:.3f} ms wall, {} ticks = {:.3f} t614, {:.3f} ms busy",
                           Delta(write.wall_ns, stats->last_wall) / 1e6, ticks,
                           static_cast<double>(ticks) / TicksPerMs,
                           Delta(write.busy, stats->last_busy) / TicksPerMs);
    }
    LOG_INFO(HW_GPU,
             "nx2 mode diag write (present {}): {:#x} {}{} payload {:#x} ticks {} "
             "(since the previous write there: {}; {:.3f} ms into the frame; late {:.3f} ms)",
             write.present, write.address, SourceName(write.source), write.fence ? " fence" : "",
             write.payload, write.value, same,
             static_cast<double>(write.wall_ns - slice_first_wall) / 1e6,
             static_cast<double>(write.late_ns) / 1e6);
}

/// A timestamp the query cache has just written to guest memory (GPU::GetTicks value).
inline void Timestamp(u64 address, u64 value, u32 payload, bool fence, Source source,
                      u64 issued_ns) {
    Write write{
        .address = address,
        .value = value,
        .wall_ns = NowNs(),
        .busy = BusyNow(),
        .present = Core::g_nx2_present_count.load(std::memory_order_relaxed),
        .late_ns = 0,
        .payload = payload,
        .source = source,
        .fence = fence,
    };
    write.late_ns = issued_ns != 0 && write.wall_ns > issued_ns ? write.wall_ns - issued_ns : 0;
    auto& s = g_state;
    std::scoped_lock lock{s.mutex};
    s.ring[s.ring_next++ % s.ring.size()] = write;
    ++s.writes;
    s.fence_writes += fence ? 1 : 0;
    s.late_writes += write.late_ns > 2'000'000 ? 1 : 0;
    if (s.slice_writes++ == 0) {
        s.slice_first_value = value;
        s.slice_first_wall = write.wall_ns;
        s.slice_first_busy = write.busy;
    }
    s.slice_last_value = value;
    s.slice_last_wall = write.wall_ns;
    s.slice_last_busy = write.busy;
    AddressStats* stats = nullptr;
    for (size_t i = 0; i < s.address_count; ++i) {
        if (s.addresses[i].address == address) {
            stats = &s.addresses[i];
            break;
        }
    }
    if (stats == nullptr && s.address_count < s.addresses.size()) {
        stats = &s.addresses[s.address_count++];
        *stats = AddressStats{.address = address, .source = source, .fence = fence};
    }
    if (stats == nullptr) {
        ++s.addresses_dropped;
    }
    if (write.present <= s.detail_until && s.window_lines < 400 && s.detail_lines < 20000) {
        ++s.window_lines;
        ++s.detail_lines;
        LogWrite(write, stats, s.slice_first_wall);
    }
    if (stats != nullptr) {
        if (stats->has_last && value >= stats->last_value && write.wall_ns >= stats->last_wall) {
            stats->sum_value += static_cast<double>(value - stats->last_value);
            stats->sum_wall += static_cast<double>(write.wall_ns - stats->last_wall);
            stats->sum_busy += static_cast<double>(write.busy - stats->last_busy);
            ++stats->deltas;
        }
        stats->last_value = value;
        stats->last_wall = write.wall_ns;
        stats->last_busy = write.busy;
        stats->has_last = true;
        ++stats->writes;
        ++stats->total;
    }
}

/// Maxwell 3D SET_REPORT_SEMAPHORE_D word (operation, report type, structure size).
inline void Report3D(u32 word) {
    auto& s = g_state;
    std::scoped_lock lock{s.mutex};
    const auto seen = std::span{s.report_words}.first(s.report_word_count);
    if (std::ranges::find(seen, word) != seen.end() ||
        s.report_word_count == s.report_words.size()) {
        return;
    }
    s.report_words[s.report_word_count++] = word;
    LOG_INFO(HW_GPU,
             "nx2 mode diag: 3D report semaphore word {:#x} (operation {}, report {}, one word {}) "
             "first seen at present {}",
             word, word & 3, (word >> 23) & 0x1f, (word >> 28) & 1,
             Core::g_nx2_present_count.load(std::memory_order_relaxed));
}

/// Ampere host SEM_EXECUTE: RELEASE (op 1) with RELEASE_TIMESTAMP (bit 25) wants a timestamp at
/// +8 and PAYLOAD_SIZE (bit 24) a 64-bit payload; Puller::ProcessSemaphoreExecute writes neither.
inline void SemExecute(u32 argument, u64 address) {
    if ((argument & 7) != 1) {
        return;
    }
    auto& s = g_state;
    std::scoped_lock lock{s.mutex};
    ++s.sem_exec_releases;
    const bool timestamp = ((argument >> 25) & 1) != 0;
    const bool wide = ((argument >> 24) & 1) != 0;
    s.sem_exec_timestamp += timestamp ? 1 : 0;
    s.sem_exec_wide += wide ? 1 : 0;
    if ((timestamp || wide) && s.sem_exec_logged < 16) {
        ++s.sem_exec_logged;
        LOG_INFO(HW_GPU,
                 "nx2 mode diag: SEM_EXECUTE release {:#x} argument {:#x}{}{} at present {} "
                 "(only the 32-bit payload is written)",
                 address, argument, timestamp ? " RELEASE_TIMESTAMP" : "", wide ? " 64-bit" : "",
                 Core::g_nx2_present_count.load(std::memory_order_relaxed));
    }
}

/// A QMD release flag word (kepler_compute.cpp): logged the first time each is seen.
inline void QmdRelease(u32 flags, u64 address) {
    auto& s = g_state;
    std::scoped_lock lock{s.mutex};
    const auto seen = std::span{s.qmd_flags}.first(s.qmd_flag_count);
    if (std::ranges::find(seen, flags) != seen.end() || s.qmd_flag_count == s.qmd_flags.size()) {
        return;
    }
    s.qmd_flags[s.qmd_flag_count++] = flags;
    LOG_INFO(HW_GPU, "nx2 mode diag: QMD release flags {:#x} first seen ({:#x}, present {})",
             flags, address, Core::g_nx2_present_count.load(std::memory_order_relaxed));
}

/// nvhost-ctrl-gpu GET_GPU_TIME (the CPU reading the GPU timer).
inline void GpuTimeCall(u64 value) {
    g_state.gpu_time_calls.fetch_add(1, std::memory_order_relaxed);
    g_state.gpu_time_last.store(value, std::memory_order_relaxed);
}

/// A native compute dispatch whose program was rejected (its outputs are not written).
inline void RejectedCompute(u64 program) {
    auto& s = g_state;
    std::scoped_lock lock{s.mutex};
    for (size_t i = 0; i < s.rejected_count; ++i) {
        if (s.rejected[i].first == program) {
            ++s.rejected[i].second;
            return;
        }
    }
    if (s.rejected_count < s.rejected.size()) {
        s.rejected[s.rejected_count++] = {program, 1};
    }
}

/// Render targets changed: the size of colour target 0 (else of the depth target) and of
/// viewport 0 (twice the scale of its transform).
inline void RenderTarget(u32 rt_w, u32 rt_h, float scale_x, float scale_y) {
    const u32 vp_w = static_cast<u32>(std::lround(std::fabs(scale_x) * 2.0f));
    const u32 vp_h = static_cast<u32>(std::lround(std::fabs(scale_y) * 2.0f));
    auto& s = g_state;
    std::scoped_lock lock{s.mutex};
    bool found = false;
    for (size_t i = 0; i < s.target_count; ++i) {
        auto& target = s.targets[i];
        if (target.rt_w == rt_w && target.rt_h == rt_h && target.vp_w == vp_w &&
            target.vp_h == vp_h) {
            ++target.count;
            found = true;
            break;
        }
    }
    if (!found && s.target_count < s.targets.size()) {
        s.targets[s.target_count++] = {rt_w, rt_h, vp_w, vp_h, 1};
    }
    // Large targets only (a dynamic resolution moves those of the scene): log new sizes.
    if (static_cast<u64>(rt_w) * rt_h < 1280 * 720) {
        return;
    }
    const std::array<u32, 4> key{rt_w, rt_h, vp_w, vp_h};
    const auto seen = std::span{s.sizes_seen}.first(s.sizes_seen_count);
    if (std::ranges::find(seen, key) != seen.end() || s.sizes_seen_count == s.sizes_seen.size()) {
        return;
    }
    s.sizes_seen[s.sizes_seen_count++] = key;
    LOG_INFO(HW_GPU,
             "nx2 mode diag: render target {}x{} with viewport {}x{} first seen at present {} "
             "(mode word {:#x})",
             rt_w, rt_h, vp_w, vp_h, Core::g_nx2_present_count.load(std::memory_order_relaxed),
             s.last_mode_word);
}

inline std::string FrameText(const Frame& f) {
    return fmt::format("present {} mode {:#x} interval {}: {:.1f} ms after the previous one (wait "
                       "{:.1f}, composite {:.1f}, pacing {:.1f}), word 10 {:.1f} ms old, busy "
                       "{:.1f} ms; {} timestamps spanning {:.3f} t614, {:.2f} ms wall, {:.2f} ms busy",
                       f.present, f.mode_word, f.interval, f.gap_ms, f.wait_ms, f.composite_ms,
                       f.pace_ms, f.age_ms, f.busy_ms, f.writes, f.span_t614, f.span_wall,
                       f.span_busy);
}

/// vi2 229 after its pacing (nx2_gsv.h PresentClientBuffer). `words` is the request; word 10 is
/// the CNTPCT of the game (low 32 bits), word 12 its mode (Bananza: interval << 16 | 60).
inline void Present(const u32* words, u32 interval, std::chrono::steady_clock::time_point entered,
                    std::chrono::steady_clock::time_point waited,
                    std::chrono::steady_clock::time_point composited,
                    std::chrono::steady_clock::time_point paced) {
    const auto ms = [](auto duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    const u64 entered_ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(entered.time_since_epoch()).count());
    // The CNTPCT of the game when the present arrived (now minus the time spent in here).
    const u64 frequency = (std::max<u64>)(Common::g_wall_clock.GetGuestCNTFRQ(), 1);
    const u32 cntpct = static_cast<u32>(Common::g_wall_clock.GetCNTPCT());
    const double since_entered = ms(std::chrono::steady_clock::now() - entered);
    const double age_ms = static_cast<double>(static_cast<u32>(cntpct - words[10])) * 1000.0 /
                              static_cast<double>(frequency) -
                          since_entered;
    const u64 busy = BusyNow();
    auto& s = g_state;
    std::scoped_lock lock{s.mutex};
    Frame frame{
        .present = Core::g_nx2_present_count.load(std::memory_order_relaxed),
        .mode_word = words[12],
        .interval = interval,
        .gap_ms = s.last_entered_ns != 0 && entered_ns > s.last_entered_ns
                      ? static_cast<double>(entered_ns - s.last_entered_ns) / 1e6
                      : 0.0,
        .wait_ms = ms(waited - entered),
        .composite_ms = ms(composited - waited),
        .pace_ms = ms(paced - composited),
        .age_ms = age_ms,
        .busy_ms = s.last_busy != 0 && busy >= s.last_busy
                       ? static_cast<double>(busy - s.last_busy) / TicksPerMs
                       : 0.0,
        .writes = s.slice_writes,
        .span_t614 = Delta(s.slice_last_value, s.slice_first_value) / TicksPerMs,
        .span_wall = Delta(s.slice_last_wall, s.slice_first_wall) / 1e6,
        .span_busy = Delta(s.slice_last_busy, s.slice_first_busy) / TicksPerMs,
    };
    s.last_entered_ns = entered_ns;
    s.last_busy = busy;
    s.slice_writes = 0;
    s.slice_first_value = s.slice_last_value = 0;
    s.slice_first_wall = s.slice_last_wall = 0;
    s.slice_first_busy = s.slice_last_busy = 0;

    const bool changed = s.have_mode && words[12] != s.last_mode_word;
    if (changed && s.transitions < MaxTransitions()) {
        ++s.transitions;
        LOG_INFO(HW_GPU,
                 "nx2 mode diag: mode word {:#x} -> {:#x} at present {} (change {}); the 16 "
                 "frames before:",
                 s.last_mode_word, words[12], frame.present, s.transitions);
        const u64 count = (std::min<u64>)(s.frames_total, s.frames.size());
        for (u64 i = count; i > 0; --i) {
            LOG_INFO(HW_GPU, "nx2 mode diag:   -{} {}", i,
                     FrameText(s.frames[(s.frames_total - i) % s.frames.size()]));
        }
        LOG_INFO(HW_GPU, "nx2 mode diag:   now {}", FrameText(frame));
        // The writes of the last three frames before the change (the latest 96).
        const u64 since = frame.present > 3 ? frame.present - 3 : 0;
        const u64 oldest = s.ring_next - (std::min<u64>)(s.ring_next, s.ring.size());
        u64 start = s.ring_next;
        while (start > oldest && s.ring_next - start < 96 &&
               s.ring[(start - 1) % s.ring.size()].present >= since) {
            --start;
        }
        for (u64 i = start; i < s.ring_next; ++i) {
            const Write& write = s.ring[i % s.ring.size()];
            LOG_INFO(HW_GPU,
                     "nx2 mode diag:   write (present {}): {:#x} {}{} payload {:#x} ticks {} "
                     "busy {} late {:.3f} ms",
                     write.present, write.address, SourceName(write.source),
                     write.fence ? " fence" : "", write.payload, write.value, write.busy,
                     static_cast<double>(write.late_ns) / 1e6);
        }
        s.detail_until = (std::max)(s.detail_until, frame.present + 6);
        s.window_lines = 0;
    }
    if (frame.present == 300 || (frame.present > 300 && (frame.present - 300) % 1800 == 0)) {
        s.detail_until = (std::max)(s.detail_until, frame.present + 6);
        s.window_lines = 0;
        LOG_INFO(HW_GPU, "nx2 mode diag: sample, the writes of the next 6 frames follow");
    }
    if (frame.present <= s.detail_until) {
        LOG_INFO(HW_GPU, "nx2 mode diag frame: {}", FrameText(frame));
    }
    s.last_mode_word = words[12];
    s.have_mode = true;
    s.frames[s.frames_total++ % s.frames.size()] = frame;

    // This second.
    ++s.presents;
    ++s.modes[(std::min<u32>)(interval, 4)];
    if (frame.writes != 0) {
        ++s.frames_with_writes;
        s.frame_span_t614 += frame.span_t614;
        s.frame_span_wall += frame.span_wall;
        s.frame_span_busy += frame.span_busy;
    }
    s.age_min = s.presents == 1 ? age_ms : (std::min)(s.age_min, age_ms);
    s.age_max = s.presents == 1 ? age_ms : (std::max)(s.age_max, age_ms);
    s.age_sum += age_ms;
    if (s.second_start_ns == 0) {
        s.second_start_ns = entered_ns;
        return;
    }
    if (entered_ns - s.second_start_ns < 1'000'000'000) {
        return;
    }
    const double seconds = static_cast<double>(entered_ns - s.second_start_ns) / 1e9;
    const double with = (std::max<u32>)(s.frames_with_writes, 1);
    std::string rejected;
    for (size_t i = 0; i < s.rejected_count; ++i) {
        rejected += fmt::format(" {:#x}x{}", s.rejected[i].first, s.rejected[i].second);
    }
    s.rejected_count = 0; // counted per second, so later programs find room
    std::array<Target, 24> targets = s.targets;
    std::sort(targets.begin(), targets.begin() + static_cast<std::ptrdiff_t>(s.target_count),
              [](const Target& a, const Target& b) {
                  const u64 area_a = static_cast<u64>(a.rt_w) * a.rt_h;
                  const u64 area_b = static_cast<u64>(b.rt_w) * b.rt_h;
                  return area_a > area_b || (area_a == area_b && a.count > b.count);
              });
    std::string target_text;
    for (size_t i = 0; i < (std::min<size_t>)(s.target_count, 4); ++i) {
        target_text += fmt::format(" {}x{} vp {}x{} x{}", targets[i].rt_w, targets[i].rt_h,
                                   targets[i].vp_w, targets[i].vp_h, targets[i].count);
    }
    const std::string dropped =
        s.addresses_dropped != 0 ? fmt::format(", {} writes to others", s.addresses_dropped)
                                 : std::string{};
    LOG_INFO(HW_GPU,
             "nx2 mode diag {:.2f} s: {} presents (interval 1x{} 2x{} 3x{} 4x{}), {} timestamps "
             "({} fence, {} written over 2 ms late, {} addresses{}), per frame with any ({}): "
             "span {:.3f} t614, {:.2f} ms wall, {:.2f} ms busy; word 10 age {:.1f}/{:.1f}/{:.1f} "
             "ms; GET_GPU_TIME {} (last {}); SEM_EXECUTE releases {} ({} want a timestamp, {} "
             "64-bit); rejected compute dispatches{}; targets{}",
             seconds, s.presents, s.modes[1], s.modes[2], s.modes[3], s.modes[4], s.writes,
             s.fence_writes, s.late_writes, s.address_count, dropped, s.frames_with_writes,
             s.frame_span_t614 / with, s.frame_span_wall / with, s.frame_span_busy / with,
             s.age_min, s.age_sum / s.presents, s.age_max,
             s.gpu_time_calls.exchange(0, std::memory_order_relaxed),
             s.gpu_time_last.load(std::memory_order_relaxed), s.sem_exec_releases,
             s.sem_exec_timestamp, s.sem_exec_wide,
             rejected.empty() ? std::string{" none"} : rejected,
             target_text.empty() ? std::string{" none"} : target_text);
    // The addresses written most this second.
    std::array<size_t, 128> order{};
    for (size_t i = 0; i < s.address_count; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(s.address_count),
              [&](size_t a, size_t b) { return s.addresses[a].writes > s.addresses[b].writes; });
    std::string address_text;
    for (size_t i = 0; i < (std::min<size_t>)(s.address_count, 8); ++i) {
        const AddressStats& a = s.addresses[order[i]];
        if (a.writes == 0) {
            break;
        }
        const double deltas = (std::max<u32>)(a.deltas, 1);
        const double wall = a.sum_wall / deltas / 1e6;
        const double t614 = a.sum_value / deltas / TicksPerMs;
        address_text += fmt::format(" | {:#x} {}{} {}/s: every {:.2f} ms wall, {:.3f} t614 "
                                    "(x{:.4f}), {:.2f} ms busy",
                                    a.address, SourceName(a.source), a.fence ? " fence" : "",
                                    a.writes, wall, t614, wall > 0.0 ? t614 / wall : 0.0,
                                    a.sum_busy / deltas / TicksPerMs);
    }
    if (!address_text.empty()) {
        LOG_INFO(HW_GPU, "nx2 mode diag addresses:{}", address_text);
    }
    // Addresses not written this second make room for others (rings of semaphores move on).
    size_t kept_addresses = 0;
    for (size_t i = 0; i < s.address_count; ++i) {
        AddressStats a = s.addresses[i];
        if (a.writes == 0) {
            continue;
        }
        a.writes = 0;
        a.deltas = 0;
        a.sum_value = a.sum_wall = a.sum_busy = 0.0;
        s.addresses[kept_addresses++] = a;
    }
    s.address_count = kept_addresses;
    s.second_start_ns = entered_ns;
    s.modes = {};
    s.presents = 0;
    s.writes = s.fence_writes = s.late_writes = 0;
    s.addresses_dropped = 0;
    s.frame_span_t614 = s.frame_span_wall = s.frame_span_busy = 0.0;
    s.frames_with_writes = 0;
    s.age_min = s.age_max = s.age_sum = 0.0;
    s.sem_exec_releases = s.sem_exec_timestamp = s.sem_exec_wide = 0;
    s.target_count = 0;
}

} // namespace VideoCommon::Nx2ModeDiag
