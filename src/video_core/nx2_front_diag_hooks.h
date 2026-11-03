// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_front_diag.h"
#include "video_core/nx2_profile.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

// EDEN_NX2_GPU_PROFILE: the split GPU thread's front diag lines (nx2_front_diag.h), counted where
// the front sends work and logged for the front's profile window once the next one began:
// - "front macros": Maxwell3D front macro calls by start address (sampled time, JIT / HLE);
// - "front sends": DmaPusher method calls and macro writes, time spent sending them (sampled);
// - "front register writes": 3D register writes by method, of the value held or not sent;
// - "front drains": each drain caller's waits by length.
// Front thread only, behind On(); the counters touch nothing the emulation reads, the timed paths
// are out of line (the untimed ones keep their one call). Also EDEN_NX2_PROBE_FRONT_US=n: the
// front spins n microseconds per command list (any profile).
namespace VideoCommon::Nx2FrontDiag {

// EDEN_NX2_FRONT_DIAG=0: these lines off with the profile on (their counters cost the front a few
// percent more). Read at the first call, as Nx2Profile::Enabled() is.
inline std::atomic<s8> g_on{-1};

YUZU_NO_INLINE inline bool ReadOn() {
    const char* value = std::getenv("EDEN_NX2_FRONT_DIAG");
    const bool on = Nx2Profile::Enabled() && (value == nullptr || value[0] != '0');
    g_on.store(on ? 1 : 0, std::memory_order_relaxed);
    if (on) {
        LOG_INFO(HW_GPU,
                 "Front diag lines on with the profile (EDEN_NX2_FRONT_DIAG=0 turns them off)");
    }
    return on;
}

inline bool On() {
    const s8 value = g_on.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        return ReadOn();
    }
    return value != 0;
}

/// Who a DmaPusher method call went to.
enum class SendKind : u8 {
    Puller,       ///< Puller methods (semaphores, binds)
    Front3D,      ///< Maxwell3D's front (FrontCallMethod / FrontCallMultiMethod)
    FrontCompute, ///< KeplerCompute's front
    Split,        ///< Another engine, sent to the back as it is
    Direct,       ///< The split off: run on this thread
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(SendKind::Count)> send_kind_names{
    "puller", "3D front", "compute front", "other engines", "unsplit",
};

inline constexpr u32 MacroSampleBits = 3; ///< One macro call in 8 timed
inline constexpr u32 SendSampleBits = 4;  ///< One method call or macro write in 16 timed

struct State {
    // The window counted in (Nx2Profile's).
    u64 window{};
    u64 window_ticks{};

    // Per window.
    MacroTable macros;
    RegisterWrites registers;
    std::array<DrainHistogram, static_cast<size_t>(Nx2Profile::DrainSite::Count)> drains{};
    std::array<u64, static_cast<size_t>(SendKind::Count)> send_calls{};
    std::array<u64, static_cast<size_t>(SendKind::Count)> send_sampled{};
    std::array<u64, static_cast<size_t>(SendKind::Count)> send_ticks{};
    u64 macro_writes{};
    u64 macro_writes_sampled{};
    u64 macro_write_ticks{};

