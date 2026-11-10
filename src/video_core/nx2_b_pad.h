// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/nx2_ab.h"

#ifdef ARCHITECTURE_x86_64
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

// EDEN_NX2_B_PAD_NS=<n> (off by default): every draw the rasterizer sets up (PrepareDraw: K1's
// stage B, or the split's back) first spins n nanoseconds on the time stamp counter. A virtual
// slowdown of that thread alone: if the frame rate falls about as much as the thread's busy time
// grows, the thread is critical and work taken off it pays; if it hardly moves, it does not.
// =ab:<n>: no pad in the A/B test's phase A, n ns in B (common/nx2_ab.h; a positive control for
// the A/B method). The rate is measured at the first draw whatever the phase; a pad holds no
// state, so each draw takes the phase on its own.
namespace VideoCommon::Nx2BPad {

/// Pad length in time stamp counter ticks: -1 not read yet, 0 off.
inline std::atomic<s64> g_pad_ticks{-1};
/// =ab:<n>: the pad follows the A/B phase (stored before g_pad_ticks).
inline std::atomic<bool> g_pad_follows{false};

YUZU_NO_INLINE inline s64 ReadPadTicks() {
    const char* value = std::getenv("EDEN_NX2_B_PAD_NS");
    const u64 fixed = value != nullptr ? std::strtoull(value, nullptr, 10) : 0;
    const auto pad = Common::Nx2Ab::Switch::Parse(
        value, static_cast<u32>(fixed < 0xffffffffULL ? fixed : 0xffffffffULL));
    Common::Nx2Ab::LogBinding("EDEN_NX2_B_PAD_NS", pad);
    const u64 ns = pad.At(Common::Nx2Ab::Phase::B); // the on value
    s64 ticks = 0;
#ifdef ARCHITECTURE_x86_64
    if (ns != 0) {
        // The counter's rate against the steady clock over 20 ms (once, at the first draw).
        const auto t0 = std::chrono::steady_clock::now();
        const u64 c0 = __rdtsc();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(20)) {
        }
        const auto t1 = std::chrono::steady_clock::now();
        const u64 c1 = __rdtsc();
        const double elapsed_ns =
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        const double ticks_per_ns = static_cast<double>(c1 - c0) / elapsed_ns;
        ticks = static_cast<s64>(static_cast<double>(ns) * ticks_per_ns + 0.5);
        LOG_INFO(HW_GPU, "EDEN_NX2_B_PAD_NS={}: each draw spins {} ticks ({:.3f} ticks/ns)", ns,
                 ticks, ticks_per_ns);
    }
#else
    if (ns != 0) {
        LOG_WARNING(HW_GPU, "EDEN_NX2_B_PAD_NS needs the time stamp counter (x86-64): off");
    }
#endif
    g_pad_follows.store(pad.FollowsPhase(), std::memory_order_relaxed);
    g_pad_ticks.store(ticks, std::memory_order_release);
    return ticks;
}

/// At the start of each draw's setup.
inline void Pad() {
    s64 ticks = g_pad_ticks.load(std::memory_order_acquire);
    if (ticks < 0) [[unlikely]] {
        ticks = ReadPadTicks();
    }
    if (ticks == 0) [[likely]] {
        return;
    }
    if (g_pad_follows.load(std::memory_order_relaxed) &&
        Common::Nx2Ab::CurrentPhase() == Common::Nx2Ab::Phase::A) {
        return;
    }
#ifdef ARCHITECTURE_x86_64
    const u64 end = __rdtsc() + static_cast<u64>(ticks);
    while (__rdtsc() < end) {
        _mm_pause();
    }
#endif
}

} // namespace VideoCommon::Nx2BPad
