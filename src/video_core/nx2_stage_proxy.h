// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/rasterizer_interface.h"

namespace Core {
class System;
}

namespace VideoCommon::Nx2Stages {

/// Back stages (nx2_stages.h), cut a: the rasterizer of the engines and of the channels' memory
/// managers while staged (DmaPusher keeps the real one, D2). Owner: T5. From A outside a join
/// (Forwarding()) a member either goes to BC as a record (Post: the arguments copied, spans' bytes
/// with them, std::function moved to the heap as DeferFunction does) or runs on A with the stages
/// parked (JoinFromA); from any other thread it calls the real rasterizer. K1 classes:
/// - Draw, DrawIndirect, DrawTexture, Clear: a draw record, the side state that changed before it
///   (WriteSideState, nx2_stage_api.h) of the drawing primary, GPU::Maxwell3D() on A (K10); BC
///   applies it to that primary's replica, then draws.
/// - Join: the members returning a value (MustFlushRegion, HoldsGpuWrittenData, GetFlushArea,
///   PendingSemaphoreRelease, TryCacheInvalidation, OnCPUWrite, AccelerateSurfaceCopy,
///   WorkEpoch, BoundGeometryBuffer); FlushRegion, FlushAll, FlushAndInvalidateRegion (S7);
///   AccelerateConditionalRendering (S1); InitializeChannel, BindChannel, ReleaseChannel.
/// - The real one directly: HasDrawTransformFeedback (a device constant), LoadDiskResources.
/// - Only expected in a join (ring level for the engines but Maxwell3D), reported from A outside
///   one (STAGE_CHECK, S7): AccessAccelerateDMA (then the real one), InvalidateRegion and
///   DispatchCompute (then a join).
/// - Every other member: Post (AccelerateInlineToMemory and InnerInvalidation with their data).
/// A member whose record would pass MaxBodyWords (nx2_stage_ring.h, K9) runs in a join instead:
/// AccelerateInlineToMemory's data (Upload), a draw whose side state is too long (Other).
class Nx2StageProxy final : public VideoCore::RasterizerInterface {
public:
    explicit Nx2StageProxy(VideoCore::RasterizerInterface& real_);
    ~Nx2StageProxy() override;

    [[nodiscard]] VideoCore::RasterizerInterface& Real() const {
        return real;
    }

    void Draw(bool is_indexed, u32 instance_count) override;
    void DrawIndirect() override;
    void DrawTexture() override;
    void Clear(u32 layer_count) override;
    void DispatchCompute() override;
    void ResetCounter(VideoCommon::QueryType type) override;
    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override;
    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size) override;
    void DisableGraphicsUniformBuffer(size_t stage, u32 index) override;
    void SignalFence(std::function<void()>&& func) override;
    void SyncOperation(std::function<void()>&& func) override;
    void SignalSyncPoint(u32 value) override;
    void SignalReference() override;
    void ReleaseFences(bool force = true) override;
    void FlushAll() override;
    void FlushRegion(DAddr addr, u64 size,
                     VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    bool MustFlushRegion(DAddr addr, u64 size,
                         VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    bool HoldsGpuWrittenData(DAddr addr, u64 size) override;
    void NoteEngineWork() override;
    [[nodiscard]] u64 WorkEpoch() const noexcept override;
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override;
    void InvalidateRegion(DAddr addr, u64 size,
                          VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) override;
    void OnCacheInvalidation(PAddr addr, u64 size) override;
    void OnCachedWrite(DAddr addr, u64 size) override;
    std::optional<u32> PendingSemaphoreRelease(DAddr addr) override;
    u32 BoundGeometryBuffer(u32 index) override;
    bool TryCacheInvalidation(DAddr addr, u64 size) override;
    bool OnCPUWrite(PAddr addr, u64 size) override;
    void InvalidateGPUCache() override;
    void UnmapMemory(DAddr addr, u64 size) override;
    void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) override;
    void FlushAndInvalidateRegion(
        DAddr addr, u64 size, VideoCommon::CacheType which = VideoCommon::CacheType::All) override;
    void WaitForIdle() override;
    void FragmentBarrier() override;
    void TiledCacheBarrier() override;
    void FlushCommands() override;
    void TickFrame() override;
    bool AccelerateConditionalRendering() override;
    [[nodiscard]] bool AccelerateSurfaceCopy(
        const Tegra::Engines::Fermi2D::Surface& src, const Tegra::Engines::Fermi2D::Surface& dst,
        const Tegra::Engines::Fermi2D::Config& copy_config) override;
    [[nodiscard]] Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override;
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override;
    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback) override;
    void InitializeChannel(Tegra::Control::ChannelState& channel) override;
    void BindChannel(Tegra::Control::ChannelState& channel) override;
    void ReleaseChannel(s32 channel_id) override;
    void RegisterTransformFeedback(GPUVAddr tfb_object_addr) override;
    bool HasDrawTransformFeedback() override;

private:
    VideoCore::RasterizerInterface& real;
};

/// Start at =1 and =2, never at =3 (K1; nothing runs behind the front): makes the proxy for
/// `real`, publishes it (Proxy()), then stages every channel the GPU has initialized, not only
/// the bound one (StageChannel; S3). T5 enumerates them (Control::Scheduler's channels); one
/// without its payload yet is left to BindNewChannel. Holds the lock BindNewChannel and
/// BindNewAddressSpace take, so a channel the guest initializes meanwhile is never left on the
/// real rasterizer (K7).
void InstallProxy(Core::System& system, VideoCore::RasterizerInterface& real);
/// Stop (the stages stopped): unstages every channel, frees the proxy; under the same lock.
void UninstallProxy(Core::System& system);
/// The proxy while installed (=1 and =2, Start to Stop), else null (stages off, =3). Any thread
/// (acquire). A channel bound first after Start is staged in its BindChannel join (S3). The front
/// tags BindChannel as before (Defer, the same thunk) while it is null (T5, K1).
Nx2StageProxy* Proxy();

/// GPU::Impl::InitChannel's binding (gpu.cpp, T5; K7), on a guest CPU thread while A may run a
/// staged channel with the same address space. While the proxy is installed: DmaPusher to `real`
/// (D2), the engines and the memory manager to the proxy (SetEngineRasterizer); never to `real`
/// first, the shared memory manager would send A's calls past the proxy. Else
/// ChannelState::BindRasterizer(&real) as before. Under InstallProxy's lock. The channel itself
/// is staged at its first BindChannel join (S3).
void BindNewChannel(Tegra::Control::ChannelState& channel, VideoCore::RasterizerInterface& real);
/// GPU::Impl::InitAddressSpace's binding (gpu.cpp, T5; K7): the proxy while installed, else
/// `real` as before. Under InstallProxy's lock.
void BindNewAddressSpace(Tegra::MemoryManager& memory_manager,
                         VideoCore::RasterizerInterface& real);

/// A, in the join of a channel's first bind in a split session or of a channel change
/// (Control::Scheduler, S3): stages the channel if it is not; if it is, points its engines and
/// memory manager at the proxy again (an address space the channel was given since). Nothing
/// without the proxy.
void StageBoundChannel(Tegra::Control::ChannelState& channel);

} // namespace VideoCommon::Nx2Stages
