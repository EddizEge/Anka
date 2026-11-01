// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <limits>

#include <boost/container/small_vector.hpp>

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vk_multi_range_buffer.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class DescriptorPool;
class Scheduler;
struct HostVertexBinding;

class BufferCacheRuntime;

class Buffer : public VideoCommon::BufferBase {
public:
    explicit Buffer(BufferCacheRuntime&, VideoCommon::NullBufferParams null_params);
    explicit Buffer(BufferCacheRuntime& runtime, VAddr cpu_addr_, u64 size_bytes_,
                    bool sparse_compatible_);

    [[nodiscard]] VkBufferView View(u32 offset, u32 size, VideoCore::Surface::PixelFormat format);

    [[nodiscard]] VkBuffer Handle() const noexcept {
        return *buffer;
    }

    [[nodiscard]] VkDeviceAddress DeviceAddress() const noexcept {
        return device_address;
    }

    [[nodiscard]] bool IsSparseCompatible() const noexcept {
        return sparse_compatible;
    }

    [[nodiscard]] vk::MemoryLocation Location() const noexcept {
        return buffer.Location();
    }

    [[nodiscard]] bool IsRegionUsed(u64 offset, u64 size) const noexcept {
        return tracker.IsUsed(offset, size);
    }

    void MarkUsage(u64 offset, u64 size) noexcept;

    void ResetUsageTracking() noexcept {
        tracker.Reset();
    }

    [[nodiscard]] u64 LastUsageTick() const noexcept {
        return last_usage_tick;
    }

    operator VkBuffer() const noexcept {
        return *buffer;
    }

private:
    struct BufferView {
        u32 offset;
        u32 size;
        VideoCore::Surface::PixelFormat format;
        vk::BufferView handle;
    };

    const Device* device{};
    Scheduler* scheduler{};
    vk::Buffer buffer;
    std::vector<BufferView> views;
    VideoCommon::UsageTracker tracker;
    VkDeviceAddress device_address{};
    u64 last_usage_tick{};
    bool is_null{};
    bool sparse_compatible{};
};

class QuadArrayIndexBuffer;
class QuadStripIndexBuffer;

class BufferCacheRuntime {
    friend Buffer;

    using PrimitiveTopology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;

public:
    explicit BufferCacheRuntime(const Device& device_, MemoryAllocator& memory_manager_,
                                Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                                GuestDescriptorQueue& guest_descriptor_queue,
                                ComputePassDescriptorQueue& compute_pass_descriptor_queue,
                                DescriptorPool& descriptor_pool);

    void TickFrame(Common::SlotVector<Buffer>& slot_buffers) noexcept;

    u64 CurrentTick();

    bool IsFree(u64 tick);

    void Wait(u64 tick);

    void Finish();

    u64 GetDeviceLocalMemory() const;

    u64 GetDeviceMemoryUsage() const;

    bool CanReportMemoryUsage() const;

    u32 GetUniformBufferAlignment() const;

    u32 GetStorageBufferAlignment() const;

    [[nodiscard]] StagingBufferRef UploadStagingBuffer(size_t size);

    [[nodiscard]] StagingBufferRef DownloadStagingBuffer(size_t size, bool deferred = false);

    bool CanReorderUpload(const Buffer& buffer, std::span<const VideoCommon::BufferCopy> copies);

    void FreeDeferredStagingBuffer(StagingBufferRef& ref);

    void PreCopyBarrier();

    void CopyBuffer(VkBuffer dst_buffer, VkBuffer src_buffer,
                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                    bool can_reorder_upload = false);

    void PostCopyBarrier();

    void ClearBuffer(VkBuffer dest_buffer, u32 offset, size_t size, u32 value);

