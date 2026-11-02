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
    /// 3 pass-through (A forwards ring 1 ranges for BC to run with GpuSplit::Execute)
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

/// Whether the stages run (Start to Stop). Any thread.
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

/// Producer (A for ToB, B for ToC): room for a Call record running `thunk` with `payload_words`
/// words; returns where the payload goes. The consumer sees it from Commit on (published as
/// GpuSplit does). Only while Active().
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

/// A ran ring 1 up to `ring1_position` (the end of each batch, every 4096 words). The last stage
/// (K1: B, K2: C) stores it to GpuSplit's consumed, then Nx2BackCpu::NoteBack and, caught up,
/// Nx2BusyClock::BackStored (D1): Drain then means every stage ran everything (E7).
void PostMark(Ring ring, u64 ring1_position);
/// A, before every record it forwards: sends the register deltas captured since the last one
/// (T2's RegCapture, D3).
void FlushBeforeForward();

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
/// STAGE_CHECK one past 50 ms is logged.
void JoinFromA(FunctionRef op);
/// K2, B: C runs everything before this point and parks; `op` runs on B.
void JoinFromB(FunctionRef op);
/// K2, C: B stops at a record boundary and parks; `op` runs on C (CPU FlushRegion requests run in
/// TickWork, gpu.cpp:284-303).
void ParkB(FunctionRef op);

/// EDEN_NX2_STAGE_CHECK: reports (the first ones, with the stack, as GpuSplit::CheckOwner) a call
/// of `where` from a thread other than `stage`'s; A inside a join may do anything.
void CheckStage(Stage stage, const char* where);
/// The stage recording Vulkan commands, whose use of the scheduler GpuSplit::CheckOwner checks:
/// K1 B, K2 C; A inside a join.
Stage RecordOwner();
/// =2: the consumer's comparison after a record, every verify_every (T2 fills the Maxwell3D side).
void VerifyHook();

/// GpuSplit::MaybeStart, before the back thread starts (nothing runs behind the front): when
/// Get().mode != 0, makes the rings, starts B (and C), installs the proxy (InstallProxy); Active()
/// is true from here on and the back thread runs A (RunStagedBack).
void Start(Core::System& system);
/// GpuSplit::Stop, under a Drain: B and C stop, the proxy goes (UninstallProxy), the rings are
/// freed; Active() false.
void Stop();

} // namespace VideoCommon::Nx2Stages
