// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "video_core/memory_manager.h"

namespace VideoCommon {

struct VirtualSegment {
    GPUVAddr gpu_addr;
    DAddr device_addr;
    u32 size;
};

using VirtualSegments = boost::container::small_vector<VirtualSegment, 8>;

/// EDEN_NX2_SSBO_MEMO=0: storage buffer bindings and their virtual ranges are resolved again for
/// every draw, as before. Read once, like g_nx2_sync_memo (both per-draw paths test it).
inline const bool g_nx2_ssbo_memo = [] {
    const char* value = std::getenv("EDEN_NX2_SSBO_MEMO");
    return value == nullptr || value[0] != '0';
}();

class VirtualRangeCache {
public:
    static constexpr size_t MAX_ENTRIES = 8192;
    static constexpr size_t MAX_DEFERRED = 4096;

    const VirtualSegments* Query(Tegra::MemoryManager& memory, GPUVAddr gpu_addr, u32 size) {
        if (has_deferred.load(std::memory_order_acquire)) {
            ApplyDeferred();
        }
        if (entries.size() > MAX_ENTRIES) {
            entries.clear();
            ++changes;
        }
        const size_t as_id = memory.GetID();
        const u64 key = MakeKey(as_id, gpu_addr);
        const auto it = entries.find(key);
        if (it != entries.end() && it->second.as_id == as_id &&
            it->second.gpu_addr == gpu_addr && it->second.size == size) {
            return &it->second.segments;
        }
        Entry entry{};
        entry.as_id = as_id;
        entry.gpu_addr = gpu_addr;
        entry.size = size;
        const auto ranges = memory.GetSubmappedRange(gpu_addr, size);
        GPUVAddr expected = gpu_addr;
        bool contiguous = true;
        for (const auto& [range_addr, range_size] : ranges) {
            if (range_addr != expected || range_size == 0) {
                contiguous = false;
                break;
            }
            const std::optional<DAddr> device_addr = memory.GpuToCpuAddress(range_addr);
            if (!device_addr || *device_addr == 0) {
                contiguous = false;
                break;
            }
            if (range_size > static_cast<size_t>((std::numeric_limits<u32>::max)())) {
                contiguous = false;
                break;
            }
            entry.segments.push_back(VirtualSegment{
                .gpu_addr = range_addr,
                .device_addr = *device_addr,
                .size = static_cast<u32>(range_size),
            });
            expected += range_size;
        }
        if (!contiguous || expected != gpu_addr + size) {
            entry.segments.clear();
        }
        const auto result = entries.insert_or_assign(key, std::move(entry));
        ++changes;
        return &result.first->second.segments;
    }

    /// Query's segments when the range has two or more of them, else null.
    /// EDEN_NX2_SSBO_MEMO: each draw queried every storage buffer of every stage again, nearly
    /// always to find one segment (~1.3% of the split back on the wide view, in the map lookup).
    /// The answers are remembered, direct-mapped by address, with the change count of entries:
    /// while no entry was added, replaced or dropped, the map is untouched, so Query's lookup
    /// would find the same entry at the same place and return the same segments.
    const VirtualSegments* QueryMultiRange(Tegra::MemoryManager& memory, GPUVAddr gpu_addr,
                                           u32 size) {
        if (!g_nx2_ssbo_memo) {
            const VirtualSegments* const found = Query(memory, gpu_addr, size);
            return found != nullptr && found->size() >= 2 ? found : nullptr;
        }
        // Query's own upkeep first, in its order; the remembered answer stands for its lookup.
        if (has_deferred.load(std::memory_order_acquire)) {
            ApplyDeferred();
        }
        if (entries.size() > MAX_ENTRIES) {
            entries.clear();
            ++changes;
        }
        const size_t as_id = memory.GetID();
        FrontEntry& front =
            front_entries[static_cast<size_t>((gpu_addr >> 8) ^ (gpu_addr >> 20)) &
                          (FRONT_SIZE - 1)];
        if (front.changes == changes && front.gpu_addr == gpu_addr && front.as_id == as_id &&
            front.size == size) {
            return front.multi;
        }
        const VirtualSegments* const found = Query(memory, gpu_addr, size);
        const VirtualSegments* const multi =
            found != nullptr && found->size() >= 2 ? found : nullptr;
        // The count after Query: the entry it returned stays where it is until the count moves.
        front = FrontEntry{
            .gpu_addr = gpu_addr,
            .changes = changes,
            .as_id = as_id,
            .multi = multi,
            .size = size,
        };
        return multi;
    }

    void Unmap(size_t as_id, GPUVAddr gpu_addr, u64 size) {
        if (size == 0) {
            return;
        }
        {
            std::scoped_lock lock{deferred_mutex};
            if (!deferred.empty()) {
                DeferredUnmap& last = deferred.back();
                if (last.as_id == as_id && last.gpu_addr + last.size == gpu_addr) {
                    last.size += size;
                    has_deferred.store(true, std::memory_order_release);
                    return;
                }
            }
            if (deferred.size() >= MAX_DEFERRED) {
                deferred.clear();
                deferred_overflow = true;
            } else {
                deferred.push_back(DeferredUnmap{
                    .as_id = as_id,
                    .gpu_addr = gpu_addr,
                    .size = size,
                });
            }
        }
        has_deferred.store(true, std::memory_order_release);
    }

private:
    struct Entry {
        VirtualSegments segments;
        size_t as_id{};
        GPUVAddr gpu_addr{};
        u32 size{};
    };

    struct DeferredUnmap {
        size_t as_id;
        GPUVAddr gpu_addr;
        u64 size;
    };

    static u64 MakeKey(size_t as_id, GPUVAddr gpu_addr) {
        return (static_cast<u64>(as_id) << 48) ^ gpu_addr;
    }

    void ApplyDeferred() {
        std::vector<DeferredUnmap> pending;
        bool overflow = false;
        {
            std::scoped_lock lock{deferred_mutex};
            has_deferred.store(false, std::memory_order_release);
            pending.swap(deferred);
            overflow = deferred_overflow;
            deferred_overflow = false;
        }
        if (overflow) {
            entries.clear();
            ++changes;
            return;
        }
        if (pending.empty() || entries.empty()) {
            return;
        }
        for (auto it = entries.begin(); it != entries.end();) {
            const Entry& entry = it->second;
            const GPUVAddr entry_end = entry.gpu_addr + entry.size;
            bool overlaps = false;
            for (const DeferredUnmap& unmap : pending) {
                if (unmap.as_id != entry.as_id) {
                    continue;
                }
                if (entry.gpu_addr < unmap.gpu_addr + unmap.size && unmap.gpu_addr < entry_end) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps) {
                it = entries.erase(it);
                ++changes;
            } else {
                ++it;
            }
        }
    }

    /// A QueryMultiRange answer (EDEN_NX2_SSBO_MEMO), valid while changes holds.
    struct FrontEntry {
        GPUVAddr gpu_addr{};
        u64 changes{};
        size_t as_id{};
        const VirtualSegments* multi{};
        u32 size{};
    };
    static constexpr size_t FRONT_SIZE = 128;

    ::Common::unordered_map<u64, Entry> entries;
    /// Bumped whenever entries gains, replaces or loses an entry (clear included). Starts above
    /// the zero of unused front entries.
    u64 changes = 1;
    std::array<FrontEntry, FRONT_SIZE> front_entries{};
    std::vector<DeferredUnmap> deferred;
    std::mutex deferred_mutex;
    std::atomic<bool> has_deferred{false};
    bool deferred_overflow{};
};

} // namespace VideoCommon
