// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <climits>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "common/common_types.h"

#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

struct StagingBufferRef {
    VkBuffer buffer;
    VkDeviceAddress device_address;
    VkDeviceSize offset;
    std::span<u8> mapped_span;
    MemoryUsage usage;
    u32 log2_level;
    u64 index;
};

class StagingBufferPool {
public:
    static constexpr size_t NUM_SYNCS = 16;

    explicit StagingBufferPool(const Device& device, MemoryAllocator& memory_allocator,
                               Scheduler& scheduler);
    ~StagingBufferPool();

    StagingBufferRef Request(size_t size, MemoryUsage usage, bool deferred = false);
    void FreeDeferred(StagingBufferRef& ref);

    [[nodiscard]] VkBuffer StreamBuf() const noexcept {
        return *stream_buffer;
    }

    /// The host bytes of an upload range handed out (the stream buffer or an upload staging
    /// buffer), by buffer and offset or by device address; empty if none holds it. Slow:
    /// verification only (EDEN_NX2_LIGHT_UNIFORM=2).
    [[nodiscard]] std::span<const u8> MappedBytes(VkBuffer buffer, VkDeviceSize offset,
                                                  VkDeviceSize size) const;
    [[nodiscard]] std::span<const u8> MappedBytesAt(VkDeviceAddress address,
                                                    VkDeviceSize size) const;

    void TickFrame();

    /// Bytes of the cached staging buffers of one use (DeviceLocal, Upload or Download), free or
    /// in use; not the stream buffer (EDEN_NX2_REACTIVE_STATS).
    [[nodiscard]] u64 CachedBytes(MemoryUsage usage) const noexcept;

private:
    struct StreamBufferCommit {
        size_t upper_bound;
        u64 tick;
    };

    struct StagingBuffer {
        vk::Buffer buffer;
        VkDeviceAddress device_address;
        std::span<u8> mapped_span;
        MemoryUsage usage;
        u32 log2_level;
        u64 index;
        u64 tick = 0;
        bool deferred{};

        StagingBufferRef Ref() const noexcept {
            return {
                .buffer = *buffer,
                .device_address = device_address,
                .offset = 0,
                .mapped_span = mapped_span,
                .usage = usage,
                .log2_level = log2_level,
                .index = index,
            };
        }
    };

    struct StagingBuffers {
        std::vector<StagingBuffer> entries;
        size_t delete_index = 0;
        size_t iterate_index = 0;
    };

    static constexpr size_t NUM_LEVELS = sizeof(size_t) * CHAR_BIT;
    using StagingBuffersCache = std::array<StagingBuffers, NUM_LEVELS>;

    StagingBufferRef GetStreamBuffer(size_t size);

    bool AreRegionsActive(size_t region_begin, size_t region_end) const;

    StagingBufferRef GetStagingBuffer(size_t size, MemoryUsage usage, bool deferred = false);

    std::optional<StagingBufferRef> TryGetReservedBuffer(size_t size, MemoryUsage usage,
                                                         bool deferred);

    StagingBufferRef CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred);

    StagingBuffersCache& GetCache(MemoryUsage usage);

    void ReleaseCache(MemoryUsage usage);

    void ReleaseLevel(StagingBuffersCache& cache, size_t log2);
    size_t Region(size_t iter) const noexcept {
        return iter / region_size;
    }

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;

    vk::Buffer stream_buffer;
    VkDeviceAddress stream_buffer_address{};
    std::span<u8> stream_pointer;
    VkDeviceSize stream_buffer_size;
    VkDeviceSize region_size;

    size_t iterator = 0;
    size_t used_iterator = 0;
    size_t free_iterator = 0;
    std::array<u64, NUM_SYNCS> sync_ticks{};

    StagingBuffersCache device_local_cache;
    StagingBuffersCache upload_cache;
    StagingBuffersCache download_cache;

    size_t current_delete_level = 0;
    u64 buffer_index = 0;
    u64 unique_ids{};
};

/// Cut c (Kesim-2): B's own staging pool (§3.6/2). StagingBufferPool's logic with what it hands out
/// stamped with B's pending seq (Nx2Record::SeqTicks::Stamp) instead of the scheduler's tick, free
/// once C noted the seq and the GPU passed its tick; the texture cache keeps the scheduler's pool.
/// Its stream buffer is half the size. Single thread (B, or any with B parked).
class StageStagingBufferPool {
public:
    /// Indices of what it hands out start here (StagingBufferRef::index tells the pools apart)
    static constexpr u64 INDEX_BASE = u64{1} << 62;

