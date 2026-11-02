// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <vector>

#include "common/logging.h"
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"

#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/nx2_profile.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/texture_cache/util.h"

namespace Vulkan {
namespace {
VkBufferCopy MakeBufferCopy(const VideoCommon::BufferCopy& copy) {
    return VkBufferCopy{
        .srcOffset = copy.src_offset,
        .dstOffset = copy.dst_offset,
        .size = copy.size,
    };
}

VkIndexType IndexTypeFromNumElements(const Device& device, u32 num_elements) {
    if (num_elements <= 0xff && device.IsExtIndexTypeUint8Supported()) {
        return VK_INDEX_TYPE_UINT8_EXT;
    }
    if (num_elements <= 0xffff) {
        return VK_INDEX_TYPE_UINT16;
    }
    return VK_INDEX_TYPE_UINT32;
}

size_t BytesPerIndex(VkIndexType index_type) {
    switch (index_type) {
    case VK_INDEX_TYPE_UINT8_EXT:
        return 1;
    case VK_INDEX_TYPE_UINT16:
        return 2;
    case VK_INDEX_TYPE_UINT32:
        return 4;
    default:
        ASSERT_MSG(false, "Invalid index type={}", index_type);
        return 1;
    }
}

vk::Buffer CreateBuffer(const Device& device, const MemoryAllocator& memory_allocator, u64 size,
                        VkDeviceSize sparse_alignment) {
    VkBufferUsageFlags flags =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsExtConditionalRendering()) {
        flags |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    const VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size,
        .usage = flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (sparse_alignment > 1) {
        return memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal, sparse_alignment);
    }
    return memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);
}
// K3: whether the buffer runtime's joins let go of B's cache locks (option (b), HeldLocks::
// Unlocked) or keep the buffer cache mutex (option (a), decided after R2: no window in which
// another thread sees a buffer cache operation half done).
constexpr bool StageJoinsUnlock = false;

} // Anonymous namespace

Buffer::Buffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams null_params)
    : VideoCommon::BufferBase(null_params), scheduler{&runtime.scheduler}, owner{&runtime},
      tracker{4096} {
    if (runtime.device.HasNullDescriptor()) {
        return;
    }
    device = &runtime.device;
    buffer = runtime.CreateNullBuffer();
    is_null = true;
    if (device->IsBufferDeviceAddressSupported()) {
        device_address = device->GetLogical().GetBufferDeviceAddress(*buffer);
    }
}

Buffer::Buffer(BufferCacheRuntime& runtime, DAddr cpu_addr_, u64 size_bytes_,
               bool sparse_compatible_)
    : VideoCommon::BufferBase(cpu_addr_, size_bytes_), device{&runtime.device},
      scheduler{&runtime.scheduler}, owner{&runtime},
      buffer{CreateBuffer(*device, runtime.memory_allocator, SizeBytes(),
                          runtime.SparseAlignmentFor(sparse_compatible_))},
      tracker{SizeBytes()} {
    sparse_compatible = sparse_compatible_;
    if (runtime.device.HasDebuggingToolAttached()) {
        buffer.SetObjectNameEXT(fmt::format("Buffer {:#x}", CpuAddr()).c_str());
    }
    if (device->IsBufferDeviceAddressSupported()) {
        device_address = device->GetLogical().GetBufferDeviceAddress(*buffer);
    }
}

Buffer::~Buffer() {
    if (owner != nullptr && owner->Staged() && buffer) [[unlikely]] {
        std::vector<vk::BufferView> handles;
        handles.reserve(views.size());
        for (BufferView& view : views) {
            handles.push_back(std::move(view.handle));
        }
        owner->BuryBuffer(std::move(buffer), std::move(handles));
    }
}

void Buffer::MarkUsage(u64 offset, u64 size) noexcept {
    tracker.Track(offset, size);
    last_usage_tick = owner->UsageStamp();
}

VkBufferView Buffer::View(u32 offset, u32 size, VideoCore::Surface::PixelFormat format) {
    if (!device) {
        // Null buffer supported, return a null descriptor
        return VK_NULL_HANDLE;
    } else if (is_null) {
        // Null buffer not supported, adjust offset and size
        offset = 0;
        size = 0;
    }
    const auto it{std::ranges::find_if(views, [offset, size, format](const BufferView& view) {
        return offset == view.offset && size == view.size && format == view.format;
    })};
    if (it != views.end()) {
        return *it->handle;
    }
    views.push_back({
        .offset = offset,
        .size = size,
        .format = format,
        .handle = device->GetLogical().CreateBufferView({
            .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .buffer = *buffer,
            .format = MaxwellToVK::SurfaceFormat(*device, FormatType::Buffer, false, format).format,
            .offset = offset,
            .range = size,
        }),
    });
    return *views.back().handle;
}

