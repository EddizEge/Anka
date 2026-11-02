// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/logging.h"
#include "core/core.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/scheduler.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_mode_diag.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_stage_api.h"
#include "video_core/nx2_stage_proxy.h"
#include "video_core/nx2_stages.h"

namespace VideoCommon::Nx2Stages {

namespace {

// Set by InstallProxy before A starts, reset by UninstallProxy once the stages stopped: the
// stages read them as they run records. g_published is Proxy() (acquire, any thread), null
// while not installed. The proxy itself is never freed (R1 sahiplik-1): an engine, memory
// manager or a call that read the pointer before UninstallProxy may still reach it, and with no
// A running it calls the real rasterizer. InstallProxy uses it again for the same real one and
// retires it for another (a new renderer).
std::unique_ptr<Nx2StageProxy> g_proxy;
std::vector<std::unique_ptr<Nx2StageProxy>> g_retired;
std::atomic<Nx2StageProxy*> g_published{};
VideoCore::RasterizerInterface* g_real{};
Core::System* g_system{};
bool g_check{};
/// InstallProxy / UninstallProxy against BindNewChannel / BindNewAddressSpace (guest threads, K7).
std::mutex g_install_mutex;

/// Most words of an inline write's data in a record (its thunk and three words besides; K9).
constexpr size_t MaxInlineWords = MaxBodyWords - 4;
/// Most ranges of an invalidation in one record (larger ones go as several).
constexpr size_t MaxRanges = 4096;
static_assert(1 + MaxRanges * 2 <= MaxBodyWords);

/// BC's side of a record: the real rasterizer (checked to run on B under STAGE_CHECK).
VideoCore::RasterizerInterface& OnBc(const char* where) {
    if (g_check) [[unlikely]] {
        CheckStage(Stage::B, where);
    }
    return *g_real;
}

/// A channel or replica missing where the proxy expects it staged (STAGE_CHECK; S3).
void ReportUnstaged(const char* what, s32 bind_id) {
    Nx2Profile::Add(Nx2Profile::Counter::StageUnproxied, 1);
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 16) {
        LOG_ERROR(HW_GPU, "Back stages: channel {} {}", bind_id, what);
    }
}

void CheckStaged(Tegra::Control::ChannelState& channel) {
    if (!ChannelStaged(channel)) {
        ReportUnstaged("is not staged", channel.bind_id);
        return;
    }
    auto& maxwell_3d = channel.payload->maxwell_3d;
    if (&maxwell_3d.Rasterizer() != Proxy()) {
        ReportUnstaged("has Maxwell3D on the real rasterizer", channel.bind_id);
    }
    if (ReplicaOf(maxwell_3d, Stage::B) == nullptr) {
        ReportUnstaged("has no replica for B", channel.bind_id);
    }
}

/// What a draw-like record runs on BC, after the side state that changed before it.
enum class Event : u8 { Draw, DrawIndirect, DrawTexture, Clear };

/// Word 0 of a draw-like record: bits 0-7 the event, bit 8 indexed, bits 32-63 the instance or
/// layer count. Word 1 the primary Maxwell3D, then its side state (WriteSideState).
constexpr u64 EventWord(Event event, bool is_indexed, u32 count) {
    return static_cast<u64>(event) | (is_indexed ? u64{1} << 8 : 0) | (u64{count} << 32);
}

void RunEvent(VideoCore::RasterizerInterface& real, Event event, bool is_indexed, u32 count) {
    switch (event) {
    case Event::Draw:
        return real.Draw(is_indexed, count);
    case Event::DrawIndirect:
        return real.DrawIndirect();
    case Event::DrawTexture:
        return real.DrawTexture();
    case Event::Clear:
        return real.Clear(count);
    }
}

void EventThunk(const u64* payload, u32 words) {
    auto& real = OnBc("Nx2StageProxy draw");
    if (words > 2) {
        Nx2Profile::Scope scope{Nx2Profile::Zone::ReplicaApply};
        auto* const primary = reinterpret_cast<Tegra::Engines::Maxwell3D*>(payload[1]);
        if (auto* const replica = ReplicaOf(*primary, Stage::B)) [[likely]] {
            ApplySideState(*replica, payload + 2, words - 2);
        } else {
            ReportUnstaged("drew without a replica for B", -1);
        }
    }
    const u64 word = payload[0];
    RunEvent(real, static_cast<Event>(word & 0xff), ((word >> 8) & 1) != 0,
             static_cast<u32>(word >> 32));
}

/// A: a draw-like record with the side state of the bound channel's Maxwell3D that changed since
/// it was last sent (S5/2). Channel changes run on A (Control::Scheduler's join), so the GPU's
/// current channel is the one whose methods A runs.
void PostEvent(Event event, bool is_indexed, u32 count) {
    Nx2Profile::Scope scope{Nx2Profile::Zone::PacketBuild};
    auto& primary = g_system->GPU().Maxwell3D();
    const u32 side_words = SideStateWords(primary);
    if (1 + 2 + side_words > MaxBodyWords) [[unlikely]] {
        // Too long for a record (K9): applied to the replica and drawn with the stages parked.
        std::vector<u64> side(side_words);
        WriteSideState(primary, side.data());
        JoinFromA(JoinReason::Other, [&] {
            if (auto* const replica = ReplicaOf(primary, Stage::B)) {
                ApplySideState(*replica, side.data(), side_words);
            } else {
                ReportUnstaged("drew without a replica for B", -1);
            }
            RunEvent(*g_real, event, is_indexed, count);
        });
        return;
    }
    u64* const payload = Reserve(Ring::ToB, &EventThunk, 2 + side_words);
    payload[0] = EventWord(event, is_indexed, count);
    payload[1] = reinterpret_cast<u64>(&primary);
    if (side_words != 0) {
        WriteSideState(primary, payload + 2);
    }
    Commit(Ring::ToB);
}

/// Words 0-2: GPU address, copy size, bytes; then the bytes.
void InlineThunk(const u64* payload, [[maybe_unused]] u32 words) {
    OnBc("Nx2StageProxy::AccelerateInlineToMemory")
        .AccelerateInlineToMemory(
            payload[0], static_cast<size_t>(payload[1]),
            std::span<const u8>{reinterpret_cast<const u8*>(payload + 3),
                                static_cast<size_t>(payload[2])});
}

void PostInline(GPUVAddr address, size_t copy_size, std::span<const u8> memory) {
    const u32 data_words = static_cast<u32>((memory.size() + sizeof(u64) - 1) / sizeof(u64));
    u64* const payload = Reserve(Ring::ToB, &InlineThunk, 3 + data_words);
    payload[0] = address;
    payload[1] = copy_size;
    payload[2] = memory.size();
    if (data_words != 0) {
        payload[2 + data_words] = 0;
        std::memcpy(payload + 3, memory.data(), memory.size());
    }
    Commit(Ring::ToB);
}

/// Two words a range: address, size.
void InvalidationThunk(const u64* payload, u32 words) {
    thread_local std::vector<std::pair<DAddr, std::size_t>> ranges;
    ranges.clear();
    for (u32 i = 0; i + 1 < words; i += 2) {
        ranges.emplace_back(payload[i], static_cast<std::size_t>(payload[i + 1]));
    }
    OnBc("Nx2StageProxy::InnerInvalidation").InnerInvalidation(ranges);
}

} // Anonymous namespace

