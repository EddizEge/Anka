// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include "video_core/control/channel_state.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/nx2_reg_replica.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_stage_api.h"
#include "video_core/rasterizer_interface.h"

// Back stages: the channels' replicas and what keeps them equal to the primaries (T2). The
// Maxwell3D side of it (capture hooks, delta and side state application, the constant buffer
// write, =2 comparison) is in maxwell_3d.cpp, next to what it mirrors.
namespace VideoCommon::Nx2Stages {

namespace {

using Tegra::Engines::Maxwell3D;

/// Binds the caches to `channel` again when they are bound to it: they take their Maxwell3D at
/// BindToChannel (EngineFor), the StateTracker its dirty flags at ChangeChannel.
void RebindIfBound(Tegra::Control::ChannelState& channel, VideoCore::RasterizerInterface& real) {
    if (g_nx2_bound_channel.load(std::memory_order_relaxed) == channel.bind_id) {
        real.BindChannel(channel);
    }
}

std::unique_ptr<Maxwell3D> MakeReplica(Maxwell3D& primary, VideoCore::RasterizerInterface& real) {
    auto replica = std::make_unique<Maxwell3D>(primary.GetMemoryManager());
    replica->StageCopyFrom(primary, &real);
    return replica;
}

} // Anonymous namespace

bool ChannelStaged(const Tegra::Control::ChannelState& channel) {
    return channel.stage_staged.load(std::memory_order_acquire);
}

void StageChannel(Tegra::Control::ChannelState& channel, VideoCore::RasterizerInterface& proxy,
                  VideoCore::RasterizerInterface& real) {
    if (!channel.initialized || !channel.payload) {
        // Not set up yet (the guest makes channels lazily, S3): staged at its first BindChannel
        // join.
        return;
    }
    if (ChannelStaged(channel)) {
        // An address space bound in between gets the proxy too (S3).
        channel.memory_manager->BindRasterizer(&proxy);
        return;
    }
    auto& primary = channel.payload->maxwell_3d;
    channel.stage_replica_b = MakeReplica(primary, real);
    if (Get().cut_c) {
        channel.stage_replica_c = MakeReplica(primary, real);
    }
    primary.StageBegin(&Capture(), {channel.stage_replica_b.get(), channel.stage_replica_c.get()});
    SetEngineRasterizer(channel, &proxy);
    channel.stage_staged.store(true, std::memory_order_release);
    RebindIfBound(channel, real);
}

void UnstageChannel(Tegra::Control::ChannelState& channel, VideoCore::RasterizerInterface& real) {
    if (!ChannelStaged(channel)) {
        return;
    }
    SetEngineRasterizer(channel, &real);
    channel.payload->maxwell_3d.StageEnd();
    channel.stage_staged.store(false, std::memory_order_release);
    // The caches let go of the replicas before they are freed.
    RebindIfBound(channel, real);
    channel.stage_replica_b.reset();
    channel.stage_replica_c.reset();
}

void SetEngineRasterizer(Tegra::Control::ChannelState& channel,
                         VideoCore::RasterizerInterface* rasterizer) {
    // ChannelState::BindRasterizer but DmaPusher's (it keeps the real one, D2).
    channel.memory_manager->BindRasterizer(rasterizer);
    auto& payload = *channel.payload;
    payload.maxwell_3d.BindRasterizer(rasterizer);
    payload.fermi_2d.BindRasterizer(rasterizer);
    payload.kepler_memory.BindRasterizer(rasterizer);
    payload.kepler_compute.BindRasterizer(rasterizer);
    payload.maxwell_dma.BindRasterizer(rasterizer);
}

Tegra::Engines::Maxwell3D& EngineFor(Tegra::Control::ChannelState& channel, Role role) {
    if (!ChannelStaged(channel)) {
        return channel.payload->maxwell_3d;
    }
    if (role == Role::Record && channel.stage_replica_c) {
        return *channel.stage_replica_c;
    }
    return *channel.stage_replica_b;
}

Tegra::Engines::Maxwell3D* ReplicaOf(Tegra::Engines::Maxwell3D& primary, Stage stage) {
    switch (stage) {
    case Stage::B:
        return primary.stage_replicas[0];
    case Stage::C:
        return primary.stage_replicas[1];
    default:
        return nullptr;
    }
}

Nx2RegReplica::RegCapture& Capture() {
    static Nx2RegReplica::RegCapture capture;
    // Reset reads whether the constant buffer data registers are kept (=2).
    [[maybe_unused]] static const bool ready = [] {
        capture.Reset();
        return true;
    }();
    return capture;
}

// ApplyDelta and ApplySideState: the callers time them (Zone::ReplicaApply), and DeltaWords is
// counted where A takes the words (Nx2Stages::TakeDeltas; B and C apply the same ones).
void ApplyDelta(Tegra::Engines::Maxwell3D& replica, const u64* words, u32 count) {
    replica.StageApplyDelta(words, count);
}

void StageCbWrite(Tegra::Engines::Maxwell3D& replica, GPUVAddr address, const u32* data,
                  u32 count) {
    replica.StageCbWrite(address, data, count);
}

u32 SideStateWords(Tegra::Engines::Maxwell3D& primary) {
    return primary.stage_sent ? primary.StageSideWords() : 0;
}

void WriteSideState(Tegra::Engines::Maxwell3D& primary, u64* out) {
    Nx2Profile::Add(Nx2Profile::Counter::SideStates, 1);
    primary.StageWriteSide(out);
}

void ApplySideState(Tegra::Engines::Maxwell3D& replica, const u64* words, u32 count) {
    replica.StageApplySide(words, count);
}

bool VerifyReplica(Tegra::Engines::Maxwell3D& primary, Stage stage) {
    Tegra::Engines::Maxwell3D* const replica = ReplicaOf(primary, stage);
    if (replica == nullptr || !primary.stage_sent) {
        return true;
    }
    Nx2Profile::Add(Nx2Profile::Counter::VerifyChecks, 1);
    const bool same = primary.StageVerify(*replica);
    if (!same) {
        Nx2Profile::Add(Nx2Profile::Counter::VerifyMismatches, 1);
    }
    return same;
}

} // namespace VideoCommon::Nx2Stages
