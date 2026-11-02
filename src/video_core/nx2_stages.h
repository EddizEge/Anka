// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstring>
#include <memory>
#include <type_traits>

#include "common/common_types.h"
#include "video_core/nx2_stage_ring.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class Maxwell3D;
}

// EDEN_NX2_BACK_STAGES: the split GPU thread's back (gpu_split.h) as a pipeline of stages.
// A ("GPUBack") runs the engines against their primary registers; their rasterizer is a proxy
// (nx2_stage_proxy.h) that sends what the caches and Vulkan do to B ("GPUMem": memory, buffer
// cache) and C ("GPURec": textures, the Vulkan scheduler) through rings of records, or runs it on
// A while the stages below are parked (a join). B and C read per-channel replicas of Maxwell3D,
// brought up to date with register deltas (nx2_reg_replica.h). Cut a (Kesim-1): A | BC, B does
// everything C would; cut c (Kesim-2): A | B | C. Off (=0): no thread, ring or replica; nothing
// behaves differently. Contract of the parallel tasks (PLAN.md §2.4); owner of the bodies: T1.
namespace VideoCommon::Nx2Stages {

/// The stages. With cut a alone, B is "BC" (memory and record).
enum class Stage : u8 { A, B, C };

/// What a cache reads Maxwell3D for. Memory (buffer cache, pipeline cache): B's replica. Record
/// (rasterizer, texture cache, query cache, StateTracker): C's replica with cut c, else B's.
enum class Role : u8 { Memory, Record };

struct Config {
    /// EDEN_NX2_BACK_STAGES: 0 off, 1 on, 2 verify (lockstep, depth 1, replicas compared),
    /// 3 pass-through (A forwards ring 1 ranges for BC to run with GpuSplit::Execute; no channel
    /// is staged and the proxy is not installed: BC runs the primaries as the unstaged back, K1)
    u8 mode{};
    bool cut_a{};        ///< EDEN_NX2_BACK_CUTS holds 'a' (default with mode != 0: "a" in K1)
    bool cut_c{};        ///< ... holds 'c' (K2)
    bool check{};        ///< EDEN_NX2_STAGE_CHECK=1: owner checks and hazard counters at full speed
    u32 verify_every{1}; ///< EDEN_NX2_BACK_STAGES_EVERY: =2 compares every n-th record
};

/// Read once, at the first call (GpuSplit::MaybeStart).
const Config& Get();

namespace Detail {
inline std::atomic<bool> g_active{};
inline constexpr u8 NoStage = 0xff;
/// The stage the thread runs (set by the stage threads; A in RunStagedBack).
inline thread_local u8 t_stage = NoStage;
/// A runs a join's work (JoinFromA): the stages below are parked at its point of the stream.
inline thread_local bool t_in_join{};
} // namespace Detail

/// Whether the stages run (Start to Stop), =3 included. Any thread. What depends on staged
/// channels tests them, never Active() alone (K1): Maxwell3D's hooks its stage_capture
/// (nx2_stage_api.h), the front's BindChannel tag ChannelStaged and Proxy() (nx2_stage_proxy.h).
inline bool Active() {
    return Detail::g_active.load(std::memory_order_relaxed);
}
inline bool OnStage(Stage stage) {
    return Detail::t_stage == static_cast<u8>(stage);
}
/// On A, inside JoinFromA (or a join A makes at ring level, T1).
inline bool InJoin() {
    return Detail::t_in_join;
}
/// What the proxy forwards from: A outside a join. From any other thread (B or C, the front
/// after a Drain, A inside a join) it calls the real rasterizer.
inline bool Forwarding() {
    return OnStage(Stage::A) && !InJoin();
}

enum class Ring : u8 { ToB, ToC };

/// A Call record's function: gets the record's payload (after the thunk word) and its words.
using Thunk = void (*)(const u64* payload, u32 words);
/// A ring 1 Call entry's function (GpuSplit::CallThunk), of Forward records (S18: calling it as a
/// Thunk would be undefined).
using Ring1Thunk = void (*)(const u64* payload);

/// Producer (A for ToB, B for ToC): room for a Call record running `thunk` with `payload_words`
/// words; returns where the payload goes. From A, the deltas A's capture holds go in the same
/// record, before it (S5/1). The consumer sees it from Commit on (published as GpuSplit does).
/// Only while Active().
u64* Reserve(Ring ring, Thunk thunk, u32 payload_words);
void Commit(Ring ring);
/// Makes what was committed visible to the consumer now (the end of a batch, before a wait).
void Publish(Ring ring);

template <typename F>
void PostThunk(const u64* payload, [[maybe_unused]] u32 words) {
    (*reinterpret_cast<const F*>(payload))();
}

/// Runs `func` on the consumer after everything sent before it (a Call record).
template <typename F>
void Post(Ring ring, const F& func) {
    static_assert(std::is_trivially_copyable_v<F> && alignof(F) <= alignof(u64));
    constexpr u32 words = static_cast<u32>((sizeof(F) + sizeof(u64) - 1) / sizeof(u64));
    u64* const payload = Reserve(ring, &PostThunk<F>, words);
    std::memcpy(payload, &func, sizeof(F));
    Commit(ring);
}

/// A: a ring 1 Call entry tagged DeferTarget::Owner, copied as it is (thunk and payload, D2).
void Forward(Ring ring, Ring1Thunk thunk, const u64* payload, u32 payload_words);
/// A: constant buffer data ProcessCBMultiData sends while staged (T2), for the consumer's replica
/// of the current target (StageCbWrite). Joins the CbData record just before when that is the
/// last one written, not published yet and ends at `address` (S5/3).
void PostCbData(Ring ring, GPUVAddr address, const u32* data, u32 count);
/// A: the primary Maxwell3D the deltas after this record are for (the consumer takes its own
/// replica, ReplicaOf). RegCapture::Note sends it when the engine written to changes; the deltas
/// pending until then go in this record, for the previous target.
void PostTarget(Ring ring, Tegra::Engines::Maxwell3D* primary);
/// A ran ring 1 up to `ring1_position` (the end of each batch, every 4096 words). The last stage
/// (K1: B, K2: C) stores it to GpuSplit's consumed, then Nx2BackCpu::NoteBack and, caught up,
/// Nx2BusyClock::BackStored (D1): Drain then means every stage ran everything (E7).
void PostMark(Ring ring, u64 ring1_position);

/// Why A joins (counted: Counter::JoinsAEngine + reason).
enum class JoinReason : u8 {
    Engine,    ///< Executable methods of an engine other than Maxwell3D (ring level, T1; S9/2)
    Channel,   ///< A channel bound first or changed (StageChannel, S3/S9)
    Value,     ///< A rasterizer member returning a value (MustFlushRegion, GetFlushArea, ...)
    Flush,     ///< FlushRegion / FlushAll / FlushAndInvalidateRegion from A (S7)
    Condition, ///< Conditional rendering (ProcessQueryCondition, its rasterizer call; S1)
    Upload,    ///< Block linear inline upload writing guest memory from A (T5)
    Tagged,    ///< A ring 1 Call tagged DeferTarget::Join (RunCommand, dma_pusher's thunk)
    Other,
    Count,
};

/// A non-owning reference to a callable, called before the function taking it returns.
class FunctionRef {
public:
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef>)
    FunctionRef(F&& func) noexcept
        : object{const_cast<void*>(static_cast<const void*>(std::addressof(func)))},
          call{[](void* callable) { (*static_cast<std::remove_reference_t<F>*>(callable))(); }} {}