class QuadIndexBuffer {
public:
    QuadIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                    Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                    RecordTarget& target_)
        : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
          staging_pool{staging_pool_}, target{target_} {}

    virtual ~QuadIndexBuffer() = default;

    void UpdateBuffer(u32 num_indices_) {
        if (num_indices_ <= num_indices) {
            return;
        }

        scheduler.Finish();

        num_indices = num_indices_;
        index_type = IndexTypeFromNumElements(device, num_indices);

        const u32 num_quads = GetQuadsNum(num_indices);
        const u32 num_triangle_indices = num_quads * 6;
        const u32 num_first_offset_copies = 4;
        const size_t bytes_per_index = BytesPerIndex(index_type);
        const size_t size_bytes = num_triangle_indices * bytes_per_index * num_first_offset_copies;
        const VkBufferCreateInfo buffer_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size_bytes,
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);
        if (device.HasDebuggingToolAttached()) {
            buffer.SetObjectNameEXT("Quad LUT");
        }

        const bool host_visible = buffer.IsHostVisible();
        const StagingBufferRef staging = [&] {
            if (host_visible) {
                return StagingBufferRef{};
            }
            return staging_pool.Request(size_bytes, MemoryUsage::Upload);
        }();

        u8* staging_data = host_visible ? buffer.Mapped().data() : staging.mapped_span.data();
        const size_t quad_size = bytes_per_index * 6;

        for (u32 first = 0; first < num_first_offset_copies; ++first) {
            for (u32 quad = 0; quad < num_quads; ++quad) {
                MakeAndUpdateIndices(staging_data, quad_size, quad, first);
                staging_data += quad_size;
            }
        }

        if (!host_visible) {
            scheduler.RequestOutsideRenderPassOperationContext();
            scheduler.Record([src_buffer = staging.buffer, src_offset = staging.offset,
                              dst_buffer = *buffer, size_bytes](vk::CommandBuffer cmdbuf) {
                const VkBufferCopy copy{
                    .srcOffset = src_offset,
                    .dstOffset = 0,
                    .size = size_bytes,
                };
                const VkBufferMemoryBarrier write_barrier{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_INDEX_READ_BIT,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = dst_buffer,
                    .offset = 0,
                    .size = size_bytes,
                };
                cmdbuf.CopyBuffer(src_buffer, dst_buffer, copy);
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, write_barrier);
            });
        } else {
            buffer.Flush();
        }
    }

    /// UpdateBuffer for B inside a packet (cut c): no Finish, the old table goes to `graveyard`
    /// until C is done with what B sent before; the copy goes into B's record list.
    void UpdateBufferStaged(u32 num_indices_, StageStagingBufferPool& stage_pool,
                            Nx2Record::SeqGraveyard<vk::Buffer>& graveyard, u64 seq) {
        if (num_indices_ <= num_indices) {
            return;
        }
        if (buffer) {
            graveyard.Bury(std::move(buffer), seq);
        }

        num_indices = num_indices_;
        index_type = IndexTypeFromNumElements(device, num_indices);

        const u32 num_quads = GetQuadsNum(num_indices);
        const u32 num_triangle_indices = num_quads * 6;
        const u32 num_first_offset_copies = 4;
        const size_t bytes_per_index = BytesPerIndex(index_type);
        const size_t size_bytes = num_triangle_indices * bytes_per_index * num_first_offset_copies;
        const VkBufferCreateInfo buffer_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size_bytes,
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);
        if (device.HasDebuggingToolAttached()) {
            buffer.SetObjectNameEXT("Quad LUT");
        }

        const bool host_visible = buffer.IsHostVisible();
        const StagingBufferRef staging = [&] {
            if (host_visible) {
                return StagingBufferRef{};
            }
            return stage_pool.Request(size_bytes, MemoryUsage::Upload);
        }();

        u8* staging_data = host_visible ? buffer.Mapped().data() : staging.mapped_span.data();
        const size_t quad_size = bytes_per_index * 6;

        for (u32 first = 0; first < num_first_offset_copies; ++first) {
            for (u32 quad = 0; quad < num_quads; ++quad) {
                MakeAndUpdateIndices(staging_data, quad_size, quad, first);
                staging_data += quad_size;
            }
        }

        if (!host_visible) {
            target.RequestOutsideRenderPassOperationContext();
            target.Record([src_buffer = staging.buffer, src_offset = staging.offset,
                           dst_buffer = *buffer, size_bytes](vk::CommandBuffer cmdbuf) {
                const VkBufferCopy copy{
                    .srcOffset = src_offset,
                    .dstOffset = 0,
                    .size = size_bytes,
                };
                const VkBufferMemoryBarrier write_barrier{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_INDEX_READ_BIT,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = dst_buffer,
                    .offset = 0,
                    .size = size_bytes,
                };
                cmdbuf.CopyBuffer(src_buffer, dst_buffer, copy);
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, write_barrier);
            });
        } else {
            buffer.Flush();
        }
    }

    void BindBuffer(u32 first) {
        const VkIndexType index_type_ = index_type;
        const size_t sub_first_offset = static_cast<size_t>(first % 4) * GetQuadsNum(num_indices);
        const size_t offset =
            (sub_first_offset + GetQuadsNum(first)) * 6ULL * BytesPerIndex(index_type);
        target.Record([buffer_ = *buffer, index_type_, offset](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindIndexBuffer(buffer_, offset, index_type_);
        });
    }

protected:
    virtual u32 GetQuadsNum(u32 num_indices) const = 0;

    virtual void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) = 0;

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;
    RecordTarget& target;

    vk::Buffer buffer{};
    MemoryCommit memory_commit{};
    VkIndexType index_type{};
    u32 num_indices = 0;
};

class QuadArrayIndexBuffer : public QuadIndexBuffer {
public:
    QuadArrayIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                         Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                         RecordTarget& target_)
        : QuadIndexBuffer(device_, memory_allocator_, scheduler_, staging_pool_, target_) {}

    ~QuadArrayIndexBuffer() = default;

