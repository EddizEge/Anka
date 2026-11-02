// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_BACK_CPU: a physical core of its own for the split GPU thread's back (gpu_split.h).
namespace VideoCommon::Nx2BackCpu {

// The back is the one thread a frame waits on (~98% busy in the wide areas of title 4). Windows
// runs it on either logical processor of any core (SetCurrentThreadToPerformanceCores does nothing
// there), and the other logical processor of that core may run another busy thread of the process
// meanwhile (the front spinning in Drain, HostTiming, a guest CPU core): the two then share one
// core's execution units. Unset or 0 (the default until an A/B shows a gain): as before, nothing
// is called. 1: the back runs on one core (both of its logical processors, CPU Sets) and the
// process's other threads are kept off that core (the process's default CPU Sets; threads of
// other processes may still use it). EDEN_NX2_BACK_CORE=<n> picks the core by Windows' CoreIndex,
// as the log line names it (default: the second core of processor group 0, away from core 0's
// interrupts). 2: pinned, and where the back and the front run is logged (verify). 3: not pinned,
// logged (where Windows puts them). Read at the first call (the back's StartBack or the front's
// NoteFront), as the other split switches are, so a front end that sets it in main() is seen.
inline int Mode() {
    static std::atomic<s8> mode{-1};
    s8 value = mode.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        const char* env = std::getenv("EDEN_NX2_BACK_CPU");
        value = static_cast<s8>(env == nullptr  ? 0
                                : env[0] == '1' ? 1
                                : env[0] == '2' ? 2
                                : env[0] == '3' ? 3
                                                : 0);
        mode.store(value, std::memory_order_relaxed);
    }
    return value;
}

/// Whether the back and the front note where they run (EDEN_NX2_BACK_CPU=2 or 3).
inline bool Logging() {
    return Mode() >= 2;
}

#ifdef _WIN32

struct CpuSet {
    ULONG id;
    WORD group;
    BYTE logical;
    BYTE core;
};

/// The system's CPU Sets (one per logical processor).
inline std::vector<CpuSet> ReadCpuSets() {
    std::vector<CpuSet> sets;
    const HANDLE process = GetCurrentProcess();
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, process, 0);
    if (length == 0) {
        return sets;
    }
    std::vector<u8> buffer(length);
    if (!GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
                                    length, &length, process, 0)) {
        return sets;
    }
    for (ULONG offset = 0; offset + 2 * sizeof(DWORD) <= length;) {
        const auto* const info =
            reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (info->Size == 0) {
            break;
        }
        if (info->Type == CpuSetInformation) {
            sets.push_back(CpuSet{
                .id = info->CpuSet.Id,
                .group = info->CpuSet.Group,
                .logical = info->CpuSet.LogicalProcessorIndex,
                .core = info->CpuSet.CoreIndex,
            });
        }
        offset += info->Size;
    }
    return sets;
}

/// Logical processor of group 0 -> its core (CoreIndex), for the log; 0xff: unknown.
inline std::array<u8, 64> g_core_of = [] {
    std::array<u8, 64> cores;
    cores.fill(0xff);
    return cores;
}();
/// Where the front last waited for the back (GetCurrentProcessorNumber), for the log.
inline std::atomic<u32> g_front_cpu{~0U};
/// Set by the back once pinned; read by the front after the back thread ended (Stop).
inline bool g_pinned{};
/// Back stages (StartFront): the core (CoreIndex, group 0) the front has to itself, 0xff: none;
/// set before the back (stage B) starts and pins, which keeps the process's threads off it too.
inline u8 g_front_core{0xff};
inline DWORD g_front_thread{};

struct Placement {
    std::array<u32, 64> back{};
    std::array<u32, 64> front{};
    u32 samples{};
    u32 shared{};
    u32 moves{};
    u32 last{~0U};
};
inline Placement g_placement; ///< Back thread only