    void operator()() const {
        call(object);
    }

private:
    void* object;
    void (*call)(void*);
};

/// A: the stages below run everything sent before this point and park (B writes a Join to ring
/// 3 and waits for C's park with cut c); then `op` runs on A with every cache, the scheduler and
/// the replicas (equal to the primaries there), A being the record owner (InJoin, RecordOwner,
/// CheckOwner); then they go on. The ack wait spins briefly, then sleeps (an atomic wait); with
/// STAGE_CHECK one past 50 ms is logged. =2 lockstep: for a Join record the producer waits for
/// the park ack, never for the record's end (the consumer ends it only once A releases; S8).
void JoinFromA(JoinReason reason, FunctionRef op);
/// K2, B: C runs everything before this point and parks; `op` runs on B (RESETFIX's FlushRegion,
/// Wait/Finish from the buffer runtime, ...). B never calls it holding the buffer cache mutex
/// (S13).
void JoinFromB(FunctionRef op);
/// K2, C: B stops at a record boundary and parks; `op` runs on C (CPU FlushRegion requests run in
/// TickWork, gpu.cpp:284-303). Priority (S13): while C waits for B's park, a JoinFromB B made
/// first wins: C parks for it, then asks again. C takes no buffer cache lock while parked.
void ParkB(FunctionRef op);

/// EDEN_NX2_STAGE_CHECK: reports (the first ones, with the stack, as GpuSplit::CheckOwner) and
/// counts (Counter::StageViolations) a call of `where` from a thread other than `stage`'s; A
/// inside a join may do anything.
void CheckStage(Stage stage, const char* where);
/// The stage recording Vulkan commands, whose use of the scheduler GpuSplit::CheckOwner checks:
/// K1 B, K2 C; A inside a join.
Stage RecordOwner();

/// GpuSplit::MaybeStart, before the back thread starts (nothing runs behind the front): when
/// Get().mode != 0, makes the rings and starts B (and C); at =1 and =2 it also installs the proxy
/// and stages every channel (InstallProxy, S3), at =3 neither (K1: BC runs the primaries with
/// GpuSplit::Execute, a staged channel's hooks would send records from BC to its own ring and its
/// caches would read replicas no delta reaches). Active() is true from here on and the back
/// thread runs A (RunStagedBack). The real rasterizer's NoteEngineWork: no light draw follows one
/// from before (S18).
void Start(Core::System& system);
/// GpuSplit::Stop, after the back thread (A) stopped: B and C run what they hold and stop (at
/// shutdown, with the stop request alone, they stop without it: NotifyShutdown, gpu_thread.cpp),
/// the channels are unstaged (UninstallProxy, =1 and =2), the rings freed, NoteEngineWork as at
/// Start; Active() false.
void Stop();

} // namespace VideoCommon::Nx2Stages