    explicit StageStagingBufferPool(const Device& device, MemoryAllocator& memory_allocator,
                                    Scheduler& scheduler, const u64& pending_seq);
    ~StageStagingBufferPool();

    StagingBufferRef Request(size_t size, MemoryUsage usage, bool deferred = false);
    void FreeDeferred(StagingBufferRef& ref);

    [[nodiscard]] static bool Owns(const StagingBufferRef& ref) noexcept {
        return ref.index >= INDEX_BASE;
    }

    [[nodiscard]] VkBuffer StreamBuf() const noexcept {
        return *stream_buffer;
    }

    /// Times the stream buffer wrapped: a range handed out in this epoch keeps its bytes until the
    /// next one at least (EDEN_NX2_UBO_REUSE's "same tick" while staged).
    [[nodiscard]] u64 StreamEpoch() const noexcept {
        return stream_epoch;
    }

    /// Keeps the stream regions of [offset, offset + size) until what B stamps now is done (a
    /// uniform slice bound again).
    void HoldStream(VkDeviceSize offset, VkDeviceSize size);

    /// As StagingBufferPool's (verification only).
    [[nodiscard]] std::span<const u8> MappedBytes(VkBuffer buffer, VkDeviceSize offset,
                                                  VkDeviceSize size) const;
    [[nodiscard]] std::span<const u8> MappedBytesAt(VkDeviceAddress address,
                                                    VkDeviceSize size) const;

    void TickFrame();

private:
    static constexpr size_t NUM_SYNCS = StagingBufferPool::NUM_SYNCS;

    struct StagingBuffer {
        vk::Buffer buffer;
        VkDeviceAddress device_address;
        std::span<u8> mapped_span;
        MemoryUsage usage;
        u32 log2_level;
        u64 index;
        u64 stamp = 0;
        bool deferred{};

        StagingBufferRef Ref() const noexcept {
            return {
                .buffer = *buffer,
                .device_address = device_address,
                .offset = 0,
                .mapped_span = mapped_span,
                .usage = usage,
                .log2_level = log2_level,
                .index = index,
            };
        }
    };

    struct StagingBuffers {
        std::vector<StagingBuffer> entries;
        size_t delete_index = 0;
        size_t iterate_index = 0;
    };

    static constexpr size_t NUM_LEVELS = sizeof(size_t) * CHAR_BIT;
    using StagingBuffersCache = std::array<StagingBuffers, NUM_LEVELS>;

    /// What B hands out now is stamped with
    [[nodiscard]] u64 Stamp() const noexcept;
    [[nodiscard]] bool IsStampFree(u64 stamp) const noexcept;

    StagingBufferRef GetStreamBuffer(size_t size);
    bool AreRegionsActive(size_t region_begin, size_t region_end) const;
    StagingBufferRef GetStagingBuffer(size_t size, MemoryUsage usage, bool deferred = false);
    std::optional<StagingBufferRef> TryGetReservedBuffer(size_t size, MemoryUsage usage,
                                                         bool deferred);
    StagingBufferRef CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred);
    StagingBuffersCache& GetCache(MemoryUsage usage);
    void ReleaseLevel(StagingBuffersCache& cache, size_t log2);
    size_t Region(size_t iter) const noexcept {
        return iter / region_size;
    }

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    const u64& pending_seq;

    vk::Buffer stream_buffer;
    VkDeviceAddress stream_buffer_address{};
    std::span<u8> stream_pointer;
    VkDeviceSize stream_buffer_size;
    VkDeviceSize region_size;

    size_t iterator = 0;
    size_t used_iterator = 0;
    size_t free_iterator = 0;
    bool recheck_from_start = false; ///< A wrap found region 0 in use: nothing checked yet
    std::array<u64, NUM_SYNCS> sync_stamps{};
    u64 stream_epoch = 0;

    StagingBuffersCache device_local_cache;
    StagingBuffersCache upload_cache;
    StagingBuffersCache download_cache;

    size_t current_delete_level = 0;
    u64 buffer_index = 0;
    u64 unique_ids = INDEX_BASE;
};

} // namespace Vulkan
