// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <type_traits>
#include <utility>

#include "common/common_types.h"
#include "common/range_sets.h"

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

/// EDEN_NX2_SSBO_FAST (BufferCache::StorageBufferBindingAt): MemoryManager::GetMemoryLayoutSize
/// takes a lock and walks kind_map, a range map whose neighbours of one kind are joined, to
/// answer the end of the run holding the address minus the address. An answer s at q is then the
/// run [q, q + s): any address a in it gets q + s - a, while the manager (owner) and its mapping
/// epoch (bumped after every kind_map change) are the same. Unmapped answers (0) are not kept.
class LayoutRunMemo {
public:
    static constexpr size_t NUM_RUNS = 4;

    /// The size for `addr` from a remembered run.
    bool Find(size_t owner, u64 epoch, u64 addr, u64& size) const noexcept {
        for (const Run& run : runs) {
            if (run.begin <= addr && addr < run.end && run.owner == owner && run.epoch == epoch) {
                size = run.end - addr;
                return true;
            }
        }
        return false;
    }

    /// GetMemoryLayoutSize(addr) returned `size`, with `epoch` read before it.
    void Store(size_t owner, u64 epoch, u64 addr, u64 size) noexcept {
        if (size == 0 || size > ~u64{0} - addr) {
            return;
        }
        runs[next++ % NUM_RUNS] = Run{
            .owner = owner,
            .epoch = epoch,
            .begin = addr,
            .end = addr + size,
        };
    }

private:
    struct Run {
        size_t owner;
        u64 epoch;
        u64 begin;
        u64 end; ///< Equal to begin: empty
    };
    std::array<Run, NUM_RUNS> runs{};
    u32 next{};
};

/// EDEN_NX2_GPU_WRITTEN_FILTER: a Common::RangeSet (boost::icl, a tree under a pool allocator
/// with a global lock) with a summary of where its ranges may lie, so that queries far from all
/// of them answer without the tree: the hull of every range added since the last Clear or
/// Rebuild and, with PageBits, a bit per 4 KiB page of the 64 GiB device space any added range
/// touched (2 MiB, allocated at the first Add; a bit per 4 MiB region marks the regions with
/// any). The summary only grows on Add; Subtract clears the bits of the pages it covers whole
/// (no range can keep bytes there) and Rebuild makes it exact again. A query whose range lies
/// outside the summary meets no range: Intersects is false, ForEachInRange calls nothing and
/// Subtract changes nothing, as the tree would. Mode 0 (the default) passes everything through;
/// mode 2 also asks the tree when the summary answered and counts any disagreement.
template <bool PageBits>
class SummarizedRangeSet {
public:
    static constexpr u32 PAGE_BITS = 12;
    static constexpr u32 REGION_BITS = 22;
    static constexpr u32 SPACE_BITS = 36;
    static constexpr size_t PAGE_WORDS = size_t{1} << (SPACE_BITS - PAGE_BITS - 6);
    static constexpr size_t REGION_WORDS = size_t{1} << (SPACE_BITS - REGION_BITS - 6);
    static constexpr size_t WORDS_PER_REGION = size_t{1} << (REGION_BITS - PAGE_BITS - 6);

    struct Stats {
        u64 queries;         ///< Intersects calls
        u64 query_misses;    ///< Of them answered false by the summary
        u64 false_positives; ///< Of the others the tree answered false
        u64 adds;
        u64 subtracts;
        u64 subtract_misses; ///< Subtract calls the summary left without the tree
        u64 mismatches;      ///< Mode 2: the summary said none, the tree found one
    };

    SummarizedRangeSet() = default;
    SummarizedRangeSet(SummarizedRangeSet&& other) noexcept
        : set{std::move(other.set)}, mode{other.mode}, low{other.low}, high{other.high},
          stats{other.stats} {
        static_assert(!PageBits, "moving the page summary is not supported");
        other.low = ~u64{0};
        other.high = 0;
    }
    SummarizedRangeSet& operator=(SummarizedRangeSet&&) = delete;

    void SetMode(int mode_) {
        mode = mode_;
        Rebuild();
    }

    void Add(u64 addr, size_t size) {
        if (mode != 0 && size != 0) {
            ++stats.adds;
            low = (std::min)(low, addr);
            high = (std::max)(high, addr + size);
            if constexpr (PageBits) {
                MarkPages(addr, size);
            }
        }
        set.Add(addr, size);
    }

    void Subtract(u64 addr, size_t size) {
        if (mode != 0) {
            ++stats.subtracts;
            if (Missed(addr, size)) {
                ++stats.subtract_misses;
                return;
            }
            if constexpr (PageBits) {
                ClearWholePages(addr, size);
            }
        }
        set.Subtract(addr, size);
    }

    [[nodiscard]] bool Intersects(u64 addr, size_t size) {
        if (mode == 0) {
            return set.Intersects(addr, size);
        }
        ++stats.queries;
        if (Missed(addr, size)) {
            ++stats.query_misses;
            return false;
        }
        const bool result = set.Intersects(addr, size);
        stats.false_positives += result ? 0 : 1;
        return result;
    }

    template <typename Func>
    void ForEachInRange(u64 addr, size_t size, Func&& func) const {
        if (mode != 0 && Missed(addr, size)) {
            return;
        }
        set.ForEachInRange(addr, size, std::forward<Func>(func));
    }

    template <typename Func>
    void ForEach(Func&& func) const {
        set.ForEach(std::forward<Func>(func));
    }

    void Clear() {
        set.Clear();
        Reset();
    }

    [[nodiscard]] bool Empty() const {
        return set.Empty();
    }