    void BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format, u32 num_indices,
                         u32 base_vertex, VkBuffer buffer, u32 offset, u32 size);

    void BindQuadIndexBuffer(PrimitiveTopology topology, u32 first, u32 count);

    void BindVertexBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size, u32 stride);

    void BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings);

    void BindTransformFeedbackBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size);

    void BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>& bindings);

    std::span<u8> BindMappedUniformBuffer([[maybe_unused]] size_t stage,
                                          [[maybe_unused]] u32 binding_index,
                                          u32 size) {
        const StagingBufferRef ref = staging_pool.Request(size, MemoryUsage::Upload);
        guest_descriptor_queue.AddBuffer(ref.buffer, ref.device_address,
                                         static_cast<u32>(ref.offset), size);
        return ref.mapped_span;
    }

    /// EDEN_NX2_UBO_REUSE (on unless =0): the buffer cache reads a streamed uniform buffer into
    /// UniformReuseScratch, then BindMappedUniformBufferReuse binds the slice streamed for the
    /// same guest range earlier in this scheduler tick when its size and bytes match, or streams
    /// a new one. Returns whether a slice was reused.
    [[nodiscard]] static bool UniformReuseEnabled() noexcept {
        // Read at the first call, as a function-local static const is, without that static's
        // thread-safe initialization check on every call (MSVC: a thread-local read; this runs
        // for each uniform buffer bind).
        static std::atomic<s8> enabled{-1};
        s8 value = enabled.load(std::memory_order_relaxed);
        if (value < 0) [[unlikely]] {
            const char* env = std::getenv("EDEN_NX2_UBO_REUSE");
            value = static_cast<s8>(env == nullptr || env[0] != '0');
            enabled.store(value, std::memory_order_relaxed);
        }
        return value != 0;
    }

    [[nodiscard]] std::span<u8> UniformReuseScratch(u32 size) {
        if (uniform_reuse_scratch.size() < size) {
            uniform_reuse_scratch.resize((std::max<size_t>)(size, 4096));
        }
        return {uniform_reuse_scratch.data(), size};
    }

    bool BindMappedUniformBufferReuse(size_t stage, u32 index, DAddr device_addr, u32 size);

    /// EDEN_NX2_UBO_DIRECT (on unless =0): with EDEN_NX2_UBO_REUSE on, the buffer cache hands
    /// guest bytes that are contiguous in host memory to BindMappedUniformBufferReuseFrom, which
    /// compares and copies them in place instead of through UniformReuseScratch.
    [[nodiscard]] static bool UniformDirectEnabled() noexcept {
        // Read at the first call, as a function-local static const is, without that static's
        // thread-safe initialization check on every call (MSVC: a thread-local read; this runs
        // for each uniform buffer bind).
        static std::atomic<s8> enabled{-1};
        s8 value = enabled.load(std::memory_order_relaxed);
        if (value < 0) [[unlikely]] {
            const char* env = std::getenv("EDEN_NX2_UBO_DIRECT");
            value = static_cast<s8>(env == nullptr || env[0] != '0');
            enabled.store(value, std::memory_order_relaxed);
        }
        return value != 0;
    }

    bool BindMappedUniformBufferReuseFrom(size_t stage, u32 index, DAddr device_addr,
                                          const u8* guest, u32 size);

    /// EDEN_NX2_UBO_STAGE (on unless =0): with EDEN_NX2_UBO_REUSE on, a bind first tries the
    /// slice its stage's slot was bound to last (constants written to a new address with the
    /// same values every draw, slices the direct-mapped table lost to other ranges), and a slice
    /// holding at least the bytes bound serves a shorter bind of them (one range bound to stages
    /// that use different sizes of it).
    [[nodiscard]] static bool UniformStageEnabled() noexcept {
        // Read at the first call, as a function-local static const is, without that static's
        // thread-safe initialization check on every call (MSVC: a thread-local read; this runs
        // for each uniform buffer bind).
        static std::atomic<s8> enabled{-1};
        s8 value = enabled.load(std::memory_order_relaxed);
        if (value < 0) [[unlikely]] {
            const char* env = std::getenv("EDEN_NX2_UBO_STAGE");
            value = static_cast<s8>(env == nullptr || env[0] != '0');
            enabled.store(value, std::memory_order_relaxed);
        }
        return value != 0;
    }

    /// Binds reused that EDEN_NX2_UBO_REUSE alone would have streamed (EDEN_NX2_UBO_STAGE), for
    /// the log.
    [[nodiscard]] u64 UniformStageReuses() const noexcept {
        return uniform_stage_reuses;
    }

    [[nodiscard]] VkDeviceSize SparseAlignmentFor(bool sparse_compatible) const noexcept {
        if (!sparse_compatible || !multi_range_buffers.use_sparse) {
            return 0;
        }
        return multi_range_buffers.block_size;
    }

    [[nodiscard]] bool PrefersSparseSources() const noexcept {
        return multi_range_buffers.use_sparse;
    }

    void ResetMultiRange() noexcept {
        multi_range_sources.clear();
        multi_range_total = 0;
    }

    void PushMultiRangeSource(const Buffer& buffer, u32 offset, u32 size) {
        const vk::MemoryLocation location = buffer.Location();
        multi_range_sources.push_back(MultiRangeSource{
            .handle = buffer.Handle(),
            .memory = location.memory,
            .memory_offset = location.offset,
            .offset = offset,
            .size = size,
            .write_tick = buffer.getWriteTick(),
            .memory_type = location.memory_type,
        });
        multi_range_total += size;
    }

    bool BindMultiRangeStorageBuffer(u64 key, bool is_written);

    void InvalidateMultiRange(u64 key) {
        multi_range_buffers.Invalidate(key);
    }

    void OnBufferDeleted(const Buffer& buffer) {
        multi_range_buffers.DropOwner(scheduler, buffer.Handle());
    }

    void BindUniformBuffer(const Buffer& buffer, u32 offset, u32 size) {
        BindBuffer(buffer, offset, size);
    }

    void BindStorageBuffer(const Buffer& buffer, u32 offset, u32 size,
                           [[maybe_unused]] bool is_written) {
        BindBuffer(buffer, offset, size);
    }

    void BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                           VideoCore::Surface::PixelFormat format) {
        if (guest_descriptor_queue.UsesDescriptorBuffer()) {
            const VkDeviceAddress address = buffer.DeviceAddress();
            const VkFormat vk_format = address == 0 ? VK_FORMAT_UNDEFINED : TexelBufferFormat(format);
            guest_descriptor_queue.AddTexelBuffer(VK_NULL_HANDLE, address, offset, size, vk_format);
        } else {
            guest_descriptor_queue.AddTexelBuffer(buffer.View(offset, size, format));
        }
    }

    bool ShouldLimitDynamicStorageBuffers() const {
        return limit_dynamic_storage_buffers;
    }

    u32 GetMaxDynamicStorageBuffers() const {
        return max_dynamic_storage_buffers;
    }