    // Kept.
    Sampler sampler;
    u32 current_macro{~0U};
};

inline State& GetState() {
    static State state;
    return state;
}

inline void LogWindow(State& state, u64 window_ns, double ns_per_tick);

/// The state, the previous window logged and its counters cleared once Nx2Profile began another
/// (Report, at the top of the GPU thread's loop).
inline State& Window() {
    State& state = GetState();
    const auto& data = Nx2Profile::t_data;
    if (state.window != data.window_start) [[unlikely]] {
        if (state.window != 0 && data.window_start > state.window &&
            data.window_start_ticks > state.window_ticks) {
            const u64 window_ns = data.window_start - state.window;
            LogWindow(state, window_ns,
                      static_cast<double>(window_ns) /
                          static_cast<double>(data.window_start_ticks - state.window_ticks));
        }
        state.macros.Clear();
        state.registers.Clear();
        state.drains = {};
        state.send_calls = {};
        state.send_sampled = {};
        state.send_ticks = {};
        state.macro_writes = 0;
        state.macro_writes_sampled = 0;
        state.macro_write_ticks = 0;
        state.window = data.window_start;
        state.window_ticks = data.window_start_ticks;
    }
    return state;
}

/// At the top of the GPU thread's loop after Nx2Profile::Report (On()): the window that ended
/// logged there, outside the zones.
YUZU_NO_INLINE inline void Report() {
    static_cast<void>(Window());
}

/// A drain counted as Nx2Profile::CountDrain does, its wait into the caller's histogram.
template <typename Split>
inline bool CountDrain(Split& split, Nx2Profile::DrainSite site) {
    if (!On()) {
        return Nx2Profile::CountDrain(split, site);
    }
    const u64 start = Nx2Profile::Ticks();
    const bool waited = Nx2Profile::CountDrain(split, site);
    const u64 ticks = Nx2Profile::Ticks() - start;
    Window().drains[static_cast<size_t>(site)].Note(waited, ticks);
    return waited;
}

/// A timed stretch of the front, nested profile zones out (their time goes to t_data.child_ticks
/// of the zone around it as they end).
struct Timer {
    u64 start{};
    u64 child{};
    bool sampled{};
};

inline Timer StartTimer(State& state, u32 bits) {
    Timer timer;
    if (state.sampler.Next(bits)) {
        timer.sampled = true;
        timer.child = Nx2Profile::t_data.child_ticks;
        timer.start = Nx2Profile::Ticks();
    }
    return timer;
}

/// Exclusive and nested ticks of a sampled timer.
inline std::pair<u64, u64> StopTimer(const Timer& timer) {
    const u64 elapsed = Nx2Profile::Ticks() - timer.start;
    const u64 child = Nx2Profile::t_data.child_ticks - timer.child;
    return {elapsed > child ? elapsed - child : 0, child};
}

/// A front macro call of the macro at `start` begins (On()).
YUZU_NO_INLINE inline Timer MacroBegin(u32 start) {
    State& state = Window();
    state.current_macro = start;
    return StartTimer(state, MacroSampleBits);
}

/// ... and ended; `hle`: FrontIndirectDraw took it.
YUZU_NO_INLINE inline void MacroEnd(const Timer& timer, u32 start, bool hle) {
    State& state = GetState(); // MacroBegin took the window
    MacroTable::Entry& entry = state.macros.Find(start);
    ++entry.calls;
    entry.hle_calls += hle ? 1 : 0;
    if (timer.sampled) {
        const auto [exclusive, child] = StopTimer(timer);
        ++entry.sampled;
        entry.ticks += exclusive;
        entry.child_ticks += child;
    }
    state.current_macro = ~0U;
}

/// Whom the DmaPusher method call being sent went to: DmaPusher sets it on its way (a plain
/// store whether timed or not; GPU thread).
inline std::atomic<SendKind> g_send_kind{SendKind::Split};

inline void SetSendKind(SendKind kind) {
    g_send_kind.store(kind, std::memory_order_relaxed);
}

/// A DmaPusher method call, `call`, timed for whom it went to (On()).
template <typename Call>
YUZU_NO_INLINE void TimedSend(const Call& call) {
    const Timer timer = StartTimer(Window(), SendSampleBits);
    SetSendKind(SendKind::Split);
    call();
    State& state = GetState(); // StartTimer took the window
    const auto index = static_cast<size_t>(g_send_kind.load(std::memory_order_relaxed));
    ++state.send_calls[index];
    if (timer.sampled) {
        ++state.send_sampled[index];
        state.send_ticks[index] += StopTimer(timer).first;
    }
}

/// A macro write FrontWrite sends, `write`, timed (On()).
template <typename Write>
YUZU_NO_INLINE void TimedMacroWrite(const Write& write) {
    const Timer timer = StartTimer(Window(), SendSampleBits);
    write();
    State& state = GetState(); // StartTimer took the window
    ++state.macro_writes;
    if (timer.sampled) {
        ++state.macro_writes_sampled;
        state.macro_write_ticks += StopTimer(timer).first;
    }
}

/// A 3D register write FrontWrite got: the value held already, not sent (On()).
inline void NoteRegisterWrite(u32 method, bool same, bool skipped) {
    Window().registers.Note(method, same, skipped);
}

/// EDEN_NX2_PROBE_FRONT_US=n (read once): microseconds the front spins per command list.
inline u32 ProbeFrontUs() {
    static const u32 us = [] {
        const char* value = std::getenv("EDEN_NX2_PROBE_FRONT_US");
        const u32 parsed =
            value != nullptr ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0U;
        if (parsed != 0) {
            LOG_INFO(HW_GPU,
                     "EDEN_NX2_PROBE_FRONT_US={}: the GPU thread's front spins {} us per command "
                     "list it pushes",
                     parsed, parsed);
        }
        return parsed;
    }();
    return us;
}

inline void SpinFor(u32 us) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds{us};
    while (std::chrono::steady_clock::now() < until) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }
}

