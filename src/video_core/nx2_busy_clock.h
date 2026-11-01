// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "common/cpu_features.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/gpu_split.h"

namespace VideoCommon::Nx2BusyClock {

// EDEN_NX2_GPU_BUSY_CLOCK=1 (off by default): the GPU timer (GPU::GetTicks: query and semaphore
// timestamps) runs only while the GPU pipeline works. Donkey Kong Bananza picks its 60 / 30 / 20
// fps mode from GPU timestamps; the wall clock gave it the frame period instead of our work (the
// pipeline waits for its commands, and while we hold its presents), and fast_gpu_time's /256 gave
// it next to nothing.
//
// The pipeline is idle while the GPU thread (the front) waits for the CPU's next command (profile
// zone "idle") and the split's back has run everything published to it (always so with the split
// off). The front publishes before it waits, and nothing else publishes, so the back catches up
// last or not at all. Not idle: the front draining the back or waiting for ring space (the back
// works), polling suspended channels ("pending sleep", a few ms a second in the profiles seen),
// the back waiting while the front parses. Host GPU time the threads don't wait for isn't counted.
//
// Each change of the two halves (front: before and after its wait; back: when it caught up while
// the front waits) re-evaluates both under a mutex and, if the result changed, moves `word`. The
// order of those stores and loads is Dekker's: the front stores its flag then reads `consumed`,
// the back stores `consumed` then (after a fence) reads the flag; one of them sees the other.
// Everything is in the wall clock's GPU ticks (CoreTiming::GetGPUTicks with multicore), so only
// with multicore: single core times the GPU from the emulated CPU's ticks.
//
// EDEN_NX2_PRESENT_DIAG keeps the busy time too (its per-second "gpu busy"), clock on or off.

/// Whether GPU::GetTicks uses the busy clock.
inline bool Enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("EDEN_NX2_GPU_BUSY_CLOCK");
        if (text == nullptr || text[0] == '0') {
            return false;
        }
        if (!Settings::values.use_multi_core.GetValue()) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_GPU_BUSY_CLOCK needs multicore: off");
            return false;
        }
        return true;
    }();
    return enabled;
}

/// Whether the idle time is kept (the clock, or EDEN_NX2_PRESENT_DIAG).
inline bool Tracked() {
    static const bool tracked = [] {
        const char* diag = std::getenv("EDEN_NX2_PRESENT_DIAG");
        return Enabled() || (diag != nullptr && diag[0] != '0' &&
                             Settings::values.use_multi_core.GetValue());
    }();
    return tracked;
}

constexpr u64 IdleBit = u64{1} << 63;

struct State {
    std::mutex mutex;
    /// IdleBit set: idle, the busy ticks the clock stopped at in the rest. Clear: the GPU ticks
    /// spent idle so far (busy ticks = now - word).
    std::atomic<u64> word{};
    /// The highest value Ticks returned: readers never see the clock go back.
    std::atomic<u64> highest{};
    /// The front waits for a command.
    std::atomic<bool> front_waiting{};
};
inline State g_state;

/// The same ticks as CoreTiming::GetGPUTicks with multicore.
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetGPUTick());
}

/// Moves the clock to the pipeline's current state (after either half changed).
inline void Update() {
    std::scoped_lock lock{g_state.mutex};
    const bool idle = g_state.front_waiting.load(std::memory_order_seq_cst) &&
                      GpuSplit::Get().BackCaughtUp();
    const u64 word = g_state.word.load(std::memory_order_relaxed);
    if (idle == ((word & IdleBit) != 0)) {
        return;
    }
    const u64 now = Now();
    if (idle) {
        const u64 busy = now > word ? now - word : 0;
        g_state.word.store(IdleBit | busy, std::memory_order_release);
    } else {
        const u64 busy = word & ~IdleBit;
        g_state.word.store(now > busy ? now - busy : 0, std::memory_order_release);
    }
}

/// Front (GPU thread): around its wait for the CPU's next command.
inline void FrontWaits(bool waiting) {
    g_state.front_waiting.store(waiting, std::memory_order_seq_cst);
    Update();
}

/// Back: after it stored `consumed`.
inline void BackStored() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (g_state.front_waiting.load(std::memory_order_seq_cst)) {
        Update();
    }
}

/// The busy GPU ticks so far (any thread). Never less than a value returned before.
inline u64 Ticks() {
    u64 word = g_state.word.load(std::memory_order_acquire);
    u64 value;
    for (;;) {
        if ((word & IdleBit) != 0) {
            value = word & ~IdleBit;
            break;
        }
        const u64 now = Now();
        const u64 again = g_state.word.load(std::memory_order_acquire);
        if (again == word) {
            value = now > word ? now - word : 0;
            break;
        }
        word = again;
    }
    // A reader that took `word` just before the pipeline went idle and the time just after the
    // updater did may return more than the value the clock stopped at.
    u64 highest = g_state.highest.load(std::memory_order_relaxed);
    while (value > highest &&
           !g_state.highest.compare_exchange_weak(highest, value, std::memory_order_relaxed)) {
    }
    return (std::max)(value, highest);
}

} // namespace VideoCommon::Nx2BusyClock
