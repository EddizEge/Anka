// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"
#include "video_core/nx2_stages.h"

namespace Tegra {
class MemoryManager;
namespace Control {
struct ChannelState;
}
namespace Engines {
class Maxwell3D;
}
} // namespace Tegra

namespace VideoCore {
class RasterizerInterface;
}

namespace VideoCommon::Nx2RegReplica {
class RegCapture;
}

// Back stages: what the stages need of the channels and Maxwell3D between them (S14). Owner of
// the bodies: T2 (nx2_stage_api.cpp, maxwell_3d.*, control/channel_state*); callers: T1, T5, the
// integration. A channel is staged when it has its replicas and its engines' rasterizer is the
// proxy; only at =1 and =2 (at =3 none is, K1). Maxwell3D's stage hooks (ProcessDirtyRegisters'
// Note, ProcessCBMultiData's PostCbData, ProcessQueryCondition's join) test its stage_capture:
// set, it is a staged primary, which only A runs; never Nx2Stages::Active() (K1: at =3 BC runs the
// unstaged primaries with GpuSplit::Execute).
namespace VideoCommon::Nx2Stages {

/// Whether the channel is staged. Any thread (acquire): the front tags BindChannel's deferral
/// with it (T5: Join when not staged or another channel, Owner otherwise; S3, S9/1; while
/// Proxy() is null, stages off or =3, the deferral of today, K1).
bool ChannelStaged(const Tegra::Control::ChannelState& channel);

/// Stages parked (Start, or A in a join for a channel bound first after Start, S3): makes the
/// channel's replicas as copies of its primary Maxwell3D (registers, dirty flags,
/// state.shader_stages, draw_manager state, light, shadow_scratch; native_turing as the primary's,
/// no macro JIT; S18), points Maxwell3D's stage_capture at Capture(), and the rasterizer of its
/// engines and of its memory manager at `proxy` (SetEngineRasterizer). When the channel is the
/// one bound to the caches, binds them again through `real` (they take their Maxwell3D at
/// BindToChannel). Again for a staged channel: points its memory manager at `proxy` once more (an
/// address space bound in between, S3).
void StageChannel(Tegra::Control::ChannelState& channel, VideoCore::RasterizerInterface& proxy,
                  VideoCore::RasterizerInterface& real);
/// Stages stopped (Stop): engines and memory manager back to `real`, stage_capture null, the
/// caches bound again as above, the replicas freed; the primary's dirty flags all set,
/// light.armed false (S18).
void UnstageChannel(Tegra::Control::ChannelState& channel, VideoCore::RasterizerInterface& real);

/// ChannelState::BindRasterizer without DmaPusher's (it keeps the real one, D2): the engines'
/// and the channel's memory manager's rasterizer.
void SetEngineRasterizer(Tegra::Control::ChannelState& channel,
                         VideoCore::RasterizerInterface* rasterizer);

/// The Maxwell3D a cache of `role` reads for the channel: the primary while the stages are off
/// or the channel is not staged; else B's replica, or C's for Record with cut c.
/// ChannelSetupCaches<P>::BindToChannel takes it by the cache's role, set with
/// ChannelSetupCaches<P>::SetStageRole(Role) (control/channel_state_cache.h, T2; Record unless
/// set). K1 reads B's replica for both roles and sets none; K2 sets Memory for the buffer and
/// pipeline caches (T7). The StateTracker's flags are the Record engine's (T2).
Tegra::Engines::Maxwell3D& EngineFor(Tegra::Control::ChannelState& channel, Role role);
/// The replica of `primary` `stage` keeps (consumers of Target records); null when there is none.
Tegra::Engines::Maxwell3D* ReplicaOf(Tegra::Engines::Maxwell3D& primary, Stage stage);

/// A's register capture (one, for every staged Maxwell3D).
Nx2RegReplica::RegCapture& Capture();
/// Consumer of delta words (B: B's replica; C: C's): Nx2RegReplica::ApplyDelta with the replica's
/// light change notes.
void ApplyDelta(Tegra::Engines::Maxwell3D& replica, const u64* words, u32 count);
/// Consumer of a CbData record: ProcessCBMultiData's write on the replica (IsLightSameWrite,
/// SAME_SKIP, NoteLightWrite, WriteBlockCached; Maxwell3D::StageCbWrite).
void StageCbWrite(Tegra::Engines::Maxwell3D& replica, GPUVAddr address, const u32* data,
                  u32 count);

/// A: words of the side state parts that changed since they were last sent (0: none).
u32 SideStateWords(Tegra::Engines::Maxwell3D& primary);
/// A: writes them (SideStateWords words, Nx2RegReplica's format) and notes them sent.
void WriteSideState(Tegra::Engines::Maxwell3D& primary, u64* out);
/// Consumer: applies a side state to the replica (S2: ORs light.rebound in).
void ApplySideState(Tegra::Engines::Maxwell3D& replica, const u64* words, u32 count);

/// =2 (T1 calls it after a record while the producer waits): compares `stage`'s replica with the
/// primary (registers, cb binds, draw_state, engine_state, replace_table; light: rebound, armed,
/// same_writes, cb_writes and changes, S2); logs the first 16 differences, counts the rest
/// (Counter::VerifyMismatches). Returns whether they matched.
bool VerifyReplica(Tegra::Engines::Maxwell3D& primary, Stage stage);

} // namespace VideoCommon::Nx2Stages
