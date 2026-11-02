// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/control/channel_state.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_stage_proxy.h"

// Skeleton (T0): every member calls the real rasterizer, and the proxy is never installed. The
// forwarding (Post / Join) is T5's.
namespace VideoCommon::Nx2Stages {

Nx2StageProxy::Nx2StageProxy(VideoCore::RasterizerInterface& real_) : real{real_} {}

Nx2StageProxy::~Nx2StageProxy() = default;

void Nx2StageProxy::Draw(bool is_indexed, u32 instance_count) {
    real.Draw(is_indexed, instance_count);
}

void Nx2StageProxy::DrawIndirect() {
    real.DrawIndirect();
}

void Nx2StageProxy::DrawTexture() {
    real.DrawTexture();
}

void Nx2StageProxy::Clear(u32 layer_count) {
    real.Clear(layer_count);
}

void Nx2StageProxy::DispatchCompute() {
    real.DispatchCompute();
}

void Nx2StageProxy::ResetCounter(VideoCommon::QueryType type) {
    real.ResetCounter(type);
}

void Nx2StageProxy::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                          VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    real.Query(gpu_addr, type, flags, payload, subreport);
}

void Nx2StageProxy::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                              u32 size) {
    real.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}

void Nx2StageProxy::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    real.DisableGraphicsUniformBuffer(stage, index);
}

void Nx2StageProxy::SignalFence(std::function<void()>&& func) {
    real.SignalFence(std::move(func));
}

void Nx2StageProxy::SyncOperation(std::function<void()>&& func) {
    real.SyncOperation(std::move(func));
}

void Nx2StageProxy::SignalSyncPoint(u32 value) {
    real.SignalSyncPoint(value);
}

void Nx2StageProxy::SignalReference() {
    real.SignalReference();
}

void Nx2StageProxy::ReleaseFences(bool force) {
    real.ReleaseFences(force);
}

void Nx2StageProxy::FlushAll() {
    real.FlushAll();
}

void Nx2StageProxy::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    real.FlushRegion(addr, size, which);
}

bool Nx2StageProxy::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    return real.MustFlushRegion(addr, size, which);
}

bool Nx2StageProxy::HoldsGpuWrittenData(DAddr addr, u64 size) {
    return real.HoldsGpuWrittenData(addr, size);
}

void Nx2StageProxy::NoteEngineWork() {
    real.NoteEngineWork();
}

u64 Nx2StageProxy::WorkEpoch() const noexcept {
    return real.WorkEpoch();
}

VideoCore::RasterizerDownloadArea Nx2StageProxy::GetFlushArea(DAddr addr, u64 size) {
    return real.GetFlushArea(addr, size);
}

void Nx2StageProxy::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    real.InvalidateRegion(addr, size, which);
}

void Nx2StageProxy::InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
    real.InnerInvalidation(sequences);
}

void Nx2StageProxy::OnCacheInvalidation(PAddr addr, u64 size) {
    real.OnCacheInvalidation(addr, size);
}

void Nx2StageProxy::OnCachedWrite(DAddr addr, u64 size) {
    real.OnCachedWrite(addr, size);
}

std::optional<u32> Nx2StageProxy::PendingSemaphoreRelease(DAddr addr) {
    return real.PendingSemaphoreRelease(addr);
}

u32 Nx2StageProxy::BoundGeometryBuffer(u32 index) {
    return real.BoundGeometryBuffer(index);
}

bool Nx2StageProxy::TryCacheInvalidation(DAddr addr, u64 size) {
    return real.TryCacheInvalidation(addr, size);
}

bool Nx2StageProxy::OnCPUWrite(PAddr addr, u64 size) {
    return real.OnCPUWrite(addr, size);
}

void Nx2StageProxy::InvalidateGPUCache() {
    real.InvalidateGPUCache();
}

void Nx2StageProxy::UnmapMemory(DAddr addr, u64 size) {
    real.UnmapMemory(addr, size);
}

void Nx2StageProxy::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    real.ModifyGPUMemory(as_id, addr, size);
}

void Nx2StageProxy::FlushAndInvalidateRegion(DAddr addr, u64 size,
                                             VideoCommon::CacheType which) {
    real.FlushAndInvalidateRegion(addr, size, which);
}

void Nx2StageProxy::WaitForIdle() {
    real.WaitForIdle();
}

void Nx2StageProxy::FragmentBarrier() {
    real.FragmentBarrier();
}

void Nx2StageProxy::TiledCacheBarrier() {
    real.TiledCacheBarrier();
}

void Nx2StageProxy::FlushCommands() {
    real.FlushCommands();
}

void Nx2StageProxy::TickFrame() {
    real.TickFrame();
}

bool Nx2StageProxy::AccelerateConditionalRendering() {
    return real.AccelerateConditionalRendering();
}

bool Nx2StageProxy::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                          const Tegra::Engines::Fermi2D::Surface& dst,
                                          const Tegra::Engines::Fermi2D::Config& copy_config) {
    return real.AccelerateSurfaceCopy(src, dst, copy_config);
}

Tegra::Engines::AccelerateDMAInterface& Nx2StageProxy::AccessAccelerateDMA() {
    return real.AccessAccelerateDMA();
}

void Nx2StageProxy::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                             std::span<const u8> memory) {
    real.AccelerateInlineToMemory(address, copy_size, memory);
}

void Nx2StageProxy::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    real.LoadDiskResources(title_id, stop_loading, callback);
}

void Nx2StageProxy::InitializeChannel(Tegra::Control::ChannelState& channel) {
    real.InitializeChannel(channel);
}

void Nx2StageProxy::BindChannel(Tegra::Control::ChannelState& channel) {
    real.BindChannel(channel);
}

void Nx2StageProxy::ReleaseChannel(s32 channel_id) {
    real.ReleaseChannel(channel_id);
}

void Nx2StageProxy::RegisterTransformFeedback(GPUVAddr tfb_object_addr) {
    real.RegisterTransformFeedback(tfb_object_addr);
}

bool Nx2StageProxy::HasDrawTransformFeedback() {
    return real.HasDrawTransformFeedback();
}

void InstallProxy(Core::System& system, VideoCore::RasterizerInterface& real) {}

void UninstallProxy(Core::System& system) {}

Nx2StageProxy* Proxy() {
    return nullptr;
}

void BindNewChannel(Tegra::Control::ChannelState& channel, VideoCore::RasterizerInterface& real) {
    channel.BindRasterizer(&real);
}

void BindNewAddressSpace(Tegra::MemoryManager& memory_manager,
                         VideoCore::RasterizerInterface& real) {
    memory_manager.BindRasterizer(&real);
}

} // namespace VideoCommon::Nx2Stages