/// Back thread, before it runs anything: pins it (modes 1 and 2), reads the cores for the log.
inline void StartBack() {
    if (Mode() == 0) {
        return;
    }
    const std::vector<CpuSet> sets = ReadCpuSets();
    for (const CpuSet& set : sets) {
        if (set.group == 0 && size_t{set.logical} < g_core_of.size()) {
            g_core_of[set.logical] = set.core;
        }
    }
    if (Mode() == 3) {
        LOG_INFO(HW_GPU, "GPU thread split: the back runs where Windows puts it, placement logged "
                         "(EDEN_NX2_BACK_CPU=3)");
        return;
    }
    std::vector<BYTE> cores;
    for (const CpuSet& set : sets) {
        if (set.group == 0 && std::find(cores.begin(), cores.end(), set.core) == cores.end()) {
            cores.push_back(set.core);
        }
    }
    std::sort(cores.begin(), cores.end());
    if (cores.size() < 4) {
        LOG_INFO(HW_GPU, "GPU thread split: the back is not pinned ({} cores)", cores.size());
        return;
    }
    BYTE core = cores[1];
    if (const char* value = std::getenv("EDEN_NX2_BACK_CORE"); value != nullptr) {
        char* end = nullptr;
        const unsigned long wanted = std::strtoul(value, &end, 10);
        auto it = cores.end();
        if (end != value) {
            it = std::find_if(cores.begin(), cores.end(), [wanted](BYTE index) {
                return static_cast<unsigned long>(index) == wanted;
            });
        }
        if (it != cores.end()) {
            core = *it;
        } else {
            LOG_WARNING(HW_GPU, "EDEN_NX2_BACK_CORE={}: no such core, the back takes core {}",
                        value, u32{core});
        }
    }
    std::vector<ULONG> back_ids;
    std::vector<ULONG> other_ids;
    std::string logicals;
    for (const CpuSet& set : sets) {
        if (set.group == 0 && set.core == core) {
            back_ids.push_back(set.id);
            if (!logicals.empty()) {
                logicals += ' ';
            }
            logicals += std::to_string(u32{set.logical});
        } else if (set.group != 0 || set.core != g_front_core) {
            other_ids.push_back(set.id);
        }
    }
    if (!SetThreadSelectedCpuSets(GetCurrentThread(), back_ids.data(),
                                  static_cast<ULONG>(back_ids.size()))) {
        LOG_WARNING(HW_GPU, "GPU thread split: the back's CPU Sets were refused (error {})",
                    GetLastError());
        return;
    }
    if (!SetProcessDefaultCpuSets(GetCurrentProcess(), other_ids.data(),
                                  static_cast<ULONG>(other_ids.size()))) {
        LOG_WARNING(HW_GPU, "GPU thread split: the process's CPU Sets were refused (error {})",
                    GetLastError());
        SetThreadSelectedCpuSets(GetCurrentThread(), nullptr, 0);
        return;
    }
    g_pinned = true;
    LOG_INFO(HW_GPU,
             "GPU thread split: the back runs on core {} (logical processors {}), the process's "
             "other threads on the other {} (EDEN_NX2_BACK_CPU=0: anywhere)",
             u32{core}, logicals, other_ids.size());
}

