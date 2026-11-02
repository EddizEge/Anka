// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/control/channel_state.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/nx2_reg_replica.h"
#include "video_core/nx2_stage_api.h"

// Skeleton (T0): no channel is ever staged. The bodies are T2's.
namespace VideoCommon::Nx2Stages {

bool ChannelStaged(const Tegra::Control::ChannelState& channel) {
    return false;
}

void StageChannel(Tegra::Control::ChannelState& channel, VideoCore::RasterizerInterface& proxy,
                  VideoCore::RasterizerInterface& real) {}

void UnstageChannel(Tegra::Control::ChannelState& channel, VideoCore::RasterizerInterface& real) {}

void SetEngineRasterizer(Tegra::Control::ChannelState& channel,
                         VideoCore::RasterizerInterface* rasterizer) {}

Tegra::Engines::Maxwell3D& EngineFor(Tegra::Control::ChannelState& channel, Role role) {
    return channel.payload->maxwell_3d;
}

Tegra::Engines::Maxwell3D* ReplicaOf(Tegra::Engines::Maxwell3D& primary, Stage stage) {
    return nullptr;
}

Nx2RegReplica::RegCapture& Capture() {
    static Nx2RegReplica::RegCapture capture;
    return capture;
}

void ApplyDelta(Tegra::Engines::Maxwell3D& replica, const u64* words, u32 count) {}

void StageCbWrite(Tegra::Engines::Maxwell3D& replica, GPUVAddr address, const u32* data,
                  u32 count) {}

u32 SideStateWords(Tegra::Engines::Maxwell3D& primary) {
    return 0;
}

void WriteSideState(Tegra::Engines::Maxwell3D& primary, u64* out) {}

void ApplySideState(Tegra::Engines::Maxwell3D& replica, const u64* words, u32 count) {}

bool VerifyReplica(Tegra::Engines::Maxwell3D& primary, Stage stage) {
    return true;
}

} // namespace VideoCommon::Nx2Stages
