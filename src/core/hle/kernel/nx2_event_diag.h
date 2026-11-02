// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "core/arm/debug.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"

// EDEN_NX2_VI2_DIAG: which guest threads poll (wait with a zero timeout), wait on and reset the
// events nx2:vi2 hands out. nx2_gsv.h tracks its events here and reports the calls with its 10 s
// event summary; svcWaitSynchronization, svcResetSignal and svcClearEvent record the calls on
// tracked objects. Nothing is tracked otherwise, and those SVCs only read one atomic.
namespace Kernel::Nx2EventDiag {

constexpr std::size_t MaxEvents = 16;
constexpr std::size_t MaxThreads = 4; // per event; calls from more threads are only counted

enum class Op : u8 {
    Poll,  // svcWaitSynchronization with a zero timeout
    Wait,  // with a timeout or none
    Reset, // svcResetSignal
    Clear, // svcClearEvent
};

struct ThreadCalls {
    u64 id{};
    std::string name;
    u32 polls{}, poll_hits{};   // hits: the call returned this object signalled
    u32 waits{}, wait_hits{};
    u32 resets{}, reset_hits{}; // hits: it was signalled (the reset cleared it)
    u32 clears{};
};

struct Registry {
    std::atomic<u32> slots{}; // slots ever used
    std::array<std::atomic<const KSynchronizationObject*>, MaxEvents> objects{};
    std::mutex mutex;
    std::array<std::vector<ThreadCalls>, MaxEvents> calls{}; // under mutex
    std::array<u32, MaxEvents> other_calls{};                // under mutex
};

inline Registry& Get() {
    static Registry registry;
    return registry;
}

// False until an event was ever tracked: the SVCs' only cost.
inline bool Any() {
    return Get().slots.load(std::memory_order_acquire) != 0;
}

// The slot tracking `object`, -1 when none.
inline int Find(const KSynchronizationObject* object) {
    auto& registry = Get();
    const u32 slots = registry.slots.load(std::memory_order_acquire);
    for (u32 slot = 0; slot < slots; ++slot) {
        if (registry.objects[slot].load(std::memory_order_relaxed) == object) {
            return static_cast<int>(slot);
        }
    }
    return -1;
}

// Tracks an event's readable end; -1 when all slots are taken.
inline int Track(const KSynchronizationObject* object) {
    auto& registry = Get();
    std::scoped_lock lk{registry.mutex};
    u32 slots = registry.slots.load(std::memory_order_relaxed);
    u32 slot = 0;
    while (slot < slots && registry.objects[slot].load(std::memory_order_relaxed) != nullptr) {
        ++slot;
    }
    if (slot == MaxEvents) {
        return -1;
    }
    registry.calls[slot].clear();
    registry.other_calls[slot] = 0;
    registry.objects[slot].store(object, std::memory_order_release);
    if (slot == slots) {
        registry.slots.store(slots + 1, std::memory_order_release);
    }
    return static_cast<int>(slot);
}

// Before the object is closed.
inline void Untrack(int slot) {
    if (slot < 0 || static_cast<std::size_t>(slot) >= MaxEvents) {
        return;
    }
    auto& registry = Get();
    std::scoped_lock lk{registry.mutex};
    registry.objects[slot].store(nullptr, std::memory_order_release);
}

// A call by `thread` (the current guest thread) on the tracked slot.
inline void Record(int slot, const KThread& thread, Op op, bool hit) {
    auto& registry = Get();
    const u64 id = thread.GetThreadId();
    const auto add = [&](ThreadCalls& calls) {
        switch (op) {
        case Op::Poll:
            ++calls.polls;
            calls.poll_hits += hit ? 1 : 0;
            break;
        case Op::Wait:
            ++calls.waits;
            calls.wait_hits += hit ? 1 : 0;
            break;
        case Op::Reset:
            ++calls.resets;
            calls.reset_hits += hit ? 1 : 0;
            break;
        case Op::Clear:
            ++calls.clears;
            break;
        }
    };
    {
        std::scoped_lock lk{registry.mutex};
        auto& list = registry.calls[slot];
        for (auto& calls : list) {
            if (calls.id == id) {
                add(calls);
                return;
            }
        }
        if (list.size() >= MaxThreads) {
            ++registry.other_calls[slot];
            return;
        }
    }
    // A thread not seen on this slot: its name from guest memory, outside the lock.
    ThreadCalls calls{.id = id, .name = Core::GetThreadName(&thread).value_or("?")};
    add(calls);
    std::scoped_lock lk{registry.mutex};
    auto& list = registry.calls[slot];
    for (auto& known : list) {
        if (known.id == id) { // recorded meanwhile by another call of the same thread? merge
            known.polls += calls.polls;
            known.poll_hits += calls.poll_hits;
            known.waits += calls.waits;
            known.wait_hits += calls.wait_hits;
            known.resets += calls.resets;
            known.reset_hits += calls.reset_hits;
            known.clears += calls.clears;
            return;
        }
    }
    if (list.size() >= MaxThreads) {
        ++registry.other_calls[slot];
        return;
    }
    list.push_back(std::move(calls));
}

// The calls since the previous Take ("MainThread 0x55 polled 600 (590 signalled), ..."), and
// zeroes them (the threads stay known). "none" when there were none.
inline std::string Take(int slot) {
    if (slot < 0 || static_cast<std::size_t>(slot) >= MaxEvents) {
        return "not tracked";
    }
    auto& registry = Get();
    std::scoped_lock lk{registry.mutex};
    std::string text;
    for (auto& calls : registry.calls[slot]) {
        std::string what;
        const auto part = [&what](std::string item) {
            what += (what.empty() ? "" : ", ") + item;
        };
        if (calls.polls != 0) {
            part(fmt::format("polled {} ({} signalled)", calls.polls, calls.poll_hits));
        }
        if (calls.waits != 0) {
            part(fmt::format("waited {} ({} ended by it)", calls.waits, calls.wait_hits));
        }
        if (calls.resets != 0) {
            part(fmt::format("reset {} ({} signalled)", calls.resets, calls.reset_hits));
        }
        if (calls.clears != 0) {
            part(fmt::format("cleared {}", calls.clears));
        }
        if (!what.empty()) {
            text += fmt::format("{}{} {:#x} {}", text.empty() ? "" : "; ", calls.name, calls.id, what);
        }
        calls = ThreadCalls{.id = calls.id, .name = std::move(calls.name)};
    }
    if (registry.other_calls[slot] != 0) {
        text += fmt::format("{}{} calls from other threads", text.empty() ? "" : "; ",
                            registry.other_calls[slot]);
        registry.other_calls[slot] = 0;
    }
    return text.empty() ? "none" : text;
}

} // namespace Kernel::Nx2EventDiag
