// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/common_funcs.h"
#include "common/demangle.h"
#include "common/elf.h"
#include "common/logging.h"
#include "core/arm/debug.h"
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
                 "service and command; listed commands also by data words, {} lists each)",
                 MaxTraced, items, MaxWordVariants);
        return parsed;
    }();
    return filter;
}

// Modules of the traced process and the symbols loaded so far. Never changed once published: a
// trace that needs more makes a copy.
struct Cache {
    u64 process_id{~0ULL};
    std::vector<ModuleRange> modules;                          // sorted by base
    std::map<u64, std::shared_ptr<const SymbolTable>> symbols; // by module base, on demand
    std::chrono::steady_clock::time_point refreshed{};
};

// The mutex is only ever try-locked and held for bookkeeping alone: a service thread never waits
// on it, and the slow parts of a trace (the wait for the requesting thread to be switched out,
// memory reads, the module list) run without it.
struct State {
    std::mutex mutex;
    Seen seen;                          // under mutex
    std::shared_ptr<const Cache> cache; // under mutex
    std::atomic<u32> traced{};
    std::atomic<u32> skipped{}; // matching requests that found the mutex busy, since the last trace
    std::atomic<bool> limit_logged{};
    std::atomic<u32> forced{}; // TraceNow backtraces so far
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
// r11 -> {previous r11, lr}). Every record must lie in the readable region that holds the first
// one (the thread's stack) and above the previous one. ReadBlockUnsafe: a bad chain that points
// at GPU-cached memory must not start a rasterizer download from a service thread.
std::vector<u64> WalkFrames(Kernel::KProcess& process, const Kernel::Svc::ThreadContext& context) {
    auto& memory = process.GetMemory();
    const bool is_64 = process.Is64Bit();
    const u64 word = is_64 ? 8 : 4;
    std::vector<u64> frames{context.pc, context.lr};
    u64 fp = context.fp;
    if (fp == 0) {
        return frames;
    }
    Kernel::KMemoryInfo info{};
    Kernel::Svc::PageInfo page{};
    if (R_FAILED(process.GetPageTable().QueryInfo(std::addressof(info), std::addressof(page), fp))) {
        return frames;
    }
    const auto region = info.GetSvcMemoryInfo();
    if (region.state == Kernel::Svc::MemoryState::Free ||
        (static_cast<u32>(region.permission) & static_cast<u32>(Kernel::Svc::MemoryPermission::Read)) == 0) {
        return frames;
    }
    const u64 low = region.base_address;
    const u64 high = region.base_address + region.size;
    while (frames.size() < MaxFrames) {
        if (fp == 0 || (fp % word) != 0 || fp < low || fp + 2 * word > high ||
            !memory.IsValidVirtualAddressRange(fp, 2 * word)) {
            break;
        }
        u64 next{};
        u64 lr{};
        if (is_64) {
            std::array<u64, 2> record{};
            if (!memory.ReadBlockUnsafe(fp, record.data(), sizeof(record))) {
                break;
            }
            next = record[0];
            lr = record[1];
        } else {
            std::array<u32, 2> record{};
            if (!memory.ReadBlockUnsafe(fp, record.data(), sizeof(record))) {
                break;
            }
            next = record[0];
            lr = record[1];
        }
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

std::vector<ModuleRange> ListModules(Kernel::KProcess& process) {
    std::vector<ModuleRange> modules;
    for (const auto& [base, name] : Core::FindModules(&process)) {
        modules.push_back({base, ModuleEnd(process, base), name}); // a std::map: sorted by base
    }
    return modules;
}

// `size` bytes at `address`, inside the module, read in one block; empty when that fails.
std::vector<u8> ReadModuleBytes(Kernel::KProcess& process, const ModuleRange& module, u64 address,
                                u64 size) {
    constexpr u64 MaxTable = 64ULL << 20;
    if (size == 0 || size > MaxTable || address < module.base || address >= module.end ||
        size > module.end - address) {
        return {};
    }
    std::vector<u8> bytes(size);
    if (!process.GetMemory().ReadBlockUnsafe(address, bytes.data(), bytes.size())) {
        return {};
    }
    return bytes;
}

// The module's symbols: the .dynsym / .dynstr the NSO header names (registered by the loader),
// else the ones the MOD0 header's dynamic section names. Each table is read in one block.
SymbolTable LoadSymbols(Kernel::KProcess& process, const ModuleRange& module, std::string& source) {
    const bool is_64 = process.Is64Bit();
    const u64 entry_size = is_64 ? sizeof(Common::ELF::Elf64_Sym) : sizeof(Common::ELF::Elf32_Sym);
    for (const auto& loaded : process.GetLoadedModules()) {
        if (loaded.base != module.base || loaded.dynsym_size == 0 || loaded.dynstr_size == 0) {
            continue;
        }
        const auto symtab = ReadModuleBytes(process, module, loaded.dynsym, loaded.dynsym_size);
        const auto strtab = ReadModuleBytes(process, module, loaded.dynstr, loaded.dynstr_size);
        auto table = ParseSymbols(symtab, strtab, is_64, entry_size);
        if (!table.entries.empty()) {
            source = "its NSO header";
            return table;
        }
    }

    auto& memory = process.GetMemory();
    struct {
        u32 version;
        u32 header_offset;
    } location{};
    struct {
        u32 signature;
        u32 dynamic_offset;
    } header{};
    if (!memory.ReadBlockUnsafe(module.base, &location, sizeof(location)) ||
        location.header_offset >= module.end - module.base ||
        !memory.ReadBlockUnsafe(module.base + location.header_offset, &header, sizeof(header)) ||
        header.signature != Common::MakeMagic('M', 'O', 'D', '0')) {
        source = "nowhere (no NSO header table, no MOD0)";
        return {};
    }
    u64 strtab{};
    u64 symtab{};
    u64 strsz{};
    u64 syment = entry_size;
    u64 dynamic = module.base + location.header_offset + header.dynamic_offset;
    for (int i = 0; i < 512 && dynamic < module.end; ++i) {
        u64 tag{};
        u64 value{};
        if (is_64) {
            Common::ELF::Elf64_Dyn dyn{};
            if (!memory.ReadBlockUnsafe(dynamic, &dyn, sizeof(dyn))) {
                break;
            }
            tag = static_cast<u64>(dyn.d_tag);
            value = dyn.d_un.d_val;
            dynamic += sizeof(dyn);
        } else {
            Common::ELF::Elf32_Dyn dyn{};
            if (!memory.ReadBlockUnsafe(dynamic, &dyn, sizeof(dyn))) {
                break;
            }
            tag = static_cast<u64>(static_cast<s64>(dyn.d_tag));
            value = dyn.d_un.d_val;
            dynamic += sizeof(dyn);
        }
        if (tag == Common::ELF::ElfDtNull) {
            break;
        }
        if (tag == Common::ELF::ElfDtStrtab) {
            strtab = value;
        } else if (tag == Common::ELF::ElfDtSymtab) {
            symtab = value;
        } else if (tag == Common::ELF::ElfDtStrsz) {
            strsz = value;
        } else if (tag == Common::ELF::ElfDtSyment) {
            syment = value;
        }
    }
    if (symtab == 0 || strtab <= symtab) {
        source = "nowhere (MOD0 without a symbol table)";
        return {};
    }
    // Offsets from the base; the symbol table runs up to the string table (as Core::Symbols
    // assumes).
    const u64 module_size = module.end - module.base;
    if (strsz == 0 && strtab < module_size) {
        strsz = (std::min<u64>)(module_size - strtab, 16ULL << 20);
    }
    const auto symbols = ReadModuleBytes(process, module, module.base + symtab, strtab - symtab);
    const auto strings = ReadModuleBytes(process, module, module.base + strtab, strsz);
    source = "MOD0";
    return ParseSymbols(symbols, strings, is_64, syment);
}

std::string DescribeFrame(const Cache& cache, u32 index, u64 address) {
    const ModuleRange* const module = FindModule(cache.modules, address);
    if (module == nullptr) {
        return fmt::format("  #{} {:#x} (no module)", index, address);
    }
    const u64 offset = address - module->base;
    const auto table = cache.symbols.find(module->base);
    const auto* const symbol =
        table == cache.symbols.end() || !table->second ? nullptr : table->second->Find(offset);
    if (symbol == nullptr) {
        return fmt::format("  #{} {} +{:#x} (no symbol)", index, module->name, offset);
    }
    const u64 into = offset - symbol->start;
    const std::string demangled = Common::DemangleSymbol(symbol->name);
    if (demangled == symbol->name) {
        return fmt::format("  #{} {} +{:#x} {}+{:#x}", index, module->name, offset, symbol->name, into);
    }
    return fmt::format("  #{} {} +{:#x} {}+{:#x} = {}", index, module->name, offset, symbol->name,
                       into, demangled);
}

// The cache to describe `frames` with: `cache` when it has all they need, else a new one. The
// module list is refreshed when the process changed, or when a frame lies outside every module
// (at most once a second); symbols are loaded for the modules frames fall in. Runs unlocked.
std::shared_ptr<const Cache> Update(Kernel::KProcess& process, const std::shared_ptr<const Cache>& cache,
                                    const std::vector<u64>& frames) {
    const u64 process_id = process.GetProcessId();
    const auto now = std::chrono::steady_clock::now();
    std::shared_ptr<Cache> next;
    const auto writable = [&]() -> Cache& {
        if (!next) {
            next = cache ? std::make_shared<Cache>(*cache) : std::make_shared<Cache>();
        }
        return *next;
    };
    const bool missing = cache && std::any_of(frames.begin(), frames.end(), [&](u64 frame) {
        return FindModule(cache->modules, frame) == nullptr;
    });
    if (!cache || cache->process_id != process_id ||
        (missing && now - cache->refreshed >= std::chrono::seconds{1})) {
        auto modules = ListModules(process);
        Cache& fresh = writable();
        if (fresh.process_id != process_id) {
            fresh.symbols.clear();
        }
        // Keep the symbols of modules that did not change.
        for (auto it = fresh.symbols.begin(); it != fresh.symbols.end();) {
            const auto is_base = [&](const ModuleRange& m) { return m.base == it->first; };
            const auto now_at = std::find_if(modules.begin(), modules.end(), is_base);
            const auto was_at = std::find_if(fresh.modules.begin(), fresh.modules.end(), is_base);
            const bool same = now_at != modules.end() && was_at != fresh.modules.end() && *now_at == *was_at;
            it = same ? std::next(it) : fresh.symbols.erase(it);
        }
        fresh.process_id = process_id;
        fresh.modules = std::move(modules);
        fresh.refreshed = now;
    }
    const Cache& current = next ? *next : *cache;
    std::vector<ModuleRange> needed;
    for (const u64 frame : frames) {
        const ModuleRange* const module = FindModule(current.modules, frame);
        if (module != nullptr && !current.symbols.contains(module->base) &&
            std::find(needed.begin(), needed.end(), *module) == needed.end()) {
            needed.push_back(*module);
        }
    }
    for (const auto& module : needed) {
        std::string source;
        auto table = std::make_shared<SymbolTable>(LoadSymbols(process, module, source));
        LOG_INFO(Service, "nx2 ipc trace: {} symbols of {} ({:#x}-{:#x}) from {}", table->entries.size(),
                 module.name, module.base, module.end, source);
        writable().symbols.emplace(module.base, std::move(table));
    }
    if (next) {
        return next;
    }
    return cache;
}

// `label`: "nx2 ipc trace" (OnRequest) or "nx2 ipc trace now" (TraceNow), counted to `limit`.
void Trace(Core::System& system, State& state, std::string_view label, u32 number, u32 limit,
           const std::shared_ptr<const Cache>& cache, std::string_view service, const char* handler,
           HLERequestContext& ctx, const std::vector<u32>& words) {
    Kernel::KThread& thread = ctx.GetThread();
    Kernel::KProcess* const process = thread.GetOwnerProcess();
    std::string text = fmt::format("{} {}/{}: {} {} [{}] \"{}\", {} data words:", label, number,
                                   limit, service, ctx.GetCommand(),
                                   ctx.IsTipc() ? "tipc" : "cmif", handler == nullptr ? "unregistered" : handler,
                                   words.size());
    for (const u32 w : words) {
        text += fmt::format(" {:#x}", w);
    }
    if (const u32 skipped = state.skipped.exchange(0, std::memory_order_relaxed); skipped != 0) {
        text += fmt::format(" ({} matching requests not traced: the trace state was busy)", skipped);
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

    const auto frames = WalkFrames(*process, context);
    const auto updated = Update(*process, cache, frames);
    if (updated != cache) {
        // Publish unless another trace published first (then the next trace loads what it lacks).
        std::unique_lock lk{state.mutex, std::try_to_lock};
        if (lk.owns_lock() && state.cache == cache) {
            state.cache = updated;
        }
    }
    for (std::size_t i = 0; i < frames.size(); ++i) {
        text += '\n';
        text += DescribeFrame(*updated, static_cast<u32>(i), frames[i]);
    }
    LOG_INFO(Service, "{}", text);
}

void LogLimitOnce(State& state) {
    if (!state.limit_logged.exchange(true, std::memory_order_relaxed)) {
        LOG_INFO(Service, "nx2 ipc trace limit reached ({} requests): new requests are no longer traced",
                 MaxTraced);
    }
}

} // Anonymous namespace

bool Enabled() {
    return GetFilter().has_value();
}

void OnRequest(Core::System& system, std::string_view service, const char* handler,
               HLERequestContext& ctx) {
    const auto& filter = GetFilter();
    if (!filter) {
        return;
    }
    const MatchKind match = filter->Classify(service, ctx.GetCommand());
    if (match == MatchKind::None) {
        return;
    }
    State& state = GetState();
    if (state.traced.load(std::memory_order_relaxed) >= MaxTraced) {
        LogLimitOnce(state);
        return;
    }
    try {
        const auto* buffer = ctx.CommandBuffer();
        const auto words = RequestDataWords(std::span<const u32>{buffer, IPC::COMMAND_BUFFER_LENGTH},
                                            ctx.IsTipc(), ctx.GetDataPayloadOffset(),
                                            ctx.HasDomainMessageHeader());
        u32 number{};
        std::shared_ptr<const Cache> cache;
        {
            std::unique_lock lk{state.mutex, std::try_to_lock};
            if (!lk.owns_lock()) {
                // Not remembered as seen: the next such request is traced.
                state.skipped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (state.traced.load(std::memory_order_relaxed) >= MaxTraced) {
                lk.unlock();
                LogLimitOnce(state);
                return;
            }
            if (!state.seen.Admit(service, static_cast<u32>(ctx.GetCommandType()), ctx.GetCommand(),
                                  words, match == MatchKind::Command)) {
                return;
            }
            number = state.traced.fetch_add(1, std::memory_order_relaxed) + 1;
            cache = state.cache;
        }
        Trace(system, state, "nx2 ipc trace", number, MaxTraced, cache, service, handler, ctx, words);
    } catch (const std::exception& e) {
        LOG_WARNING(Service, "nx2 ipc trace of {} {} failed: {}", service, ctx.GetCommand(), e.what());
    } catch (...) {
        LOG_WARNING(Service, "nx2 ipc trace of {} {} failed", service, ctx.GetCommand());
    }
}

bool WillTrace(std::string_view service, u32 command) {
    const auto& filter = GetFilter();
    return filter && filter->Matches(service, command) &&
           GetState().traced.load(std::memory_order_relaxed) < MaxTraced;
}

void TraceNow(Core::System& system, std::string_view service, const char* handler,
              HLERequestContext& ctx) {
    State& state = GetState();
    try {
        const auto* buffer = ctx.CommandBuffer();
        const auto words = RequestDataWords(std::span<const u32>{buffer, IPC::COMMAND_BUFFER_LENGTH},
                                            ctx.IsTipc(), ctx.GetDataPayloadOffset(),
                                            ctx.HasDomainMessageHeader());
        std::shared_ptr<const Cache> cache;
        {
            // As in OnRequest, never wait for the mutex: no backtrace when it is busy.
            std::unique_lock lk{state.mutex, std::try_to_lock};
            if (!lk.owns_lock()) {
                LOG_INFO(Service, "nx2 ipc trace now: {} {}: the trace state was busy, no backtrace",
                         service, ctx.GetCommand());
                return;
            }
            cache = state.cache;
        }
        const u32 number = state.forced.fetch_add(1, std::memory_order_relaxed) + 1;
        if (number > MaxForced) {
            return;
        }
        Trace(system, state, "nx2 ipc trace now", number, MaxForced, cache, service, handler, ctx, words);
    } catch (const std::exception& e) {
        LOG_WARNING(Service, "nx2 ipc trace of {} {} failed: {}", service, ctx.GetCommand(), e.what());
    } catch (...) {
        LOG_WARNING(Service, "nx2 ipc trace of {} {} failed", service, ctx.GetCommand());
    }
}

} // namespace Service::Nx2IpcTrace