Nx2StageProxy::Nx2StageProxy(VideoCore::RasterizerInterface& real_) : real{real_} {}

Nx2StageProxy::~Nx2StageProxy() = default;

void Nx2StageProxy::Draw(bool is_indexed, u32 instance_count) {
    if (!Forwarding()) {
        return real.Draw(is_indexed, instance_count);
    }
    PostEvent(Event::Draw, is_indexed, instance_count);
}

void Nx2StageProxy::DrawIndirect() {
    if (!Forwarding()) {
        return real.DrawIndirect();
    }
    PostEvent(Event::DrawIndirect, false, 0);
}

void Nx2StageProxy::DrawTexture() {
    if (!Forwarding()) {
        return real.DrawTexture();
    }
    PostEvent(Event::DrawTexture, false, 0);
}

void Nx2StageProxy::Clear(u32 layer_count) {
    if (!Forwarding()) {
        return real.Clear(layer_count);
    }
    // Draw-like: it reads the render targets' registers and dirty flags of the replica.
    PostEvent(Event::Clear, false, layer_count);
}

void Nx2StageProxy::DispatchCompute() {
    if (!Forwarding()) {
        return real.DispatchCompute();
    }
    // KeplerCompute has no replica: its launches run in a ring level join (T1). From A outside one
    // it reads registers A goes on writing.
    CheckStage(Stage::B, "Nx2StageProxy::DispatchCompute");
    JoinFromA(JoinReason::Other, [this] { real.DispatchCompute(); });
}

