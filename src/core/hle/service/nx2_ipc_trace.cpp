// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstdlib>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/demangle.h"
#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/arm/symbols.h"
#include "core/core.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/nx2_ipc_trace.h"
#include "core/memory.h"

namespace Service::Nx2IpcTrace {
namespace {

const std::optional<Filter>& GetFilter() {
    static const std::optional<Filter> filter = []() -> std::optional<Filter> {
        const char* text = std::getenv("EDEN_NX2_IPC_TRACE");
        if (text == nullptr || std::string_view{text} == "1") {
            return std::nullopt;
        }
        auto parsed = ParseFilter(text);
        if (!parsed) {
            LOG_WARNING(Service, "EDEN_NX2_IPC_TRACE=\"{}\" has no service: off", text);
            return std::nullopt;
        }
        std::string items;
        for (const auto& item : parsed->items) {
            items += fmt::format(" \"{}\"", item.prefix.empty() ? "all" : item.prefix);
            if (!item.commands.empty()) {
                items += fmt::format(":{}", fmt::join(item.commands, ","));
            }
        }
        LOG_INFO(Service,
                 "EDEN_NX2_IPC_TRACE: backtrace of the first {} new requests of{} (each new "
                 "service, command and data words)",
                 MaxTraced, items);
        return parsed;
    }();
    return filter;
}

struct State {
    std::mutex mutex;
    std::set<std::tuple<std::string, u32, u32, std::vector<u32>>> seen;
    u32 traced{};
    bool limit_logged{};
    // Symbols of the modules last seen, by base address; refilled when the module list changes.
    u64 process_id{~0ULL};
    Loader::AppLoader::Modules modules;
    std::map<VAddr, VAddr> ends; // module base -> one past its last byte
    std::map<VAddr, Core::Symbols::Symbols> symbols;
};

State& GetState() {
    static State state;
    return state;
}

// The guest thread that sent a request waits in the kernel while a service host thread handles
// it. Its registers are in the KThread context once the scheduler has switched it out: Unload
// (SaveContext) runs before the core's current thread changes. The request can reach the service
// thread a little earlier (it is queued under the scheduler lock, the switch follows its release),
// so wait until no core runs the thread any more. False: still on a core after 20 ms.
bool WaitUntilSwitchedOut(Kernel::KernelCore& kernel, const Kernel::KThread& thread) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{20};
    while (true) {
        bool running = false;
        for (std::size_t core = 0; core < Core::Hardware::NUM_CPU_CORES; ++core) {
            running |= kernel.Scheduler(core).GetSchedulerCurrentThread() == &thread;
        }
        if (!running) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
}

// Return addresses through the frame records (AArch64: x29 -> {previous x29, x30}; AArch32:
// r11 -> {previous r11, lr}). Every record is checked to be mapped and above the previous one.
std::vector<u64> WalkFrames(Kernel::KProcess& process, const Kernel::Svc::ThreadContext& context) {
    auto& memory = process.GetMemory();
    const bool is_64 = process.Is64Bit();
    const u64 word = is_64 ? 8 : 4;
    std::vector<u64> frames{context.pc, context.lr};
    u64 fp = context.fp;
    while (frames.size() < MaxFrames) {
        if (fp == 0 || (fp % word) != 0 || !memory.IsValidVirtualAddressRange(fp, 2 * word)) {
            break;
        }
        const u64 lr = is_64 ? memory.Read64(fp + 8) : memory.Read32(fp + 4);
        const u64 next = is_64 ? memory.Read64(fp) : memory.Read32(fp);
        if (lr == 0) {
            break;
        }
        frames.push_back(lr);
        if (next <= fp) {
            break;
        }
        fp = next;
    }
    return frames;
}

// The loader's end for its modules (Switch 2 modules have no path, so only it knows them), else
// the end of the .text, .rodata, .data regions that follow the base.
VAddr ModuleEnd(const Kernel::KProcess& process, VAddr base) {
    for (const auto& loaded : process.GetLoadedModules()) {
        if (loaded.base == base) {
            return loaded.end;
        }
    }
    return GetInteger(Core::GetModuleEnd(&process, base)) + 1;
}

std::string DescribeFrame(State& state, u32 index, u64 address) {
    auto module = state.modules.upper_bound(address);
    if (module == state.modules.begin()) {
        return fmt::format("  #{} {:#x} (no module)", index, address);
    }
    --module;
    const auto end = state.ends.find(module->first);
    if (end != state.ends.end() && address >= end->second) {
        return fmt::format("  #{} {:#x} (no module)", index, address);
    }
    const u64 offset = address - module->first;
    const auto symbols = state.symbols.find(module->first);
    const auto name = symbols == state.symbols.end()
                          ? std::nullopt
                          : Core::Symbols::GetSymbolName(symbols->second, offset);
    if (!name) {
        return fmt::format("  #{} {} +{:#x} (no symbol)", index, module->second, offset);
    }
    const auto start = symbols->second.find(*name);
    const u64 into = start == symbols->second.end() ? 0 : offset - start->second.first;
    const std::string demangled = Common::DemangleSymbol(*name);
    if (demangled == *name) {
        return fmt::format("  #{} {} +{:#x} {}+{:#x}", index, module->second, offset, *name, into);
    }
    return fmt::format("  #{} {} +{:#x} {}+{:#x} = {}", index, module->second, offset, *name, into,
                       demangled);
}

void Trace(Core::System& system, State& state, std::string_view service, const char* handler,
           HLERequestContext& ctx, const std::vector<u32>& words) {
    Kernel::KThread& thread = ctx.GetThread();
    Kernel::KProcess* const process = thread.GetOwnerProcess();
    std::string text = fmt::format("nx2 ipc trace {}/{}: {} {} [{}] \"{}\", {} data words:", state.traced,
                                   MaxTraced, service, ctx.GetCommand(),
                                   ctx.IsTipc() ? "tipc" : "cmif", handler == nullptr ? "unregistered" : handler,
                                   words.size());
    for (const u32 w : words) {
        text += fmt::format(" {:#x}", w);
    }
    if (thread.IsDummyThread() || process == nullptr) {
        LOG_INFO(Service, "{}; from host thread {:#x} (no guest backtrace)", text, thread.GetThreadId());
        return;
    }
    const bool switched_out = WaitUntilSwitchedOut(system.Kernel(), thread);
    const Kernel::Svc::ThreadContext context = thread.GetContext();
    const auto name = Core::GetThreadName(&thread);
    text += fmt::format("; thread {:#x} \"{}\"{}", thread.GetThreadId(), name.value_or("?"),
                        switched_out ? "" : " (still on a core after 20 ms: registers may be stale)");

    auto modules = Core::FindModules(process);
    if (state.process_id != process->GetProcessId() || modules != state.modules) {
        state.process_id = process->GetProcessId();
        state.modules = std::move(modules);
        state.symbols.clear();
        state.ends.clear();
        for (const auto& [base, module_name] : state.modules) {
            state.ends.emplace(base, ModuleEnd(*process, base));
            state.symbols.emplace(base, Core::Symbols::GetSymbols(base, process->GetMemory(), process->Is64Bit()));
        }
    }
    const auto frames = WalkFrames(*process, context);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        text += '\n';
        text += DescribeFrame(state, static_cast<u32>(i), frames[i]);
    }
    LOG_INFO(Service, "{}", text);
}

} // Anonymous namespace