inline void LogWindow(State& state, u64 window_ns, double ns_per_tick) {
    const double per_second = 1e9 / static_cast<double>(window_ns);
    const auto rate = [per_second](u64 value) { return static_cast<double>(value) * per_second; };
    const auto ms = [&](double ticks) { return ticks * ns_per_tick * per_second / 1e6; };

    // Macros.
    u64 calls = 0;
    u64 hle = 0;
    double ticks = 0;
    double child = 0;
    std::string top;
    for (const MacroTable::Entry* entry : state.macros.Top(~size_t{0})) {
        calls += entry->calls;
        hle += entry->hle_calls;
        ticks += entry->EstimatedTicks();
        if (entry->sampled != 0) {
            child += static_cast<double>(entry->child_ticks) * static_cast<double>(entry->calls) /
                     static_cast<double>(entry->sampled);
        }
    }
    for (const MacroTable::Entry* entry : state.macros.Top(16)) {
        const double per_call = entry->sampled != 0
                                    ? static_cast<double>(entry->ticks) * ns_per_tick /
                                          static_cast<double>(entry->sampled)
                                    : 0.0;
        const double nested = entry->sampled != 0
                                  ? static_cast<double>(entry->child_ticks) * ns_per_tick /
                                        static_cast<double>(entry->sampled)
                                  : 0.0;
        const char* kind = entry->hle_calls == 0               ? "J"
                           : entry->hle_calls == entry->calls ? "H"
                                                              : "JH";
        top += fmt::format("{}{:#x} {} {:.0f} x {:.0f} ns (nested {:.0f} ns) dma {:.0f}",
                           top.empty() ? "" : ", ", entry->start, kind, rate(entry->calls),
                           per_call, nested, rate(entry->dma_reads));
    }
    LOG_INFO(HW_GPU,
             "GPU thread front macros per second: calls {:.0f} ({} starts; JIT {:.0f}, HLE {:.0f}, "
             "interpreter 0) taking {:.1f} ms exclusive, {:.1f} ms nested (sampled 1/{}) | first "
             "16 (start kind calls x exclusive ns): {}",
             rate(calls), state.macros.Used(), rate(calls - hle), rate(hle), ms(ticks), ms(child),
             1U << MacroSampleBits, top.empty() ? "-" : top);

    // Sends.
    std::string sends;
    u64 send_calls = 0;
    double send_ticks = 0;
    for (size_t i = 0; i < state.send_calls.size(); ++i) {
        const double estimated =
            state.send_sampled[i] != 0 ? static_cast<double>(state.send_ticks[i]) *
                                             static_cast<double>(state.send_calls[i]) /
                                             static_cast<double>(state.send_sampled[i])
                                       : 0.0;
        send_calls += state.send_calls[i];
        send_ticks += estimated;
        sends += fmt::format("{}{} {:.0f} in {:.1f} ms", sends.empty() ? "" : ", ",
                             send_kind_names[i], rate(state.send_calls[i]), ms(estimated));
    }
    const double macro_write_ticks =
        state.macro_writes_sampled != 0
            ? static_cast<double>(state.macro_write_ticks) *
                  static_cast<double>(state.macro_writes) /
                  static_cast<double>(state.macro_writes_sampled)
            : 0.0;
    LOG_INFO(HW_GPU,
             "GPU thread front sends per second: method calls {:.0f} taking {:.1f} ms ({}) | macro "
             "writes {:.0f} taking {:.1f} ms (sampled 1/{}, nested zones out)",
             rate(send_calls), ms(send_ticks), sends, rate(state.macro_writes),
             ms(macro_write_ticks), 1U << SendSampleBits);

    // Register writes.
    const auto totals = state.registers.Totals();
    std::string same;
    for (const auto& row : state.registers.Top(16)) {
        same += fmt::format("{}{:#x} {:.0f}/{:.0f} ({:.0f} not sent)", same.empty() ? "" : ", ",
                            row.method, rate(row.same), rate(row.total), rate(row.skipped));
    }
    LOG_INFO(HW_GPU,
             "GPU thread front register writes per second: {:.0f} ({:.0f} same value, {:.0f} not "
             "sent) | same value by method (first 16, same/all): {}",
             rate(totals[0]), rate(totals[1]), rate(totals[2]), same.empty() ? "-" : same);

    // Drains.
    std::string drains;
    for (size_t site = 0; site < state.drains.size(); ++site) {
        const DrainHistogram& histogram = state.drains[site];
        if (histogram.Calls() == 0) {
            continue;
        }
        const auto bands = histogram.ToBands(ns_per_tick);
        std::string text;
        for (const u64 band : bands) {
            text += fmt::format("{}{:.0f}", text.empty() ? "" : "/", rate(band));
        }
        drains += fmt::format("{}{} {:.0f}: {}", drains.empty() ? "" : ", ",
                              Nx2Profile::drain_site_names[site], rate(histogram.Calls()), text);
    }
    LOG_INFO(HW_GPU,
             "GPU thread front drains per second (calls: waits under 2/8/32/128/512/2048/8192 us "
             "and over): {}",
             drains.empty() ? "-" : drains);
}

} // namespace VideoCommon::Nx2FrontDiag