private:
    u32 GetQuadsNum(u32 num_indices_) const override {
        return num_indices_ / 4;
    }

    template <typename T>
    static std::array<T, 6> MakeIndices(u32 quad, u32 first) {
        std::array<T, 6> indices{0, 1, 2, 0, 2, 3};
        for (T& index : indices) {
            index = static_cast<T>(first + index + quad * 4);
        }
        return indices;
    }

    void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) override {
        switch (index_type) {
        case VK_INDEX_TYPE_UINT8_EXT:
            std::memcpy(staging_data, MakeIndices<u8>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT16:
            std::memcpy(staging_data, MakeIndices<u16>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT32:
            std::memcpy(staging_data, MakeIndices<u32>(quad, first).data(), quad_size);
            break;
        default:
            ASSERT(false);
            break;
        }
    }
};

class QuadStripIndexBuffer : public QuadIndexBuffer {
public:
    QuadStripIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                         Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                         RecordTarget& target_)
        : QuadIndexBuffer(device_, memory_allocator_, scheduler_, staging_pool_, target_) {}

    ~QuadStripIndexBuffer() = default;

private:
    u32 GetQuadsNum(u32 num_indices_) const override {
        return num_indices_ >= 4 ? (num_indices_ - 2) / 2 : 0;
    }

    template <typename T>
    static std::array<T, 6> MakeIndices(u32 quad, u32 first) {
        std::array<T, 6> indices{0, 3, 1, 0, 2, 3};
        for (T& index : indices) {
            index = static_cast<T>(first + index + quad * 2);
        }
        return indices;
    }

    void MakeAndUpdateIndices(u8* staging_data, size_t quad_size, u32 quad, u32 first) override {
        switch (index_type) {
        case VK_INDEX_TYPE_UINT8_EXT:
            std::memcpy(staging_data, MakeIndices<u8>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT16:
            std::memcpy(staging_data, MakeIndices<u16>(quad, first).data(), quad_size);
            break;
        case VK_INDEX_TYPE_UINT32:
            std::memcpy(staging_data, MakeIndices<u32>(quad, first).data(), quad_size);
            break;
        default:
            ASSERT(false);
            break;
        }
    }
};

BufferCacheRuntime::BufferCacheRuntime(const Device& device_, MemoryAllocator& memory_allocator_,
                                       Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                                       GuestDescriptorQueue& guest_descriptor_queue_,
                                       ComputePassDescriptorQueue& compute_pass_descriptor_queue,
                                       DescriptorPool& descriptor_pool)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      staging_pool{staging_pool_}, guest_descriptor_queue{guest_descriptor_queue_},
      quad_index_pass(device, scheduler, descriptor_pool, staging_pool,
                      compute_pass_descriptor_queue),
      multi_range_buffers(device_) {
    const VkDriverIdKHR driver_id = device.GetDriverID();
    limit_dynamic_storage_buffers = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
                                    driver_id == VK_DRIVER_ID_ARM_PROPRIETARY;
    if (limit_dynamic_storage_buffers) {
        max_dynamic_storage_buffers = device.GetMaxDescriptorSetStorageBuffersDynamic();
    }    
    if (device.SupportsUint8Indices()) {
        uint8_pass = std::make_unique<Uint8Pass>(device, scheduler, descriptor_pool, staging_pool,
                                                 compute_pass_descriptor_queue);
    }
    quad_array_index_buffer = std::make_shared<QuadArrayIndexBuffer>(
        device_, memory_allocator_, scheduler_, staging_pool_, target);
    quad_strip_index_buffer = std::make_shared<QuadStripIndexBuffer>(
        device_, memory_allocator_, scheduler_, staging_pool_, target);
}

BufferCacheRuntime::~BufferCacheRuntime() = default;

StagingBufferRef BufferCacheRuntime::UploadStagingBuffer(size_t size) {
    return RequestStaging(size, MemoryUsage::Upload);
}

StagingBufferRef BufferCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return RequestStaging(size, MemoryUsage::Download, deferred);
}

VkFormat BufferCacheRuntime::TexelBufferFormat(VideoCore::Surface::PixelFormat format) const {
    return MaxwellToVK::SurfaceFormat(device, FormatType::Buffer, false, format).format;
}

void BufferCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) {
    if (StageStagingBufferPool::Owns(ref)) [[unlikely]] {
        stage_pool->FreeDeferred(ref);
        return;
    }
    staging_pool.FreeDeferred(ref);
}

u64 BufferCacheRuntime::GetDeviceLocalMemory() const {
    return device.GetDeviceLocalMemory();
}

u64 BufferCacheRuntime::GetDeviceMemoryUsage() const {
    return device.GetDeviceMemoryUsage();
}

bool BufferCacheRuntime::CanReportMemoryUsage() const {
    return device.CanReportMemoryUsage();
}

u32 BufferCacheRuntime::GetUniformBufferAlignment() const {
    return static_cast<u32>(device.GetUniformBufferAlignment());
}

const BufferCacheRuntime::UniformSlice* BufferCacheRuntime::FindUniformSlice(
    size_t stage, u32 index, const UniformSlice& slice, DAddr device_addr, const u8* bytes,
    u32 size) {
    const u64 tick = scheduler.CurrentTick();
    if (!UniformStageEnabled()) {
        if (slice.tick == tick && slice.device_addr == device_addr && slice.size == size &&
            std::memcmp(slice.bytes.data(), bytes, size) == 0) {
            return &slice;
        }
        return nullptr;
    }
    // EDEN_NX2_UBO_STAGE: the stage slot's last slice, whatever range it was streamed for, then
    // the range's own. Either was streamed in this tick (its region stays until the host GPU
    // passed the tick, as for any reused slice; the serial tells it was not streamed again
    // since) and its record holds the bytes copied into it. Its first `size` bytes are the
    // guest's, so `size` bytes bound at its offset give the shader what a new slice would.
    // uniform_stage_reuses counts the binds EDEN_NX2_UBO_REUSE alone would have streamed.
    const auto plain_match = [&] {
        return slice.tick == tick && slice.device_addr == device_addr && slice.size == size &&
               std::memcmp(slice.bytes.data(), bytes, size) == 0;
    };
    if (size != 0 && stage < UNIFORM_STAGES && index < UNIFORM_STAGE_SLOTS) {
        const StageUniform& last = stage_uniforms[stage][index];
        const UniformSlice& candidate = uniform_slices[last.slot];
        if (last.serial != 0 && candidate.serial == last.serial && candidate.tick == tick &&
            candidate.size >= size && std::memcmp(candidate.bytes.data(), bytes, size) == 0) {
            if ((&candidate != &slice || candidate.device_addr != device_addr ||
                 candidate.size != size) &&
                !plain_match()) {
                ++uniform_stage_reuses;
                VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::UniformStageReuses, 1);
            }
            return &candidate;
        }
    }
    if (slice.tick == tick && slice.device_addr == device_addr && slice.size >= size &&
        std::memcmp(slice.bytes.data(), bytes, size) == 0) {
        if (slice.size != size) {
            ++uniform_stage_reuses;
            VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::UniformStageReuses, 1);
        }
        return &slice;
    }
    return nullptr;
}

