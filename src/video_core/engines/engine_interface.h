// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <bitset>
#include <limits>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Tegra::Engines {

enum class EngineTypes : u32 {
    Nv01Timer,
    KeplerCompute,
    Maxwell3D,
    Fermi2D,
    MaxwellDMA,
    KeplerMemory,
};

class EngineInterface {
public:
    virtual ~EngineInterface() = default;

    /// Write the value to the register identified by method.
    virtual void CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) = 0;

    /// Write multiple values to the register identified by method.
    virtual void CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) = 0;

    /// What CallMethod(system, methods[i], arguments[i], true) does for each i, in order: macro
    /// writes to methods in [method_run_begin, method_run_begin + method_run_count) that follow
    /// each other in the split GPU thread's ring (GpuSplit::Execute), at most MaxMethodRun.
    virtual void CallMethodRun(Core::System& system, const u32* methods, const u32* arguments,
                               u32 count) {
        for (u32 i = 0; i < count; ++i) {
            CallMethod(system, methods[i], arguments[i], true);
        }
    }

    /// The macro writes of a split GPU thread Batch entry (each bits 0-15 method, 32-63
    /// argument), after its ConsumeSink: CallMethod(system, method, argument, true) for each, in
    /// order, runs of the run methods through CallMethodRun as from consecutive entries
    /// (GpuSplit::Execute, EDEN_NX2_BATCH_DIRECT).
    virtual void CallMethodBatch(Core::System& system, const u64* writes, u32 count) {
        ForEachBatchWrite(
            writes, count,
            [&](u32 method, u32 argument) { CallMethod(system, method, argument, true); },
            [&](const u32* methods, const u32* arguments, u32 run) {
                CallMethodRun(system, methods, arguments, run);
            });
    }

    void ConsumeSink(Core::System& system) {
        if (!method_sink.empty()) {
            ConsumeSinkImpl(system);
        }
    }

    std::bitset<(std::numeric_limits<u16>::max)()> execution_mask{};
    std::vector<std::pair<u32, u32>> method_sink{};
    GPUVAddr current_dma_segment;
    bool current_dirty{};
    /// Methods whose consecutive macro writes go to CallMethodRun together (none by default).
    static constexpr u32 MaxMethodRun = 32;
    u32 method_run_begin{};
    u32 method_run_count{};
protected:
    /// CallMethodBatch's loop: `call(method, argument)` for a write, `call_run(methods,
    /// arguments, count)` for a run of the run methods (at most MaxMethodRun).
    template <typename Call, typename CallRun>
    void ForEachBatchWrite(const u64* writes, u32 count, Call&& call, CallRun&& call_run) const {
        for (u32 i = 0; i < count;) {
            const u32 method = static_cast<u32>(writes[i] & 0xffff);
            const u32 argument = static_cast<u32>(writes[i] >> 32);
            if (method - method_run_begin < method_run_count) {
                u32 methods[MaxMethodRun];
                u32 arguments[MaxMethodRun];
                u32 run = 0;
                for (; run < MaxMethodRun && i + run < count; ++run) {
                    const u32 next = static_cast<u32>(writes[i + run] & 0xffff);
                    if (next - method_run_begin >= method_run_count) {
                        break;
                    }
                    methods[run] = next;
                    arguments[run] = static_cast<u32>(writes[i + run] >> 32);
                }
                if (run > 1) {
                    call_run(methods, arguments, run);
                    i += run;
                    continue;
                }
            }
            call(method, argument);
            ++i;
        }
    }

    virtual void ConsumeSinkImpl(Core::System& system) {
        for (auto [method, value] : method_sink) {
            CallMethod(system, method, value, true);
        }
        method_sink.clear();
    }
};

} // namespace Tegra::Engines
