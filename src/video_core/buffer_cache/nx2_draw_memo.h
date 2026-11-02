// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <memory>

#include "common/common_types.h"

namespace VideoCommon {

/// EDEN_NX2_SYNC_SETS (BufferCache::SynchronizeBuffer): ranges left synchronized, kept in 4 KiB
/// pages, four to a set of one cache line, the set picked by the 64 KiB block of the start.
/// ForEachUploadRange clears the CPU and untracked bits of every page the range touches (its
/// masks are whole pages), so after a synchronization the range's pages are clean, not only its
/// bytes: neighbours on the same page (storage buffers of one object after another, index ranges
/// of meshes packed back to back) then find the same entry. A set holds four ranges, so ranges
/// whose blocks share a set no longer evict each other at once.
class SyncMemoSets {
public:
    static constexpr u32 PAGE_BITS = 12;
    static constexpr size_t NUM_SETS = 2048;
    static constexpr size_t NUM_WAYS = 4;

    struct Way {
        u32 begin_page;
        u32 end_page;
        u64 epoch; ///< 0: empty (the tracker's epoch starts at 1)
    };
    struct alignas(64) Set {
        std::array<Way, NUM_WAYS> ways;
    };

    /// Pages [first, end) of [addr, addr + size); false when they do not fit 32 bits.
    static bool Pages(u64 addr, u64 size, u32& first, u32& end) noexcept {
        const u64 last_page = (addr + size + (u64{1} << PAGE_BITS) - 1) >> PAGE_BITS;
        if (size == 0 || last_page > 0xFFFF'FFFFULL) {
            return false;
        }
        first = static_cast<u32>(addr >> PAGE_BITS);
        end = static_cast<u32>(last_page);
        return true;
    }

    Set& SetOf(u64 addr) {
        if (!sets) {
            sets = std::make_unique<Set[]>(NUM_SETS);
        }
        return sets[((addr >> 16) ^ (addr >> 26)) & (NUM_SETS - 1)];
    }

    /// A way of `set` covering pages [first, end) for which holds(begin, size, epoch) (the range
    /// still clean since: MemoryTracker::UnsetSince over the query), else null.
    template <typename Holds>
    static const Way* Find(const Set& set, u32 first, u32 end, Holds&& holds) {
        for (const Way& way : set.ways) {
            if (way.epoch != 0 && way.begin_page <= first && end <= way.end_page &&
                holds(way.epoch)) {
                return &way;
            }
        }
        return nullptr;
    }

    /// Why Find found nothing (profiling): 0 the set is empty, 1 a way covers the pages (but no
    /// longer holds), 2 one overlaps or touches them, 3 none does.
    static int MissKind(const Set& set, u32 first, u32 end) {
        bool any = false;
        bool touching = false;
        for (const Way& way : set.ways) {
            if (way.epoch == 0) {
                continue;
            }
            if (way.begin_page <= first && end <= way.end_page) {
                return 1;
            }
            any = true;
            touching |= way.begin_page <= end && first <= way.end_page;
        }
        return !any ? 0 : touching ? 2 : 3;
    }

    /// Pages [first, end) are clean as of `epoch`. A way overlapping or touching them that still
    /// holds (holds(begin_byte, size_bytes, way_epoch)) takes their union; one that no longer
    /// holds is replaced by them; else an empty way, else the oldest.
    template <typename Holds>
    static void Store(Set& set, u32 first, u32 end, u64 epoch, Holds&& holds) {
        Way* target = nullptr;
        for (Way& way : set.ways) {
            if (way.epoch != 0 && way.begin_page <= end && first <= way.end_page) {
                if (holds(u64{way.begin_page} << PAGE_BITS,
                          u64{way.end_page - way.begin_page} << PAGE_BITS, way.epoch)) {
                    way.begin_page = (std::min)(way.begin_page, first);
                    way.end_page = (std::max)(way.end_page, end);
                    way.epoch = epoch;
                    return;
                }
                target = &way;
                break;
            }
        }
        if (target == nullptr) {
            target = &set.ways[0];
            for (Way& way : set.ways) {
                if (way.epoch < target->epoch) {
                    target = &way;
                }
            }
        }
        *target = Way{.begin_page = first, .end_page = end, .epoch = epoch};
    }

private:
    std::unique_ptr<Set[]> sets;
};

} // namespace VideoCommon