void BufferCacheRuntime::NoteStageUniform(size_t stage, u32 index, const UniformSlice& slice) {
    if (stage < UNIFORM_STAGES && index < UNIFORM_STAGE_SLOTS) {
        stage_uniforms[stage][index] = StageUniform{
            .serial = slice.serial,
            .slot = static_cast<u32>(&slice - uniform_slices.data()),
        };
    }
}

bool BufferCacheRuntime::BindMappedUniformBufferReuse(size_t stage, u32 index, DAddr device_addr,
                                                      u32 size) {
    if (staged) [[unlikely]] {
        return BindMappedUniformBufferReuseStaged(stage, index, device_addr,
                                                  uniform_reuse_scratch.data(), size, true);
    }
    // A slice is reused only in the tick that streamed it: the stream buffer hands a range out
    // again once the host GPU passed the tick its region was stamped with (at least the tick it
    // was requested in, StagingBufferPool::GetStreamBuffer), staging buffers once IsFree(tick);
    // draws of one tick go into the same submission. Uniform buffers start on 256-byte
    // boundaries.
    UniformSlice& slice =
        uniform_slices[((device_addr >> 8) * 0x9E3779B97F4A7C15ULL) >> (64 - UNIFORM_SLICE_BITS)];
    const u8* const bytes = uniform_reuse_scratch.data();
    if (const UniformSlice* const found =
            FindUniformSlice(stage, index, slice, device_addr, bytes, size)) {
        guest_descriptor_queue.AddBuffer(found->buffer, found->buffer_address, found->offset,
                                         size);
        NoteStageUniform(stage, index, *found);
        return true;
    }
    const StagingBufferRef ref = staging_pool.Request(size, MemoryUsage::Upload);
    guest_descriptor_queue.AddBuffer(ref.buffer, ref.device_address,
                                     static_cast<u32>(ref.offset), size);
    std::memcpy(ref.mapped_span.data(), bytes, size);
    // The scratch becomes this slice's copy of the bytes; the old copy is the next scratch.
    slice.bytes.swap(uniform_reuse_scratch);
    slice.device_addr = device_addr;
    slice.tick = scheduler.CurrentTick();
    slice.buffer = ref.buffer;
    slice.buffer_address = ref.device_address;
    slice.offset = static_cast<u32>(ref.offset);
    slice.size = size;
    slice.serial = ++uniform_serial;
    NoteStageUniform(stage, index, slice);
    return false;
}

bool BufferCacheRuntime::BindMappedUniformBufferReuseFrom(size_t stage, u32 index,
                                                          DAddr device_addr, const u8* guest,
                                                          u32 size) {
    // EDEN_NX2_UBO_DIRECT: BindMappedUniformBufferReuse with the bytes read from guest memory in
    // place. A new slice copies them into its own record first and streams the record, never
    // guest memory again: the guest may write between two reads, and the stream must hold
    // exactly the bytes later binds compare against (guest -> record -> stream, as before).
    if (staged) [[unlikely]] {
        return BindMappedUniformBufferReuseStaged(stage, index, device_addr, guest, size, false);
    }
    UniformSlice& slice =
        uniform_slices[((device_addr >> 8) * 0x9E3779B97F4A7C15ULL) >> (64 - UNIFORM_SLICE_BITS)];
    if (const UniformSlice* const found =
            FindUniformSlice(stage, index, slice, device_addr, guest, size)) {
        guest_descriptor_queue.AddBuffer(found->buffer, found->buffer_address, found->offset,
                                         size);
        NoteStageUniform(stage, index, *found);
        return true;
    }
    if (slice.bytes.size() < size) {
        slice.bytes.resize((std::max<size_t>)(size, 4096));
    }
    std::memcpy(slice.bytes.data(), guest, size);
    const StagingBufferRef ref = staging_pool.Request(size, MemoryUsage::Upload);
    guest_descriptor_queue.AddBuffer(ref.buffer, ref.device_address,
                                     static_cast<u32>(ref.offset), size);
    std::memcpy(ref.mapped_span.data(), slice.bytes.data(), size);
    slice.device_addr = device_addr;
    slice.tick = scheduler.CurrentTick();
    slice.buffer = ref.buffer;
    slice.buffer_address = ref.device_address;
    slice.offset = static_cast<u32>(ref.offset);
    slice.size = size;
    slice.serial = ++uniform_serial;
    NoteStageUniform(stage, index, slice);
    return false;
}

