// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/nx2_reg_replica.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_gpu_pages.h"
#include "video_core/nx2_stage_api.h"
#include "video_core/nx2_stage_draw.h"
#include "video_core/nx2_stage_proxy.h"
#include "video_core/nx2_stage_ring.h"
#include "video_core/nx2_stages.h"

// Back stages: every contract header together, and the declarations the parallel tasks code
// against (PLAN.md §2.4). A changed signature fails here before it fails another task's branch.
namespace {
using namespace VideoCommon::Nx2Stages;
using Tegra::Engines::Maxwell3D;
using VideoCore::RasterizerInterface;

template <typename Expected, typename Actual>
constexpr bool Same = std::is_same_v<Expected, Actual>;

static_assert(Same<void (*)(Ring, Maxwell3D&, GPUVAddr, const u32*, u32), decltype(&PostCbData)>);
static_assert(Same<void (*)(Ring, Maxwell3D*), decltype(&PostTarget)>);
static_assert(Same<void (*)(Ring, u64), decltype(&PostMark)>);
static_assert(Same<void (*)(Ring, Ring1Thunk, const u64*, u32), decltype(&Forward)>);
static_assert(Same<u64* (*)(Ring, Thunk, u32), decltype(&Reserve)>);
static_assert(Same<void (*)(JoinReason, FunctionRef), decltype(&JoinFromA)>);
static_assert(Same<void (*)(FunctionRef), decltype(&JoinFromB)>);
static_assert(Same<void (*)(FunctionRef), decltype(&ParkB)>);
// K3 (a), T9: the buffer runtime's join keeps B's buffer cache mutex; the CPU's sync requests run
// on C at a record boundary in a ParkB; the STAGE_CHECK diagnostics; the record side's hooks.
static_assert(Same<void (*)(FunctionRef), decltype(&JoinFromBLocked)>);
static_assert(Same<bool (*)(FunctionRef), decltype(&StageTickWork)>);
static_assert(Same<void (*)(const char*), decltype(&CheckBufferLock)>);
static_assert(Same<void (*)(const char*), decltype(&ReportStageHazard)>);
static_assert(Same<void (*)(void (*)(void*), void (*)(void*), void*), decltype(&SetStageHooks)>);
static_assert(Same<void (*)(Tegra::Control::ChannelState&, RasterizerInterface&),
                   decltype(&BindNewChannel)>);
static_assert(Same<void (*)(Tegra::MemoryManager&, RasterizerInterface&),
                   decltype(&BindNewAddressSpace)>);
static_assert(Same<Nx2StageProxy* (*)(), decltype(&Proxy)>);
static_assert(Same<bool (*)(Maxwell3D&, Stage), decltype(&VerifyReplica)>);
static_assert(Same<void (*)(Maxwell3D&, GPUVAddr, const u32*, u32), decltype(&StageCbWrite)>);
static_assert(Same<void (VideoCommon::Nx2RegReplica::RegCapture::*)(Maxwell3D&),
                   decltype(&VideoCommon::Nx2RegReplica::RegCapture::SelectTarget)>);
static_assert(Same<void (Nx2GpuPages::*)(DAddr, u64, u64), decltype(&Nx2GpuPages::PreSet)>);
static_assert(Same<void (Nx2GpuPages::*)(DAddr, u64, u64), decltype(&Nx2GpuPages::Clear)>);
static_assert(Same<u64, decltype(DrawToC::seq)>);
static_assert(static_cast<u8>(VideoCommon::DeferTarget::Join) == 2);
} // Anonymous namespace

TEST_CASE("Nx2StageContract: the contract headers build together", "[video_core]") {
    int calls = 0;
    const auto add = [&calls] { ++calls; };
    const FunctionRef ref{add};
    ref();
    ref();
    REQUIRE(calls == 2);
    // Skeleton (T0): every page may hold GPU-written data.
    REQUIRE(Nx2GpuPages{}.Holds(0, 1));
}
