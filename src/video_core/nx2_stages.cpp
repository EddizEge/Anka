// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "common/assert.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/nx2_reg_replica.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_back_cpu.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_stage_api.h"
#include "video_core/nx2_stage_proxy.h"
#include "video_core/nx2_stages.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

// The back stages' core (T1): the stages below A (StagePipeline, nx2_stage_ring.h) run with the
// GPU's handler here, A's side of the records and joins, the owner checks, Start and Stop. A's
// loop is GpuSplit::RunStagedBack (gpu_split.cpp).
namespace VideoCommon::Nx2Stages {

// JoinFromA counts Counter::JoinsAEngine + reason.
static_assert(static_cast<size_t>(Nx2Profile::Counter::JoinsAOther) -
                  static_cast<size_t>(Nx2Profile::Counter::JoinsAEngine) + 1 ==
              static_cast<size_t>(JoinReason::Count));
static_assert(StageIdA == static_cast<u8>(Stage::A) && StageIdB == static_cast<u8>(Stage::B) &&
              StageIdC == static_cast<u8>(Stage::C));
static_assert(std::is_same_v<RecordThunk, Thunk> && std::is_same_v<RecordRing1Thunk, Ring1Thunk>);

namespace {

using Nx2Profile::Counter;
using Nx2Profile::Zone;
using Tegra::Engines::Maxwell3D;

template <Zone Which>
struct ZoneScope : Nx2Profile::Scope {
    ZoneScope() : Scope{Which} {}
};

Core::System* g_system{};
VideoCore::RasterizerInterface* g_real{};
bool g_check{};
/// The stage recording Vulkan commands (RecordOwner): B, C with cut c, A in its joins.
std::atomic<u8> g_owner{StageIdB};
u8 g_default_owner{StageIdB};
/// The default owner runs records (CheckRecordOwner: the front may use the scheduler otherwise).
std::atomic<bool> g_record_busy{};
/// The record side's begin and end of the stages (SetStageHooks).
void (*g_hook_begin)(void*){};
void (*g_hook_end)(void*){};
void* g_hook_context{};

/// What B and C do with the records (StagePipeline's handler).
struct GpuHandler {
    struct Scopes {
        using InWait = ZoneScope<Zone::StageInWait>;
        using OutWait = ZoneScope<Zone::StageOutWait>;
        using JoinWait = ZoneScope<Zone::JoinWait>;
        using ParkWait = ZoneScope<Zone::ParkWait>;
    };

    /// The primary Maxwell3D each stage's deltas are for (Target records).
    std::array<Maxwell3D*, 3> targets{};

    Maxwell3D* Replica(u8 stage) const {
        Maxwell3D* const target = targets[stage];
        return target != nullptr ? ReplicaOf(*target, static_cast<Stage>(stage)) : nullptr;
    }