bool BufferCacheRuntime::BindMappedUniformBufferReuseStaged(size_t stage, u32 index,
                                                            DAddr device_addr, const u8* bytes,
                                                            u32 size, bool from_scratch) {
    // BindMappedUniformBufferReuse(From) while staged (cut c): B cannot tell the tick a slice
    // goes into. A slice of B's stream buffer keeps its bytes for the stream epoch it was
    // handed out in (the buffer hands ranges out in order and wraps once per epoch), and a
    // reuse holds its regions until the reuse is done, so "streamed in this tick" becomes
    // "streamed in this epoch". Slices from a staging buffer (the stream was busy) are not
    // reused.
    constexpr u64 STAGE_EPOCH_BIT = u64{1} << 62;
    const u64 epoch = STAGE_EPOCH_BIT | stage_pool->StreamEpoch();
    UniformSlice& slice =
        uniform_slices[((device_addr >> 8) * 0x9E3779B97F4A7C15ULL) >> (64 - UNIFORM_SLICE_BITS)];
    const auto plain_match = [&] {
        return slice.tick == epoch && slice.device_addr == device_addr && slice.size == size &&
               std::memcmp(slice.bytes.data(), bytes, size) == 0;
    };
    const UniformSlice* found = nullptr;
    if (!UniformStageEnabled()) {
        if (plain_match()) {
            found = &slice;
        }
    } else {
        if (size != 0 && stage < UNIFORM_STAGES && index < UNIFORM_STAGE_SLOTS) {
            const StageUniform& last = stage_uniforms[stage][index];
            const UniformSlice& candidate = uniform_slices[last.slot];
            if (last.serial != 0 && candidate.serial == last.serial && candidate.tick == epoch &&
                candidate.size >= size && std::memcmp(candidate.bytes.data(), bytes, size) == 0) {
                if ((&candidate != &slice || candidate.device_addr != device_addr ||
                     candidate.size != size) &&
                    !plain_match()) {
                    ++uniform_stage_reuses;
                    VideoCommon::Nx2Profile::Add(
                        VideoCommon::Nx2Profile::Counter::UniformStageReuses, 1);
                }
                found = &candidate;
            }
        }
        if (found == nullptr && slice.tick == epoch && slice.device_addr == device_addr &&
            slice.size >= size && std::memcmp(slice.bytes.data(), bytes, size) == 0) {
            if (slice.size != size) {
                ++uniform_stage_reuses;
                VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::UniformStageReuses,
                                             1);
            }
            found = &slice;
        }
    }
    if (found != nullptr) {
        stage_pool->HoldStream(found->offset, size);
        descriptor_queue->AddBuffer(found->buffer, found->buffer_address, found->offset, size);
        NoteStageUniform(stage, index, *found);
        return true;
    }
    const StagingBufferRef ref = stage_pool->Request(size, MemoryUsage::Upload);
    descriptor_queue->AddBuffer(ref.buffer, ref.device_address, static_cast<u32>(ref.offset),
                                size);
    if (from_scratch) {
        std::memcpy(ref.mapped_span.data(), bytes, size);
        slice.bytes.swap(uniform_reuse_scratch);
    } else {
        if (slice.bytes.size() < size) {
            slice.bytes.resize((std::max<size_t>)(size, 4096));
        }
        std::memcpy(slice.bytes.data(), bytes, size);
        std::memcpy(ref.mapped_span.data(), slice.bytes.data(), size);
    }
    slice.device_addr = device_addr;
    slice.tick = ref.buffer == stage_pool->StreamBuf() ? epoch
                                                       : (std::numeric_limits<u64>::max)();
    slice.buffer = ref.buffer;
    slice.buffer_address = ref.device_address;
    slice.offset = static_cast<u32>(ref.offset);
    slice.size = size;
    slice.serial = ++uniform_serial;
    NoteStageUniform(stage, index, slice);
    return false;
}

u32 BufferCacheRuntime::GetStorageBufferAlignment() const {
    return static_cast<u32>(device.GetStorageBufferAlignment());
}

void BufferCacheRuntime::TickFrame(Common::SlotVector<Buffer>& slot_buffers) noexcept {
    for (auto it = slot_buffers.begin(); it != slot_buffers.end(); it++) {
        if (IsFree(it->LastUsageTick())) {
            it->ResetUsageTracking();
        }
    }
    if (stage_pool != nullptr) [[unlikely]] {
        stage_pool->TickFrame();
        const auto is_free = [this](u64 seq) { return scheduler.IsSeqFree(seq); };
        buffer_graveyard.Collect(is_free);
        lut_graveyard.Collect(is_free);
    }
}

u64 BufferCacheRuntime::CurrentTick() {
    return UsageStamp();
}

bool BufferCacheRuntime::IsFree(u64 tick) {
    if (Nx2Record::SeqTicks::IsStamp(tick)) [[unlikely]] {
        return scheduler.IsSeqFree(Nx2Record::SeqTicks::SeqOf(tick));
    }
    return scheduler.IsFree(tick);
}

void BufferCacheRuntime::Wait(u64 tick) {
    if (staged || Nx2Record::SeqTicks::IsStamp(tick)) [[unlikely]] {
        WaitStaged(tick);
        return;
    }
    scheduler.Wait(tick);
}

void BufferCacheRuntime::Finish() {
    if (target.list != nullptr) [[unlikely]] {
        JoinRecordOwner(StageJoin::Finish, [this] { scheduler.Finish(); });
        return;
    }
    scheduler.Finish();
}

void BufferCacheRuntime::WaitStaged(u64 tick) {
    if (IsFree(tick)) {
        return;
    }
    const bool is_stamp = Nx2Record::SeqTicks::IsStamp(tick);
    const u64 seq = Nx2Record::SeqTicks::SeqOf(tick);
    // The tick the work went into, once C recorded it.
    const auto recorded = [&]() -> std::optional<u64> {
        if (!is_stamp) {
            return tick;
        }
        if (seq <= scheduler.CDoneSeq()) {
            return scheduler.TickOfSeq(seq);
        }
        return std::nullopt;
    };
    if (const std::optional<u64> known = recorded(); known && *known < scheduler.CurrentTick()) {
        // Submitted, or being submitted by the worker: a wait records nothing.
        scheduler.GetMasterSemaphore().Wait(*known);
        return;
    }
    const auto wait = [&] {
        if (const std::optional<u64> known = recorded()) {
            scheduler.Wait(*known);
        } else {
            // B's own packet: replayed by now, in the current tick or before it.
            scheduler.Finish();
        }
    };
    if (target.list != nullptr) {
        JoinRecordOwner(StageJoin::Wait, wait);
        return;
    }
    // The record owner: a join, C with B parked, or the stages stopped.
    wait();
}