void Nx2StageProxy::ResetCounter(VideoCommon::QueryType type) {
    if (!Forwarding()) {
        return real.ResetCounter(type);
    }
    Post(Ring::ToB, [type] { OnBc("Nx2StageProxy::ResetCounter").ResetCounter(type); });
}

void Nx2StageProxy::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                          VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    if (!Forwarding()) {
        return real.Query(gpu_addr, type, flags, payload, subreport);
    }
    Post(Ring::ToB, [gpu_addr, type, flags, payload, subreport, source = Nx2ModeDiag::g_source] {
        Nx2ModeDiag::SourceScope diag_source{source};
        OnBc("Nx2StageProxy::Query").Query(gpu_addr, type, flags, payload, subreport);
    });
}

void Nx2StageProxy::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                              u32 size) {
    if (!Forwarding()) {
        return real.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
    }
    Post(Ring::ToB, [stage, index, gpu_addr, size] {
        OnBc("Nx2StageProxy::BindGraphicsUniformBuffer")
            .BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
    });
}

void Nx2StageProxy::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    if (!Forwarding()) {
        return real.DisableGraphicsUniformBuffer(stage, index);
    }
    Post(Ring::ToB, [stage, index] {
        OnBc("Nx2StageProxy::DisableGraphicsUniformBuffer")
            .DisableGraphicsUniformBuffer(stage, index);
    });
}

void Nx2StageProxy::SignalFence(std::function<void()>&& func) {
    if (!Forwarding()) {
        return real.SignalFence(std::move(func));
    }
    auto* const heap = new std::function<void()>(std::move(func));
    Post(Ring::ToB, [heap] {
        OnBc("Nx2StageProxy::SignalFence").SignalFence(std::move(*heap));
        delete heap;
    });
}

void Nx2StageProxy::SyncOperation(std::function<void()>&& func) {
    if (!Forwarding()) {
        return real.SyncOperation(std::move(func));
    }
    auto* const heap = new std::function<void()>(std::move(func));
    Post(Ring::ToB, [heap] {
        OnBc("Nx2StageProxy::SyncOperation").SyncOperation(std::move(*heap));
        delete heap;
    });
}

void Nx2StageProxy::SignalSyncPoint(u32 value) {
    if (!Forwarding()) {
        return real.SignalSyncPoint(value);
    }
    Post(Ring::ToB, [value] { OnBc("Nx2StageProxy::SignalSyncPoint").SignalSyncPoint(value); });
}

void Nx2StageProxy::SignalReference() {
    if (!Forwarding()) {
        return real.SignalReference();
    }
    Post(Ring::ToB, [] { OnBc("Nx2StageProxy::SignalReference").SignalReference(); });
}

void Nx2StageProxy::ReleaseFences(bool force) {
    if (!Forwarding()) {
        return real.ReleaseFences(force);
    }
    Post(Ring::ToB, [force] { OnBc("Nx2StageProxy::ReleaseFences").ReleaseFences(force); });
}

void Nx2StageProxy::FlushAll() {
    if (!Forwarding()) {
        return real.FlushAll();
    }
    JoinFromA(JoinReason::Flush, [this] { real.FlushAll(); });
}

void Nx2StageProxy::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (!Forwarding()) {
        return real.FlushRegion(addr, size, which);
    }
    // The caller reads guest memory right after (MemoryManager::ReadBlock; S7).
    JoinFromA(JoinReason::Flush, [&] { real.FlushRegion(addr, size, which); });
}

bool Nx2StageProxy::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (!Forwarding()) {
        return real.MustFlushRegion(addr, size, which);
    }
    bool result{};
    JoinFromA(JoinReason::Value, [&] { result = real.MustFlushRegion(addr, size, which); });
    return result;
}

bool Nx2StageProxy::HoldsGpuWrittenData(DAddr addr, u64 size) {
    if (!Forwarding()) {
        return real.HoldsGpuWrittenData(addr, size);
    }
    bool result{};
    JoinFromA(JoinReason::Value, [&] { result = real.HoldsGpuWrittenData(addr, size); });
    return result;
}

void Nx2StageProxy::NoteEngineWork() {
    if (!Forwarding()) {
        return real.NoteEngineWork();
    }
    Post(Ring::ToB, [] { OnBc("Nx2StageProxy::NoteEngineWork").NoteEngineWork(); });
}