    u32 PendingDeltas() {
        return Capture().PendingWords();
    }
    void TakeDeltas(u64* out, u32 count) {
        Capture().TakeWords(out);
        Nx2Profile::Add(Counter::DeltaWords, count);
    }
    void Apply(u8 stage, const u64* words, u32 count) {
        if (Maxwell3D* const replica = Replica(stage)) {
            Nx2Profile::Scope scope{Zone::ReplicaApply};
            ApplyDelta(*replica, words, count);
        }
    }
    void Target(u8 stage, u64 primary) {
        targets[stage] = reinterpret_cast<Maxwell3D*>(primary);
    }
    void CbData(u8 stage, u64 address, const u32* data, u32 count) {
        if (Maxwell3D* const replica = Replica(stage)) {
            StageCbWrite(*replica, address, data, count);
        }
    }
    void ExecA(u64 position, u64 words) {
        GpuSplit::Get().StageExecute(position, words);
    }
    void Mark(u8, u64 position, bool caught_up) {
        GpuSplit::Get().StageConsumed(position, caught_up);
    }
    void Verify(u8 stage) {
        if (Maxwell3D* const target = targets[stage]) {
            VerifyReplica(*target, static_cast<Stage>(stage));
        }
    }
    void BadSequence(u8 stage, u16 expected, u16 seen) {
        static std::atomic<u32> reports{};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 8) {
            LOG_CRITICAL(HW_GPU, "GPU stages: {} read record {} where {} was due",
                         Nx2Profile::stage_report_names[stage], seen, expected);
        }
    }
    void ThreadStart(u8 stage) {
        Detail::t_stage = stage;
        if (stage == StageIdB) {
            // The stage a frame waits on (S19): Critical, and the back's core of its own
            // (EDEN_NX2_BACK_CPU); A and C are kept off it with the process's other threads.
            Common::SetCurrentThreadName("GPUMem");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
            Common::SetCurrentThreadToPerformanceCores();
            Nx2BackCpu::StartBack();
        } else {
            Common::SetCurrentThreadName("GPURec");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
            Common::SetCurrentThreadToPerformanceCores();
        }
        g_system->RegisterHostThread();
    }
    void Report(u8 stage) {
        Nx2Profile::Report(Nx2Profile::stage_report_names[stage]);
    }
    void Busy(u8 stage, bool busy) {
        if (stage == g_default_owner) {
            g_record_busy.store(busy, busy ? std::memory_order_relaxed : std::memory_order_release);
        }
    }
    void Wrote(u8 to_stage, size_t bytes) {
        Nx2Profile::Add(to_stage == StageIdB ? Counter::RingBytesB : Counter::RingBytesC, bytes);
    }
    void Parked() {
        Nx2Profile::Add(Counter::Parks, 1);
    }
    /// A broken K3 rule (StagePipeline): counted.
    void Hazard(u8, StageHazard, u64, u64) {
        Nx2Profile::Add(Counter::StageViolations, 1);
    }
    /// C at a record boundary, asked (RequestBoundaryWork): nothing asks yet (StageTickWork).
    void BoundaryWork() {}
};

GpuHandler g_handler;
StagePipeline<GpuHandler> g_pipeline{g_handler};

/// The first reports of a check (CheckStage, CheckRecordOwner), with the stack (as
/// GpuSplit::CheckOwner) on stderr, and in short in the log (the measuring scripts read the log
/// alone): where, the thread's name and the first frames.
void ReportStack(const char* check, const char* where) {
#ifdef _WIN32
    constexpr u32 MaxReports = 20;
    constexpr USHORT LogFrames = 6;
    static std::atomic<u32> reports{};
    const u32 report = reports.fetch_add(1, std::memory_order_relaxed);
    if (report >= MaxReports) {
        return;
    }
    const u8 stage = Detail::t_stage;
    const char* const stage_name =
        stage < 3 ? Nx2Profile::stage_report_names[stage] : "no stage";
    const char* const owner_name =
        Nx2Profile::stage_report_names[g_owner.load(std::memory_order_relaxed)];
    char thread_name[64] = "?";
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleA("KernelBase.dll"), "GetThreadDescription"));
    if (PWSTR description{}; get_description != nullptr &&
                             SUCCEEDED(get_description(GetCurrentThread(), &description))) {
        if (description[0] != L'\0') {
            WideCharToMultiByte(CP_UTF8, 0, description, -1, thread_name, sizeof(thread_name),
                                nullptr, nullptr);
            thread_name[sizeof(thread_name) - 1] = '\0';
        }
        LocalFree(description);
    }
    std::fprintf(stderr, "GPU %s: %s on thread %lu %s (%s%s), the record owner is %s\n", check,
                 where, GetCurrentThreadId(), thread_name, stage_name,
                 Detail::t_in_join ? ", in a join" : "", owner_name);
    std::string log_frames;
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{};
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = std::strrchr(name, '\\');
        const auto offset = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                            reinterpret_cast<uintptr_t>(module));
        std::fprintf(stderr, "  frame %s+%#llx\n", base_name ? base_name + 1 : name, offset);
        if (i < LogFrames) {
            log_frames += fmt::format("{}{}+{:#x}", i == 0 ? "" : " < ",
                                      base_name ? base_name + 1 : name, offset);
        }
    }
    std::fflush(stderr);
    LOG_WARNING(HW_GPU, "GPU {} ({}/{}): {} on thread {} '{}' ({}{}), the record owner is {}; {}",
                check, report + 1, MaxReports, where, GetCurrentThreadId(), thread_name,
                stage_name, Detail::t_in_join ? ", in a join" : "", owner_name, log_frames);
#else
    LOG_ERROR(HW_GPU, "GPU {}: {}", check, where);
#endif
}