    /// Makes the summary exact again (the ranges as they are now).
    void Rebuild() {
        Reset();
        if (mode == 0) {
            return;
        }
        set.ForEach([this](u64 begin, u64 end) {
            low = (std::min)(low, begin);
            high = (std::max)(high, end);
            if constexpr (PageBits) {
                MarkPages(begin, end - begin);
            }
        });
        rebuild_false_positives = stats.false_positives;
    }

    /// Whether Rebuild is worth it: many queries the summary let through found nothing.
    [[nodiscard]] bool WantsRebuild() const noexcept {
        return mode != 0 && stats.false_positives - rebuild_false_positives >= 4096;
    }

    [[nodiscard]] size_t Count() const {
        size_t count = 0;
        set.ForEach([&count](u64, u64) { ++count; });
        return count;
    }

    [[nodiscard]] const Stats& GetStats() const noexcept {
        return stats;
    }

    [[nodiscard]] int Mode() const noexcept {
        return mode;
    }

    /// Whether a range may intersect [addr, addr + size) by the summary (false: none does).
    [[nodiscard]] bool MayIntersect(u64 addr, size_t size) const noexcept {
        if (size == 0) {
            return true; // Left to the tree
        }
        const u64 end = addr + size;
        if (end <= low || addr >= high) {
            return false;
        }
        if constexpr (PageBits) {
            if (end > (u64{1} << SPACE_BITS) || end < addr || !pages) {
                return true;
            }
            constexpr u32 REGION_SHIFT = REGION_BITS - PAGE_BITS;
            const u64 first = addr >> PAGE_BITS;
            const u64 last = (end - 1) >> PAGE_BITS;
            for (u64 region = first >> REGION_SHIFT; region <= last >> REGION_SHIFT; ++region) {
                if (((regions[region >> 6] >> (region & 63)) & 1) == 0) {
                    continue;
                }
                const u64 begin_page = (std::max)(first, region << REGION_SHIFT);
                const u64 end_page = (std::min)(last + 1, (region + 1) << REGION_SHIFT);
                if (AnyPage(begin_page, end_page)) {
                    return true;
                }
            }
            return false;
        }
        return true;
    }

private:
    static u64 WordMask(u64 page, u64 count) noexcept {
        return (count == 64 ? ~u64{0} : ((u64{1} << count) - 1)) << (page & 63);
    }

    void Reset() {
        low = ~u64{0};
        high = 0;
        if constexpr (PageBits) {
            if (!pages) {
                return;
            }
            for (size_t word = 0; word < REGION_WORDS; ++word) {
                for (u64 bits = regions[word]; bits != 0; bits &= bits - 1) {
                    const size_t region = word * 64 + static_cast<size_t>(std::countr_zero(bits));
                    std::fill_n(pages.get() + region * WORDS_PER_REGION, WORDS_PER_REGION, 0);
                }
                regions[word] = 0;
            }
        }
    }

    /// The summary answers for the tree (mode 2: only where the tree agrees).
    bool Missed(u64 addr, size_t size) const {
        if (MayIntersect(addr, size)) {
            return false;
        }
        if (mode == 2 && set.Intersects(addr, size)) {
            ++stats.mismatches;
            return false;
        }
        return true;
    }

    void MarkPages(u64 addr, size_t size) {
        // The part in the space: queries reaching past it are never answered by the bits.
        const u64 end = (std::min)(addr + size < addr ? ~u64{0} : addr + size, u64{1} << SPACE_BITS);
        if (addr >= end) {
            return;
        }
        if (!pages) {
            pages = std::make_unique<u64[]>(PAGE_WORDS);
        }
        const u64 last = (end - 1) >> PAGE_BITS;
        for (u64 page = addr >> PAGE_BITS; page <= last;) {
            const u64 word_end = (std::min)(last + 1, (page | 63) + 1);
            pages[page >> 6] |= WordMask(page, word_end - page);
            const u64 region = page >> (REGION_BITS - PAGE_BITS);
            regions[region >> 6] |= u64{1} << (region & 63);
            page = word_end;
        }
    }

    /// Clears the bits of the pages [addr, addr + size) covers whole.
    void ClearWholePages(u64 addr, size_t size) {
        const u64 end = (std::min)(addr + size < addr ? ~u64{0} : addr + size, u64{1} << SPACE_BITS);
        if (!pages || addr >= end) {
            return;
        }
        const u64 end_page = end >> PAGE_BITS;
        for (u64 page = (addr + (u64{1} << PAGE_BITS) - 1) >> PAGE_BITS; page < end_page;) {
            const u64 word_end = (std::min)(end_page, (page | 63) + 1);
            pages[page >> 6] &= ~WordMask(page, word_end - page);
            page = word_end;
        }
    }

    bool AnyPage(u64 begin, u64 end) const noexcept {
        for (u64 page = begin; page < end;) {
            const u64 word_end = (std::min)(end, (page | 63) + 1);
            if ((pages[page >> 6] & WordMask(page, word_end - page)) != 0) {
                return true;
            }
            page = word_end;
        }
        return false;
    }

    struct NoPages {};

    Common::RangeSet<u64> set;
    int mode = 0;
    u64 low = ~u64{0};
    u64 high = 0;
    mutable Stats stats{};
    u64 rebuild_false_positives = 0;
    [[no_unique_address]] std::conditional_t<PageBits, std::unique_ptr<u64[]>, NoPages> pages{};
    [[no_unique_address]] std::conditional_t<PageBits, std::array<u64, REGION_WORDS>, NoPages>
        regions{};
};

} // namespace VideoCommon