private:
    void BindBuffer(const Buffer& buffer, u32 offset, u32 size) {
        const VkBuffer handle = buffer.Handle();
        if (handle == VK_NULL_HANDLE) {
            guest_descriptor_queue.AddBuffer(handle, 0, 0, VK_WHOLE_SIZE);
        } else {
            guest_descriptor_queue.AddBuffer(handle, buffer.DeviceAddress(), offset, size);
        }
    }

    VkFormat TexelBufferFormat(VideoCore::Surface::PixelFormat format) const;

    void ReserveNullBuffer();
    vk::Buffer CreateNullBuffer();

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;
    GuestDescriptorQueue& guest_descriptor_queue;

    std::shared_ptr<QuadArrayIndexBuffer> quad_array_index_buffer;
    std::shared_ptr<QuadStripIndexBuffer> quad_strip_index_buffer;

    vk::Buffer null_buffer;

    std::unique_ptr<Uint8Pass> uint8_pass;
    QuadIndexedPass quad_index_pass;

    MultiRangeBufferCache multi_range_buffers;
    boost::container::small_vector<MultiRangeSource, 16> multi_range_sources;
    VkDeviceSize multi_range_total{};

    /// Uniform buffer slices streamed for EDEN_NX2_UBO_REUSE, direct-mapped by guest address,
    /// each with a copy of the bytes it holds; valid only in the tick they were streamed in.
    struct UniformSlice {
        DAddr device_addr{};
        u64 tick = (std::numeric_limits<u64>::max)();
        VkBuffer buffer{};
        VkDeviceAddress buffer_address{};
        u32 offset{};
        u32 size{};
        u64 serial{}; ///< A new one each time the slice is streamed (EDEN_NX2_UBO_STAGE)
        std::vector<u8> bytes;
    };
    static constexpr u32 UNIFORM_SLICE_BITS = 7;
    std::array<UniformSlice, size_t{1} << UNIFORM_SLICE_BITS> uniform_slices{};
    std::vector<u8> uniform_reuse_scratch;

    /// EDEN_NX2_UBO_STAGE: the slice each graphics stage's uniform buffer slot was bound to
    /// last, valid while that slice keeps the serial it had then.
    struct StageUniform {
        u64 serial{}; ///< 0: none
        u32 slot{};   ///< Index in uniform_slices
    };
    static constexpr size_t UNIFORM_STAGES = 5;
    static constexpr size_t UNIFORM_STAGE_SLOTS = 18;
    std::array<std::array<StageUniform, UNIFORM_STAGE_SLOTS>, UNIFORM_STAGES> stage_uniforms{};
    u64 uniform_serial{};
    u64 uniform_stage_reuses{};

    /// A slice streamed in this tick whose bytes start with `bytes` (`size` of them), to bind
    /// instead of streaming a new one, or null. `slice` is the range's slot in uniform_slices.
    [[nodiscard]] const UniformSlice* FindUniformSlice(size_t stage, u32 index,
                                                       const UniformSlice& slice,
                                                       DAddr device_addr, const u8* bytes,
                                                       u32 size);

    /// Remembers `slice` as the one the stage's slot was bound to (EDEN_NX2_UBO_STAGE).
    void NoteStageUniform(size_t stage, u32 index, const UniformSlice& slice);

    bool limit_dynamic_storage_buffers = false;
    u32 max_dynamic_storage_buffers = (std::numeric_limits<u32>::max)();
};

struct BufferCacheParams {
    using Runtime = Vulkan::BufferCacheRuntime;
    using Buffer = Vulkan::Buffer;
    using Async_Buffer = Vulkan::StagingBufferRef;
    using MemoryTracker = VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager>;

    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = false;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = false;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = false;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};

using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;

} // namespace Vulkan