/// A record's producer (K4, K9): never A inside a join, and 1 + payload words at most
/// MaxBodyWords. The asserts only when one fails: Reserve runs for each record.
void CheckRecord(u32 payload_words) {
    if (InJoin() || payload_words >= MaxBodyWords) [[unlikely]] {
        ASSERT_MSG(!InJoin(), "GPU stages: a record from A inside a join");
        ASSERT_MSG(1 + payload_words <= MaxBodyWords, "GPU stages: a record of {} words",
                   1 + payload_words);
    }
}

u64* BeginRecord(Ring ring, RecordKind kind, u32 body_words, u32 flags = 0) {
    return ring == Ring::ToB ? g_pipeline.BeginB(kind, body_words, flags)
                             : g_pipeline.BeginC(kind, body_words, flags);
}

void EndRecord(Ring ring) {
    if (ring == Ring::ToB) {
        g_pipeline.EndB();
    } else {
        g_pipeline.EndC();
    }
}

} // Anonymous namespace

const Config& Get() {
    static const Config config = [] {
        const StageOptions options = ParseStageOptions(
            std::getenv("EDEN_NX2_BACK_STAGES"), std::getenv("EDEN_NX2_BACK_CUTS"),
            std::getenv("EDEN_NX2_STAGE_CHECK"), std::getenv("EDEN_NX2_BACK_STAGES_EVERY"));
        return Config{
            .mode = options.mode,
            .cut_a = options.mode != 0,
            .cut_c = options.cut_c,
            .check = options.check,
            .verify_every = options.verify_every,
        };
    }();
    return config;
}

u64* Reserve(Ring ring, Thunk thunk, u32 payload_words) {
    CheckRecord(payload_words);
    u64* const body = BeginRecord(ring, RecordKind::Call, 1 + payload_words);
    body[0] = reinterpret_cast<u64>(thunk);
    return body + 1;
}

void Commit(Ring ring) {
    EndRecord(ring);
}

void Publish(Ring ring) {
    if (ring == Ring::ToB) {
        g_pipeline.PublishB();
    } else {
        g_pipeline.PublishC();
    }
}

void Forward(Ring ring, Ring1Thunk thunk, const u64* payload, u32 payload_words) {
    CheckRecord(payload_words);
    u64* const body = BeginRecord(ring, RecordKind::Forward, 1 + payload_words);
    body[0] = reinterpret_cast<u64>(thunk);
    std::memcpy(body + 1, payload, size_t{payload_words} * sizeof(u64));
    EndRecord(ring);
}

void PostCbData(Ring ring, Tegra::Engines::Maxwell3D& primary, GPUVAddr address, const u32* data,
                u32 count) {
    CheckRecord(0);
    Capture().SelectTarget(primary);
    if (ring == Ring::ToB) {
        g_pipeline.PostCbData(address, data, count);
        return;
    }
    do {
        const u32 part = (std::min)(count, decltype(g_pipeline)::MaxCbDataCount);
        const u32 bytes = part * static_cast<u32>(sizeof(u32));
        const u32 data_words = (bytes + 7) / 8;
        u64* const body = BeginRecord(ring, RecordKind::CbData, 2 + data_words);
        body[0] = address;
        body[1] = bytes;
        if (data_words != 0) {
            body[1 + data_words] = 0;
        }
        std::memcpy(body + 2, data, bytes);
        EndRecord(ring);
        address += bytes;
        data += part;
        count -= part;
    } while (count != 0);
}

void PostTarget(Ring ring, Tegra::Engines::Maxwell3D* primary) {
    // The stages off (a capture without them, YUZU_TESTS' unit test): no ring to write to.
    if (!Active()) {
        return;
    }
    CheckRecord(1);
    u64* const body = BeginRecord(ring, RecordKind::Target, 1);
    body[0] = reinterpret_cast<u64>(primary);
    EndRecord(ring);
}

void PostMark(Ring ring, u64 ring1_position) {
    CheckRecord(1);
    u64* const body = BeginRecord(ring, RecordKind::Mark, 1);
    body[0] = ring1_position;
    EndRecord(ring);
}

