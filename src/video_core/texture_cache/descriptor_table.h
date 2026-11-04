// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/assert.h"
#include "common/slot_vector.h"
#include "core/memory.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCommon {

template <typename T>
class DescriptorTable {
public:
    [[nodiscard]] bool Synchronize(GPUVAddr gpu_addr, u32 limit) noexcept {
        bool ret = !(current_gpu_addr == gpu_addr && current_limit == limit);
        if (ret) {
            Refresh(gpu_addr, limit);
        }
        return ret;
    }

    void Invalidate() noexcept {
        // Cleared on the next read: the texture cache invalidates every channel's tables for
        // each image it deletes, dozens of times a frame while its collector runs.
        invalidated = true;
    }

    /// Marks only descriptors whose resolved object is being deleted as unread. Keeping every
    /// unrelated entry hot avoids rebuilding whole texture tables while streamed images churn.
    template <typename Id>
    void InvalidateCachedIds(std::span<const Id> ids) noexcept {
        if (invalidated || ids.empty()) {
            return;
        }
        const size_t count = (std::min)(cached_ids.size(), size_t{current_limit} + 1);
        for (size_t index = 0; index < count; ++index) {
            const u64 bit = u64{1} << (index % 64);
            u64& read_word = read_descriptors[index / 64];
            if ((read_word & bit) == 0) {
                continue;
            }
            const u32 cached = cached_ids[index];
            if (std::ranges::any_of(ids, [cached](Id id) { return id.index == cached; })) {
                read_word &= ~bit;
            }
        }
    }

    template <class MemoryManager>
    [[nodiscard]] std::pair<T, bool> Read(const MemoryManager& gpu_memory, u32 index) noexcept {
        DEBUG_ASSERT(index <= current_limit);
        if (invalidated) {
            ClearReadDescriptors();
            invalidated = false;
        }
        const GPUVAddr gpu_addr = current_gpu_addr + index * sizeof(T);
        std::pair<T, bool> result;
        // Descriptor reads are on every draw. Entries contained in one page are contiguous in
        // host memory, so avoid the generic multi-page walk for the common case.
        const bool one_page =
            (gpu_addr & Core::Memory::YUZU_PAGEMASK) + sizeof(T) <= Core::Memory::YUZU_PAGESIZE;
        if (const u8* const host = one_page ? gpu_memory.GetPointer(gpu_addr) : nullptr) {
            std::memcpy(std::addressof(result.first), host, sizeof(T));
        } else {
            gpu_memory.ReadBlockUnsafe(gpu_addr, std::addressof(result.first), sizeof(T));
        }
        if ((read_descriptors[index / 64] & (1ULL << (index % 64))) != 0) {
            result.second = result.first != descriptors[index];
        } else {
            read_descriptors[index / 64] |= 1ULL << (index % 64);
            result.second = true;
        }
        if (result.second) {
            descriptors[index] = result.first;
        }
        return result;
    }

    /// The resolved cache object for this descriptor index. Call only after Read returned false;
    /// graphics and compute own separate DescriptorTable instances, so no tagged hash key is
    /// needed on the per-draw hot path.
    [[nodiscard]] u32 CachedId(u32 index) const noexcept {
        DEBUG_ASSERT(index < cached_ids.size());
        return cached_ids[index];
    }

    void SetCachedId(u32 index, u32 id) noexcept {
        DEBUG_ASSERT(index < cached_ids.size());
        cached_ids[index] = id;
    }

    void Refresh(GPUVAddr gpu_addr, u32 limit) noexcept {
        current_gpu_addr = gpu_addr;
        current_limit = limit;
        // Mario Brothership reallocates a lot of times, so use aggressive pre-alloc sizes
        // std::vector<T> by default uses quadratic growth, but that isn't even enough to satisfy brothership
        const size_t num_descriptors = ((limit + 0x80000) & (~0x7ffff)) + 1;
        read_descriptors.resize(Common::DivCeil(num_descriptors, 64U));
        ClearReadDescriptors();
        invalidated = false;
        //
        descriptors.resize(num_descriptors);
        cached_ids.resize(num_descriptors, Common::SlotId::INVALID_INDEX);
    }

    std::vector<u64> read_descriptors;
    std::vector<T> descriptors;
    // Descriptor indices are read on every voxel draw. Keeping the resolved SlotId beside the
    // descriptor avoids an unordered-map lookup for every texture and sampler binding.
    std::vector<u32> cached_ids;
    GPUVAddr current_gpu_addr{};
    u32 current_limit{};
    bool invalidated{}; ///< read_descriptors is to be cleared before the next read

private:
    void ClearReadDescriptors() noexcept {
        // Reserved capacity is not addressable. A later limit increase clears its new range.
        const size_t words = (std::min)(read_descriptors.size(), size_t{current_limit} / 64 + 1);
        std::fill_n(read_descriptors.begin(), words, u64{0});
    }
};

} // namespace VideoCommon