void BufferCacheRuntime::BeginStages() {
    if (stage_pool == nullptr) {
        stage_pool = std::make_unique<StageStagingBufferPool>(device, memory_allocator, scheduler,
                                                              pending_seq);
        descriptor_scratch = std::make_unique<UpdateDescriptorQueue>(
            device, STAGE_SCRATCH_ENTRIES, guest_descriptor_queue.SupportsDescriptorBuffer());
        record_arena = std::make_unique<Nx2Record::ChunkArena>();
        records = std::make_unique<RecordList>(*record_arena);
    }
    pending_seq = scheduler.CDoneSeq() + 1;
    staged = true;
}

void BufferCacheRuntime::EndStages() {
    if (!staged) {
        return;
    }
    if (target.list != nullptr) {
        // Stop came inside a packet: its commands still have to be recorded.
        LOG_ERROR(Render_Vulkan, "Buffer runtime stopped staging inside a packet (seq {})",
                  pending_seq);
        if (RecordList* const list = EndRecords()) {
            scheduler.Replay(*list, list->Seq());
        }
    }
    // Everything stamped so far went into the current tick or before it.
    scheduler.TickHandover().Note(pending_seq, scheduler.CurrentTick());
    staged = false;
}

u64 BufferCacheRuntime::BeginRecords() {
    records->Begin(pending_seq);
    target.list = records.get();
    descriptor_scratch->Restart(descriptor_scratch->UsesDescriptorBuffer());
    descriptor_queue = descriptor_scratch.get();
    return pending_seq;
}

RecordList* BufferCacheRuntime::EndRecords() {
    if (!pending_owner_drops.empty()) [[unlikely]] {
        // JoinRecordOwner retires them
        JoinRecordOwner(StageJoin::MultiRange, [] {});
    }
    RecordList* const list = TakeRecords();
    target.list = nullptr;
    descriptor_queue = &guest_descriptor_queue;
    ++pending_seq;
    return list;
}

RecordList* BufferCacheRuntime::TakeRecords() {
    if (records->Empty()) {
        return nullptr;
    }
    void* const memory =
        record_arena->Allocate(sizeof(RecordList), alignof(RecordList), records->Id());
    RecordList* const list = new (memory) RecordList(*records);
    records->Begin(pending_seq);
    return list;
}

void BufferCacheRuntime::JoinRecordOwner(StageJoin why,
                                         VideoCommon::Nx2Stages::FunctionRef op) {
    ++stage_joins[static_cast<size_t>(why)];
    RecordList* const sent = TakeRecords();
    target.list = nullptr;
    // Every join retires the multi-range owners the packet deleted so far, first.
    const auto run = [this, op] {
        DropPendingMultiRangeOwners();
        op();
    };
    if (record_owner_hook != nullptr) [[likely]] {
        if constexpr (StageJoinsUnlock) {
            const Nx2Record::HeldLocks::Unlocked unlocked;
            record_owner_hook(record_owner_context, sent, run);
        } else {
            record_owner_hook(record_owner_context, sent, run);
        }
    } else {
        // Nothing bound the hook (T9): record here, the stage owning the scheduler unchecked.
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_CRITICAL(Render_Vulkan, "Staged buffer runtime has no record owner hook");
        }
        if (sent != nullptr) {
            scheduler.Replay(*sent, sent->Seq());
        }
        run();
    }
    target.list = records.get();
}

void BufferCacheRuntime::BuryBuffer(vk::Buffer&& buffer, std::vector<vk::BufferView>&& views) {
    buffer_graveyard.Bury(BufferGrave{std::move(buffer), std::move(views)}, pending_seq);
}

void BufferCacheRuntime::StageDropMultiRangeOwner(VkBuffer owner) {
    if (!multi_range_used) {
        // No entry was ever made: nothing to retire
        return;
    }
    pending_owner_drops.push_back(owner);
}

void BufferCacheRuntime::DropPendingMultiRangeOwners() {
    for (const VkBuffer owner : pending_owner_drops) {
        multi_range_buffers.DropOwner(scheduler, owner);
    }
    pending_owner_drops.clear();
}

bool BufferCacheRuntime::CanReorderUpload(const Buffer& buffer,
                                          std::span<const VideoCommon::BufferCopy> copies) {
    if (Settings::values.disable_buffer_reorder) {
        return false;
    }
    const bool can_use_upload_cmdbuf =
        std::ranges::all_of(copies, [&](const VideoCommon::BufferCopy& copy) {
            return !buffer.IsRegionUsed(copy.dst_offset, copy.size);
        });
    return can_use_upload_cmdbuf;
}