void JoinFromA(JoinReason reason, FunctionRef op) {
    // Nested, from another thread, off or =3 (K4): at once, no record, no wait, not counted.
    if (!Forwarding() || !Active() || g_pipeline.Options().mode == 3) {
        op();
        return;
    }
    Nx2Profile::Add(Counter::JoinsA, 1);
    Nx2Profile::Add(
        static_cast<Counter>(static_cast<u32>(Counter::JoinsAEngine) + static_cast<u32>(reason)),
        1);
    const auto start =
        g_check ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const u32 join = g_pipeline.JoinABegin();
    if (g_check) {
        const auto waited = std::chrono::steady_clock::now() - start;
        if (waited > std::chrono::milliseconds{50}) {
            LOG_WARNING(HW_GPU,
                        "GPU stages: a join from A waited {} ms for the stages to park "
                        "(reason {})",
                        std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(),
                        static_cast<u32>(reason));
        }
    }
    Detail::t_in_join = true;
    g_owner.store(StageIdA, std::memory_order_release);
    op();
    g_owner.store(g_default_owner, std::memory_order_release);
    Detail::t_in_join = false;
    g_pipeline.JoinAEnd(join);
}

void JoinFromB(FunctionRef op) {
    // Nested: B inside a join's op is the record owner with C parked already.
    if (!Active() || !OnStage(Stage::B) || !g_pipeline.Options().cut_c ||
        g_owner.load(std::memory_order_relaxed) == StageIdB) {
        op();
        return;
    }
    Nx2Profile::Add(Counter::JoinsB, 1);
    g_pipeline.JoinFromB([&] {
        g_owner.store(StageIdB, std::memory_order_release);
        op();
        g_owner.store(g_default_owner, std::memory_order_release);
    });
}

void JoinFromBLocked(FunctionRef op) {
    if (!Active() || !OnStage(Stage::B) || !g_pipeline.Options().cut_c ||
        g_owner.load(std::memory_order_relaxed) == StageIdB) {
        op();
        return;
    }
    Nx2Profile::Add(Counter::JoinsB, 1);
    const auto start =
        g_check ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    g_pipeline.JoinFromBLocked([&] {
        if (g_check) {
            // B holds the buffer cache mutex while it waits: C reaching the join's target must
            // not take it (K3 (a)); a long wait is the sign it does.
            const auto waited = std::chrono::steady_clock::now() - start;
            if (waited > std::chrono::milliseconds{50}) {
                LOG_WARNING(HW_GPU,
                            "GPU stages: a locked join from B waited {} ms for C to park "
                            "(K3 deadlock suspect: C taking the buffer cache mutex)",
                            std::chrono::duration_cast<std::chrono::milliseconds>(waited).count());
            }
        }
        g_owner.store(StageIdB, std::memory_order_release);
        op();
        g_owner.store(g_default_owner, std::memory_order_release);
    });
}

void ParkB(FunctionRef op) {
    if (!Active() || !OnStage(Stage::C)) {
        op();
        return;
    }
    g_pipeline.ParkB(op);
}

bool StageTickWork([[maybe_unused]] FunctionRef run) {
    // The interface first (T9a): until its body the caller runs the requests where it stands.
    return false;
}

void CheckStage(Stage stage, const char* where) {
    if (!g_check || OnStage(stage) || (OnStage(Stage::A) && InJoin())) {
        return;
    }
    Nx2Profile::Add(Counter::StageViolations, 1);
    ReportStack("stage check", where);
}

void CheckBufferLock([[maybe_unused]] const char* where) {
    // The interface first (T9a): checked once ParkB marks its op (T9's next step).
}

void ReportStageHazard(const char* what) {
    if (!g_check) {
        return;
    }
    Nx2Profile::Add(Counter::StageViolations, 1);
    ReportStack("stage hazard", what);
}

Stage RecordOwner() {
    return static_cast<Stage>(g_owner.load(std::memory_order_acquire));
}

void SetStageHooks(void (*begin)(void*), void (*end)(void*), void* context) {
    g_hook_begin = begin;
    g_hook_end = end;
    g_hook_context = context;
}