/// Back stages (EDEN_NX2_BACK_STAGES), the front, before stage B starts (Nx2Stages::Start),
/// modes 1 and 2: the front (the GPU thread, which waits for the stages in Drain) runs on a core
/// of its own as well (S19), the next one after the back's (EDEN_NX2_FRONT_CORE=<n> picks it by
/// CoreIndex); StartBack then keeps the process's other threads off both.
inline void StartFront() {
    if (Mode() != 1 && Mode() != 2) {
        return;
    }
    const std::vector<CpuSet> sets = ReadCpuSets();
    std::vector<BYTE> cores;
    for (const CpuSet& set : sets) {
        if (set.group == 0 && std::find(cores.begin(), cores.end(), set.core) == cores.end()) {
            cores.push_back(set.core);
        }
    }
    std::sort(cores.begin(), cores.end());
    if (cores.size() < 6) {
        LOG_INFO(HW_GPU, "GPU back stages: the front is not pinned ({} cores)", cores.size());
        return;
    }
    // The back's core as StartBack picks it.
    BYTE back = cores[1];
    if (const char* value = std::getenv("EDEN_NX2_BACK_CORE"); value != nullptr) {
        char* end = nullptr;
        const unsigned long wanted = std::strtoul(value, &end, 10);
        if (end != value &&
            std::find(cores.begin(), cores.end(), static_cast<BYTE>(wanted)) != cores.end()) {
            back = static_cast<BYTE>(wanted);
        }
    }
    BYTE front = cores[1] != back ? cores[1] : cores[2];
    if (const char* value = std::getenv("EDEN_NX2_FRONT_CORE"); value != nullptr) {
        char* end = nullptr;
        const unsigned long wanted = std::strtoul(value, &end, 10);
        if (end != value && wanted != back &&
            std::find(cores.begin(), cores.end(), static_cast<BYTE>(wanted)) != cores.end()) {
            front = static_cast<BYTE>(wanted);
        } else {
            LOG_WARNING(HW_GPU,
                        "EDEN_NX2_FRONT_CORE={}: no such core or the back's, the front "
                        "takes core {}",
                        value, u32{front});
        }
    }
    std::vector<ULONG> front_ids;
    for (const CpuSet& set : sets) {
        if (set.group == 0 && set.core == front) {
            front_ids.push_back(set.id);
        }
    }
    if (!SetThreadSelectedCpuSets(GetCurrentThread(), front_ids.data(),
                                  static_cast<ULONG>(front_ids.size()))) {
        LOG_WARNING(HW_GPU, "GPU back stages: the front's CPU Sets were refused (error {})",
                    GetLastError());
        return;
    }
    g_front_core = front;
    g_front_thread = GetCurrentThreadId();
    LOG_INFO(HW_GPU, "GPU back stages: the front runs on core {}", u32{front});
}

/// Front, once the back thread ended (GpuSplit::Stop): every core is the process's again.
inline void StopBack() {
    if (g_pinned) {
        SetProcessDefaultCpuSets(GetCurrentProcess(), nullptr, 0);
        g_pinned = false;
    }
    if (g_front_core != 0xff) {
        // At shutdown the front thread already ended.
        if (GetCurrentThreadId() == g_front_thread) {
            SetThreadSelectedCpuSets(GetCurrentThread(), nullptr, 0);
        }
        g_front_core = 0xff;
    }
}

/// Front, when it starts waiting for the back (Drain, WaitForSpace).
inline void NoteFront() {
    if (Logging()) [[unlikely]] {
        g_front_cpu.store(static_cast<u32>(GetCurrentProcessorNumber()),
                          std::memory_order_relaxed);
    }
}

/// Back, at each wake and every 4096 words it runs: where it runs, where the front last waited,
/// and every 32768 samples (~10 s) a log line.
inline void NoteBack() {
    if (!Logging()) [[likely]] {
        return;
    }
    Placement& placement = g_placement;
    const u32 cpu = static_cast<u32>(GetCurrentProcessorNumber());
    const u32 front = g_front_cpu.load(std::memory_order_relaxed);
    if (cpu < placement.back.size()) {
        ++placement.back[cpu];
    }
    if (front < placement.front.size()) {
        ++placement.front[front];
        if (cpu < g_core_of.size() && g_core_of[cpu] != 0xff &&
            g_core_of[cpu] == g_core_of[front]) {
            ++placement.shared;
        }
    }
    if (placement.last != ~0U && placement.last != cpu) {
        ++placement.moves;
    }
    placement.last = cpu;
    if (++placement.samples < 32768) {
        return;
    }
    const double samples = static_cast<double>(placement.samples);
    const auto list = [samples](const std::array<u32, 64>& counts) {
        std::string text;
        for (size_t index = 0; index < counts.size(); ++index) {
            if (counts[index] != 0) {
                if (!text.empty()) {
                    text += ", ";
                }
                text += fmt::format("{}: {:.0f}%", index, 100.0 * counts[index] / samples);
            }
        }
        return text.empty() ? std::string("-") : text;
    };
    LOG_INFO(HW_GPU,
             "GPU thread split placement ({} samples{}): back on logical processors {} ({} moves); "
             "front last waiting on {}; back on the front's core {:.1f}% of samples",
             placement.samples, g_pinned ? ", back pinned" : "", list(placement.back),
             placement.moves, list(placement.front), 100.0 * placement.shared / samples);
    placement = {};
}

#else

inline void StartBack() {}
inline void StartFront() {}
inline void StopBack() {}
inline void NoteFront() {}
inline void NoteBack() {}

#endif

} // namespace VideoCommon::Nx2BackCpu