void BufferCacheRuntime::CopyBuffer(VkBuffer dst_buffer, VkBuffer src_buffer,
                                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                                    bool can_reorder_upload) {
    if (dst_buffer == VK_NULL_HANDLE || src_buffer == VK_NULL_HANDLE) {
        return;
    }
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };

    // Measuring a popular game, this number never exceeds the specified size once data is warmed up
    boost::container::small_vector<VkBufferCopy, 8> vk_copies(copies.size());
    std::ranges::transform(copies, vk_copies.begin(), MakeBufferCopy);
    if (IsStreamBuffer(src_buffer) && can_reorder_upload) {
        target.RecordWithUploadBuffer([src_buffer, dst_buffer, vk_copies](
                                          vk::CommandBuffer, vk::CommandBuffer upload_cmdbuf) {
            upload_cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
        });
        return;
    }

    target.RequestOutsideRenderPassOperationContext();
    target.Record([src_buffer, dst_buffer, vk_copies, barrier](vk::CommandBuffer cmdbuf) {
        if (barrier) {
            cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, READ_BARRIER);
        }
        cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
        if (barrier) {
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   vk::PIPELINE_STAGE_GRAPHICS_COMPUTE, 0, WRITE_BARRIER);
        }
    });
}

void BufferCacheRuntime::PreCopyBarrier() {
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    target.RequestOutsideRenderPassOperationContext();
    target.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, READ_BARRIER);
    });
}

void BufferCacheRuntime::PostCopyBarrier() {
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    target.RequestOutsideRenderPassOperationContext();
    target.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                               0, WRITE_BARRIER);
    });
}

void BufferCacheRuntime::ClearBuffer(VkBuffer dest_buffer, u32 offset, size_t size, u32 value) {
    if (dest_buffer == VK_NULL_HANDLE) {
        return;
    }
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };

    target.RequestOutsideRenderPassOperationContext();
    target.Record([dest_buffer, offset, size, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, READ_BARRIER);
        cmdbuf.FillBuffer(dest_buffer, offset, size, value);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                               0, WRITE_BARRIER);
    });
}

bool BufferCacheRuntime::BindMultiRangeStorageBuffer(u64 key, bool is_written) {
    if (multi_range_sources.empty() || multi_range_total == 0) {
        return false;
    }
    MultiRangeRef ref;
    if (target.list != nullptr) [[unlikely]] {
        // It retires entries and may wait on the scheduler (vk_multi_range_buffer)
        JoinRecordOwner(StageJoin::MultiRange, [&] {
            ref = multi_range_buffers.Get(device, scheduler, memory_allocator, key,
                                          multi_range_sources, multi_range_total);
        });
    } else {
        ref = multi_range_buffers.Get(device, scheduler, memory_allocator, key,
                                      multi_range_sources, multi_range_total);
    }
    if (ref.handle == VK_NULL_HANDLE) {
        return false;
    }
    multi_range_used = true;
    if (is_written && !ref.sparse) {
        return false;
    }
    if (ref.needs_gather) {
        PreCopyBarrier();
        VkDeviceSize dst_offset = 0;
        for (const MultiRangeSource& source : multi_range_sources) {
            const std::array<VideoCommon::BufferCopy, 1> copy{VideoCommon::BufferCopy{
                .src_offset = u64(source.offset),
                .dst_offset = u64(dst_offset),
                .size = size_t(source.size),
            }};
            CopyBuffer(ref.handle, source.handle, copy, false);
            dst_offset += source.size;
        }
        PostCopyBarrier();
        multi_range_buffers.MarkGathered(key);
    }
    descriptor_queue->AddBuffer(ref.handle, ref.address, 0, ref.size);
    return true;
}

void BufferCacheRuntime::BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format,
                                         u32 base_vertex, u32 num_indices, VkBuffer buffer,
                                         u32 offset, [[maybe_unused]] u32 size) {
    VkIndexType vk_index_type = MaxwellToVK::IndexFormat(index_format);
    VkDeviceSize vk_offset = offset;
    VkBuffer vk_buffer = buffer;
    if (topology == PrimitiveTopology::Quads || topology == PrimitiveTopology::QuadStrip) {
        vk_index_type = VK_INDEX_TYPE_UINT32;
        if (target.list != nullptr) [[unlikely]] {
            // B (S11): its own staging and descriptor entries, recorded into its list
            std::tie(vk_buffer, vk_offset) = quad_index_pass.Assemble(
                target, *stage_pool, index_format, num_indices, base_vertex, buffer, offset,
                topology == PrimitiveTopology::QuadStrip);
        } else {
            std::tie(vk_buffer, vk_offset) =
                quad_index_pass.Assemble(index_format, num_indices, base_vertex, buffer, offset,
                                         topology == PrimitiveTopology::QuadStrip);
        }
    } else if (vk_index_type == VK_INDEX_TYPE_UINT8_EXT && !device.IsExtIndexTypeUint8Supported()) {
        vk_index_type = VK_INDEX_TYPE_UINT16;
        if (uint8_pass && target.list != nullptr) [[unlikely]] {
            std::tie(vk_buffer, vk_offset) =
                uint8_pass->Assemble(target, *stage_pool, num_indices, buffer, offset);
        } else if (uint8_pass) {
            std::tie(vk_buffer, vk_offset) = uint8_pass->Assemble(num_indices, buffer, offset);
        } else if (device.GetDriverID() == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
            ReserveNullBuffer();
            vk_buffer = *null_buffer;
            vk_offset = 0;
        }
    }
    if (vk_buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null index buffers. Replace it with our own null buffer.
        ReserveNullBuffer();
        vk_buffer = *null_buffer;
    }
    target.Record([vk_buffer, vk_offset, vk_index_type](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindIndexBuffer(vk_buffer, vk_offset, vk_index_type);
    });
}