void Start(Core::System& system) {
    const Config& config = Get();
    if (config.mode == 0 || Active()) {
        return;
    }
    g_system = &system;
    g_real = system.GPU().Renderer().ReadRasterizer();
    g_check = config.check;
    if (g_check) {
        // The scheduler's owner checks (GpuSplit::CheckOwner) check the record owner.
        GpuSplit::check_owner = true;
    }
    const StageOptions options{
        .mode = config.mode,
        .cut_c = config.cut_c && config.mode != 3,
        .check = config.check,
        .verify_every = config.verify_every,
    };
    g_default_owner = options.cut_c ? StageIdC : StageIdB;
    g_owner.store(g_default_owner, std::memory_order_relaxed);
    g_record_busy.store(false, std::memory_order_relaxed);
    g_handler.targets = {};
    Capture().Reset();
    // No light draw follows one from before (S18).
    g_real->NoteEngineWork();
    // EDEN_NX2_BACK_CPU: the front on a core of its own too (S19); before B starts and pins.
    Nx2BackCpu::StartFront();
    if (options.cut_c && g_hook_begin != nullptr) {
        g_hook_begin(g_hook_context);
    }
    g_pipeline.Start(options);
    // =1 and =2 only (K1): =3 runs the primaries on BC as the unstaged back did, with no proxy
    // and no staged channel (a staged channel's hooks would send records from BC to its own ring).
    if (config.mode != 3) {
        InstallProxy(system, *g_real);
    }
    Detail::g_active.store(true, std::memory_order_seq_cst);
    LOG_INFO(HW_GPU, "GPU back stages: A | {}{} (EDEN_NX2_BACK_STAGES={}{}{})",
             options.cut_c ? "B | C" : "BC",
             config.mode == 2   ? ", lockstep with replica checks"
             : config.mode == 3 ? ", pass-through"
                                : "",
             config.mode, options.cut_c ? ", cuts ac" : "", config.check ? ", STAGE_CHECK" : "");
    // K2 (cut c) runs B | C before T9 classifies the events that need both: a trial of the
    // plumbing, not for play or measuring. Once per process (Start runs again after a Stop).
    static std::atomic_flag cut_c_warned{};
    if (options.cut_c && !cut_c_warned.test_and_set(std::memory_order_relaxed)) {
        LOG_WARNING(HW_GPU, "GPU back stages: cut c is a plumbing trial only until T9 "
                            "(EDEN_NX2_BACK_CUTS holds 'c'): not for play or measuring");
    }
}

void Stop() {
    if (!Active()) {
        return;
    }
    // A stopped (GpuSplit::Stop joined it): B and C stop at their next record boundary, with
    // nothing left after a drain, and at shutdown without running what they hold.
    g_pipeline.Stop();
    if (g_pipeline.Options().cut_c && g_hook_end != nullptr) {
        g_hook_end(g_hook_context);
    }
    if (Get().mode != 3) {
        // As Start: only what it installed (K1).
        UninstallProxy(*g_system);
    }
    Capture().Reset();
    g_handler.targets = {};
    g_real->NoteEngineWork();
    g_owner.store(StageIdB, std::memory_order_relaxed);
    g_record_busy.store(false, std::memory_order_relaxed);
    Detail::g_active.store(false, std::memory_order_seq_cst);
}

namespace Detail {

void PostExecA(u64 ring1_position, u64 words) {
    u64* const body = g_pipeline.BeginB(RecordKind::ExecA, 2);
    body[0] = ring1_position;
    body[1] = words;
    g_pipeline.EndB();
}

void PostBatchEnd(u64 ring1_position) {
    u64* const body = g_pipeline.BeginB(RecordKind::Mark, 1, MarkCaughtUp);
    body[0] = ring1_position;
    g_pipeline.EndB();
    g_pipeline.PublishB();
}

void CheckRecordOwner(const char* where) {
    const u8 owner = g_owner.load(std::memory_order_acquire);
    if (t_stage == owner) {
        return;
    }
    if (OnStage(Stage::A) && !InJoin()) {
        // A reached the real rasterizer outside a join (S3).
        Nx2Profile::Add(Counter::StageUnproxied, 1);
    } else if (t_stage == NoStage &&
               (!g_record_busy.load(std::memory_order_acquire) || GpuSplit::Get().BackCaughtUp())) {
        // The front after a drain, CPU threads: the stages are idle (as GpuSplit::CheckOwner).
        // Caught up, the last stage may still end its batch (busy) after the mark it stored, a
        // check (=2), a release: it touches no cache and no scheduler then (R1 eszaman-3).
        return;
    }
    ReportStack("stages owner check", where);
}

} // namespace Detail

} // namespace VideoCommon::Nx2Stages