bool Enabled() {
    return GetFilter().has_value();
}

void OnRequest(Core::System& system, std::string_view service, const char* handler,
               HLERequestContext& ctx) {
    const auto& filter = GetFilter();
    if (!filter || !filter->Matches(service, ctx.GetCommand())) {
        return;
    }
    State& state = GetState();
    try {
        const auto* buffer = ctx.CommandBuffer();
        const auto words = RequestDataWords(std::span<const u32>{buffer, IPC::COMMAND_BUFFER_LENGTH},
                                            ctx.IsTipc(), ctx.GetDataPayloadOffset(),
                                            ctx.HasDomainMessageHeader());
        std::scoped_lock lk{state.mutex};
        auto key = std::make_tuple(std::string{service}, static_cast<u32>(ctx.GetCommandType()),
                                   ctx.GetCommand(), words);
        if (state.seen.contains(key)) {
            return;
        }
        if (state.traced >= MaxTraced) {
            if (!state.limit_logged) {
                state.limit_logged = true;
                LOG_INFO(Service, "nx2 ipc trace limit reached ({} requests): new requests are no longer traced",
                         MaxTraced);
            }
            return;
        }
        state.seen.insert(std::move(key));
        ++state.traced;
        Trace(system, state, service, handler, ctx, words);
    } catch (const std::exception& e) {
        LOG_WARNING(Service, "nx2 ipc trace of {} {} failed: {}", service, ctx.GetCommand(), e.what());
    } catch (...) {
        LOG_WARNING(Service, "nx2 ipc trace of {} {} failed", service, ctx.GetCommand());
    }
}

} // namespace Service::Nx2IpcTrace