u64 Nx2StageProxy::WorkEpoch() const noexcept {
    if (!Forwarding()) {
        return real.WorkEpoch();
    }
    u64 result{};
    JoinFromA(JoinReason::Value, [&] { result = real.WorkEpoch(); });
    return result;
}

VideoCore::RasterizerDownloadArea Nx2StageProxy::GetFlushArea(DAddr addr, u64 size) {
    if (!Forwarding()) {
        return real.GetFlushArea(addr, size);
    }
    VideoCore::RasterizerDownloadArea result{};
    JoinFromA(JoinReason::Value, [&] { result = real.GetFlushArea(addr, size); });
    return result;
}

void Nx2StageProxy::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (!Forwarding()) {
        return real.InvalidateRegion(addr, size, which);
    }
    // MemoryManager::WriteBlock writes guest memory right after (S7): only expected in a join.
    CheckStage(Stage::B, "Nx2StageProxy::InvalidateRegion");
    JoinFromA(JoinReason::Other, [&] { real.InvalidateRegion(addr, size, which); });
}

void Nx2StageProxy::InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
    if (!Forwarding()) {
        return real.InnerInvalidation(sequences);
    }
    for (size_t first = 0; first < sequences.size(); first += MaxRanges) {
        const size_t count = (std::min)(sequences.size() - first, MaxRanges);
        u64* const payload =
            Reserve(Ring::ToB, &InvalidationThunk, static_cast<u32>(count * 2));
        for (size_t i = 0; i < count; ++i) {
            payload[i * 2] = sequences[first + i].first;
            payload[i * 2 + 1] = sequences[first + i].second;
        }
        Commit(Ring::ToB);
    }
}

void Nx2StageProxy::OnCacheInvalidation(PAddr addr, u64 size) {
    if (!Forwarding()) {
        return real.OnCacheInvalidation(addr, size);
    }
    Post(Ring::ToB, [addr, size] {
        OnBc("Nx2StageProxy::OnCacheInvalidation").OnCacheInvalidation(addr, size);
    });
}

void Nx2StageProxy::OnCachedWrite(DAddr addr, u64 size) {
    if (!Forwarding()) {
        return real.OnCachedWrite(addr, size);
    }
    Post(Ring::ToB,
         [addr, size] { OnBc("Nx2StageProxy::OnCachedWrite").OnCachedWrite(addr, size); });
}

std::optional<u32> Nx2StageProxy::PendingSemaphoreRelease(DAddr addr) {
    if (!Forwarding()) {
        return real.PendingSemaphoreRelease(addr);
    }
    std::optional<u32> result;
    JoinFromA(JoinReason::Value, [&] { result = real.PendingSemaphoreRelease(addr); });
    return result;
}

u32 Nx2StageProxy::BoundGeometryBuffer(u32 index) {
    if (!Forwarding()) {
        return real.BoundGeometryBuffer(index);
    }
    u32 result{};
    JoinFromA(JoinReason::Value, [&] { result = real.BoundGeometryBuffer(index); });
    return result;
}

bool Nx2StageProxy::TryCacheInvalidation(DAddr addr, u64 size) {
    if (!Forwarding()) {
        return real.TryCacheInvalidation(addr, size);
    }
    bool result{};
    JoinFromA(JoinReason::Value, [&] { result = real.TryCacheInvalidation(addr, size); });
    return result;
}

bool Nx2StageProxy::OnCPUWrite(PAddr addr, u64 size) {
    if (!Forwarding()) {
        return real.OnCPUWrite(addr, size);
    }
    bool result{};
    JoinFromA(JoinReason::Value, [&] { result = real.OnCPUWrite(addr, size); });
    return result;
}

void Nx2StageProxy::InvalidateGPUCache() {
    if (!Forwarding()) {
        return real.InvalidateGPUCache();
    }
    Post(Ring::ToB, [] { OnBc("Nx2StageProxy::InvalidateGPUCache").InvalidateGPUCache(); });
}

void Nx2StageProxy::UnmapMemory(DAddr addr, u64 size) {
    if (!Forwarding()) {
        return real.UnmapMemory(addr, size);
    }
    Post(Ring::ToB, [addr, size] { OnBc("Nx2StageProxy::UnmapMemory").UnmapMemory(addr, size); });
}

