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
/// parked (JoinFromA); from any other thread it calls the real rasterizer. K1 classes (D5):
/// - Draw, DrawIndirect, DrawTexture: a draw record with the side state; BC applies it, then draws.
/// - Join: the members returning a value (MustFlushRegion, HoldsGpuWrittenData, GetFlushArea,
///   AccelerateSurfaceCopy, ...); AccessAccelerateDMA returns a wrapper whose methods join.
/// - The real one directly: device constants (AccelerateConditionalRendering,
///   HasDrawTransformFeedback), LoadDiskResources.
/// - Every other member (void): Post.
/// Skeleton (T0): every member calls the real rasterizer.
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

/// Start / Stop (under a Drain): the engines' and memory managers' rasterizer to the proxy and
/// back (SetEngineRasterizer, T2).
void InstallProxy(Core::System& system, VideoCore::RasterizerInterface& real);
void UninstallProxy(Core::System& system);

} // namespace VideoCommon::Nx2Stages