void BufferCacheRuntime::BindQuadIndexBuffer(PrimitiveTopology topology, u32 first, u32 count) {
    if (count == 0) {
        ReserveNullBuffer();
        target.Record([this](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindIndexBuffer(*null_buffer, 0, VK_INDEX_TYPE_UINT32);
        });
        return;
    }

    if (topology == PrimitiveTopology::Quads) {
        if (target.list != nullptr) [[unlikely]] {
            quad_array_index_buffer->UpdateBufferStaged(first + count, *stage_pool, lut_graveyard,
                                                        pending_seq);
        } else {
            quad_array_index_buffer->UpdateBuffer(first + count);
        }
        quad_array_index_buffer->BindBuffer(first);
    } else if (topology == PrimitiveTopology::QuadStrip) {
        if (target.list != nullptr) [[unlikely]] {
            quad_strip_index_buffer->UpdateBufferStaged(first + count, *stage_pool, lut_graveyard,
                                                        pending_seq);
        } else {
            quad_strip_index_buffer->UpdateBuffer(first + count);
        }
        quad_strip_index_buffer->BindBuffer(first);
    }
}

void BufferCacheRuntime::BindVertexBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size, u32 stride) {
    if (index >= device.GetMaxVertexInputBindings()) {
        return;
    }
    if (device.IsExtExtendedDynamicStateSupported()) {
        target.Record([index, buffer, offset, size, stride](vk::CommandBuffer cmdbuf) {
            const VkDeviceSize vk_offset = buffer != VK_NULL_HANDLE ? offset : 0;
            const VkDeviceSize vk_size = buffer != VK_NULL_HANDLE ? size : VK_WHOLE_SIZE;
            const VkDeviceSize vk_stride = stride;
            cmdbuf.BindVertexBuffers2EXT(index, 1, &buffer, &vk_offset, &vk_size, &vk_stride);
        });
    } else {
        if (!device.HasNullDescriptor() && buffer == VK_NULL_HANDLE) {
            ReserveNullBuffer();
            buffer = *null_buffer;
            offset = 0;
        }
        target.Record([index, buffer, offset](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffer(index, buffer, offset);
        });
    }
}

void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    boost::container::static_vector<VkBuffer, VideoCommon::NUM_VERTEX_BUFFERS> buffer_handles(bindings.buffers.size());
    for (u32 i = 0; i < bindings.buffers.size(); ++i) {
        auto handle = bindings.buffers[i]->Handle();
        if (handle == VK_NULL_HANDLE) {
            bindings.offsets[i] = 0;
            bindings.sizes[i] = VK_WHOLE_SIZE;
            if (!device.HasNullDescriptor()) {
                ReserveNullBuffer();
                handle = *null_buffer;
            }
        }
        buffer_handles[i] = handle;
    }
    const u32 device_max = device.GetMaxVertexInputBindings();
    const u32 min_binding = (std::min)(bindings.min_index, device_max);
    const u32 max_binding = (std::min)(bindings.max_index, device_max);
    const u32 binding_count = max_binding - min_binding;
    if (binding_count == 0) {
        return;
    }
    if (device.IsExtExtendedDynamicStateSupported()) {
        target.Record([bindings_ = std::move(bindings), buffer_handles_ = std::move(buffer_handles), binding_count](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffers2EXT(bindings_.min_index, binding_count, buffer_handles_.data(), bindings_.offsets.data(), bindings_.sizes.data(), bindings_.strides.data());
        });
    } else {
        target.Record([bindings_ = std::move(bindings), buffer_handles_ = std::move(buffer_handles), binding_count](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffers(bindings_.min_index, binding_count, buffer_handles_.data(), bindings_.offsets.data());
        });
    }
}

void BufferCacheRuntime::BindTransformFeedbackBuffer(u32 index, VkBuffer buffer, u32 offset,
                                                     u32 size) {
    if (!device.IsExtTransformFeedbackSupported()) {
        // Already logged in the rasterizer
        return;
    }
    if (buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null transform feedback buffers.
        // Replace it with our own null buffer.
        ReserveNullBuffer();
        buffer = *null_buffer;
        offset = 0;
        size = 0;
    }
    target.Record([index, buffer, offset, size](vk::CommandBuffer cmdbuf) {
        const VkDeviceSize vk_offset = offset;
        const VkDeviceSize vk_size = size;
        cmdbuf.BindTransformFeedbackBuffersEXT(index, 1, &buffer, &vk_offset, &vk_size);
    });
}

void BufferCacheRuntime::BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    if (!device.IsExtTransformFeedbackSupported()) {
        // Already logged in the rasterizer
        return;
    }
    boost::container::static_vector<VkBuffer, VideoCommon::NUM_VERTEX_BUFFERS> buffer_handles(bindings.buffers.size());
    for (u32 i = 0; i < bindings.buffers.size(); ++i) {
        auto handle = bindings.buffers[i]->Handle();
        if (handle == VK_NULL_HANDLE) {
            ReserveNullBuffer();
            handle = *null_buffer;
            bindings.offsets[i] = 0;
            bindings.sizes[i] = 0;
        }
        buffer_handles[i] = handle;
    }
    target.Record([bindings_ = std::move(bindings), buffer_handles_ = std::move(buffer_handles)](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindTransformFeedbackBuffersEXT(0, u32(buffer_handles_.size()), buffer_handles_.data(), bindings_.offsets.data(), bindings_.sizes.data());
    });
}

void BufferCacheRuntime::ReserveNullBuffer() {
    if (!null_buffer) {
        null_buffer = CreateNullBuffer();
    }
}

vk::Buffer BufferCacheRuntime::CreateNullBuffer() {
    VkBufferCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = 4,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        create_info.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        create_info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    vk::Buffer ret = memory_allocator.CreateBuffer(create_info, MemoryUsage::DeviceLocal);
    if (device.HasDebuggingToolAttached()) {
        ret.SetObjectNameEXT("Null buffer");
    }

    target.RequestOutsideRenderPassOperationContext();
    target.Record([buffer = *ret](vk::CommandBuffer cmdbuf) {
        cmdbuf.FillBuffer(buffer, 0, VK_WHOLE_SIZE, 0);
    });

    return ret;
}

} // namespace Vulkan