void Nx2StageProxy::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    if (!Forwarding()) {
        return real.ModifyGPUMemory(as_id, addr, size);
    }
    Post(Ring::ToB, [as_id, addr, size] {
        OnBc("Nx2StageProxy::ModifyGPUMemory").ModifyGPUMemory(as_id, addr, size);
    });
}

void Nx2StageProxy::FlushAndInvalidateRegion(DAddr addr, u64 size,
                                             VideoCommon::CacheType which) {
    if (!Forwarding()) {
        return real.FlushAndInvalidateRegion(addr, size, which);
    }
    JoinFromA(JoinReason::Flush, [&] { real.FlushAndInvalidateRegion(addr, size, which); });
}

void Nx2StageProxy::WaitForIdle() {
    if (!Forwarding()) {
        return real.WaitForIdle();
    }
    Post(Ring::ToB, [] { OnBc("Nx2StageProxy::WaitForIdle").WaitForIdle(); });
}

void Nx2StageProxy::FragmentBarrier() {
    if (!Forwarding()) {
        return real.FragmentBarrier();
    }
    Post(Ring::ToB, [] { OnBc("Nx2StageProxy::FragmentBarrier").FragmentBarrier(); });
}

void Nx2StageProxy::TiledCacheBarrier() {
    if (!Forwarding()) {
        return real.TiledCacheBarrier();
    }
    Post(Ring::ToB, [] { OnBc("Nx2StageProxy::TiledCacheBarrier").TiledCacheBarrier(); });
}

void Nx2StageProxy::FlushCommands() {
    if (!Forwarding()) {
        return real.FlushCommands();
    }
    Post(Ring::ToB, [] { OnBc("Nx2StageProxy::FlushCommands").FlushCommands(); });
}

void Nx2StageProxy::TickFrame() {
    if (!Forwarding()) {
        return real.TickFrame();
    }
    Post(Ring::ToB, [] { OnBc("Nx2StageProxy::TickFrame").TickFrame(); });
}

bool Nx2StageProxy::AccelerateConditionalRendering() {
    if (!Forwarding()) {
        return real.AccelerateConditionalRendering();
    }
    // FlushCaching and the query cache, not a device constant (S1).
    bool result{};
    JoinFromA(JoinReason::Condition, [&] { result = real.AccelerateConditionalRendering(); });
    return result;
}

bool Nx2StageProxy::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                          const Tegra::Engines::Fermi2D::Surface& dst,
                                          const Tegra::Engines::Fermi2D::Config& copy_config) {
    if (!Forwarding()) {
        return real.AccelerateSurfaceCopy(src, dst, copy_config);
    }
    bool result{};
    JoinFromA(JoinReason::Value,
              [&] { result = real.AccelerateSurfaceCopy(src, dst, copy_config); });
    return result;
}

Tegra::Engines::AccelerateDMAInterface& Nx2StageProxy::AccessAccelerateDMA() {
    // MaxwellDMA's executable methods run in a ring level join; what it does with the interface
    // after this returns cannot be forwarded (S7).
    if (Forwarding()) {
        CheckStage(Stage::B, "Nx2StageProxy::AccessAccelerateDMA");
    }
    return real.AccessAccelerateDMA();
}

void Nx2StageProxy::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                             std::span<const u8> memory) {
    if (!Forwarding()) {
        return real.AccelerateInlineToMemory(address, copy_size, memory);
    }
    // The bytes go with the record; more than a record holds runs parked (K9).
    if ((memory.size() + sizeof(u64) - 1) / sizeof(u64) > MaxInlineWords) [[unlikely]] {
        JoinFromA(JoinReason::Upload,
                  [&] { real.AccelerateInlineToMemory(address, copy_size, memory); });
        return;
    }
    PostInline(address, copy_size, memory);
}

void Nx2StageProxy::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    real.LoadDiskResources(title_id, stop_loading, callback);
}

void Nx2StageProxy::InitializeChannel(Tegra::Control::ChannelState& channel) {
    if (!Forwarding()) {
        return real.InitializeChannel(channel);
    }
    JoinFromA(JoinReason::Channel, [&] { real.InitializeChannel(channel); });
}

void Nx2StageProxy::BindChannel(Tegra::Control::ChannelState& channel) {
    if (!Forwarding()) {
        return real.BindChannel(channel);
    }
    JoinFromA(JoinReason::Channel, [&] { real.BindChannel(channel); });
}

void Nx2StageProxy::ReleaseChannel(s32 channel_id) {
    if (!Forwarding()) {
        return real.ReleaseChannel(channel_id);
    }
    JoinFromA(JoinReason::Channel, [&] { real.ReleaseChannel(channel_id); });
}

void Nx2StageProxy::RegisterTransformFeedback(GPUVAddr tfb_object_addr) {
    if (!Forwarding()) {
        return real.RegisterTransformFeedback(tfb_object_addr);
    }
    Post(Ring::ToB, [tfb_object_addr] {
        OnBc("Nx2StageProxy::RegisterTransformFeedback").RegisterTransformFeedback(tfb_object_addr);
    });
}

bool Nx2StageProxy::HasDrawTransformFeedback() {
    return real.HasDrawTransformFeedback();
}

void InstallProxy(Core::System& system, VideoCore::RasterizerInterface& real) {
    // =3 (pass-through): BC runs ring 1 as the unstaged back does, on the primaries (K1; Start
    // does not call it there either).
    if (Get().mode == 3) {
        return;
    }
    if (Proxy() != nullptr) [[unlikely]] {
        LOG_ERROR(HW_GPU, "Back stages: the proxy was installed already");
        UninstallProxy(system);
    }
    std::scoped_lock lock{g_install_mutex};
    if (!g_proxy || &g_proxy->Real() != &real) {
        if (g_proxy) {
            g_retired.push_back(std::move(g_proxy));
        }
        g_proxy = std::make_unique<Nx2StageProxy>(real);
    }
    g_real = &real;
    g_system = &system;
    g_check = Get().check;
    g_published.store(g_proxy.get(), std::memory_order_release);
    auto* const scheduler = Tegra::Control::Scheduler::Current();
    if (scheduler == nullptr) {
        return;
    }
    // A channel the guest initializes from here on is bound to the proxy (BindNewChannel) and
    // staged in its first bind's join.
    for (const auto& channel : scheduler->Channels()) {
        if (channel->initialized) {
            StageChannel(*channel, *g_proxy, real);
            if (g_check) {
                CheckStaged(*channel);
            }
        }
    }
}

void UninstallProxy(Core::System& system) {
    std::scoped_lock lock{g_install_mutex};
    if (Proxy() == nullptr) {
        return;
    }
    g_published.store(nullptr, std::memory_order_release);
    // Every initialized channel, staged or only bound to the proxy (BindNewChannel), back on the
    // real rasterizer, with its current memory manager. An address space BindNewAddressSpace
    // bound that no channel holds stays on the proxy, which outlives it (above).
    if (auto* const scheduler = Tegra::Control::Scheduler::Current()) {
        for (const auto& channel : scheduler->Channels()) {
            if (!channel->initialized || !channel->payload) {
                continue;
            }
            if (ChannelStaged(*channel)) {
                UnstageChannel(*channel, *g_real);
            } else {
                SetEngineRasterizer(*channel, g_real);
            }
        }
    }
    g_real = nullptr;
    g_system = nullptr;
    g_check = false;
}

Nx2StageProxy* Proxy() {
    return g_published.load(std::memory_order_acquire);
}

void BindNewChannel(Tegra::Control::ChannelState& channel, VideoCore::RasterizerInterface& real) {
    std::scoped_lock lock{g_install_mutex};
    auto* const proxy = Proxy();
    if (proxy == nullptr) {
        channel.BindRasterizer(&real);
        return;
    }
    channel.payload->dma_pusher.BindRasterizer(&real);
    SetEngineRasterizer(channel, proxy);
}

void BindNewAddressSpace(Tegra::MemoryManager& memory_manager,
                         VideoCore::RasterizerInterface& real) {
    std::scoped_lock lock{g_install_mutex};
    auto* const proxy = Proxy();
    memory_manager.BindRasterizer(proxy != nullptr ? proxy : &real);
}

void StageBoundChannel(Tegra::Control::ChannelState& channel) {
    auto* const proxy = Proxy();
    if (proxy == nullptr || !channel.initialized) {
        return;
    }
    if (!ChannelStaged(channel)) {
        StageChannel(channel, *proxy, *g_real);
    } else {
        // An address space the channel was given since (BindNewAddressSpace binds new ones).
        SetEngineRasterizer(channel, proxy);
    }
    if (g_check) {
        CheckStaged(channel);
    }
}

} // namespace VideoCommon::Nx2Stages
