// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>

#include "common/common_types.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

// Back stages, cut c (Kesim-2): B skips constant buffer writes of held bytes (EDEN_NX2_SAME_SKIP)
// only where no cache holds GPU-written data (HoldsGpuWrittenData), and the texture and query
// caches are C's. A lock-free summary per device page (4 KiB) of where C may hold GPU-written
// data (images, pending query results, writes around the caches) lets B answer without C's lock.
// A page holds a sequence number, not a bit (S12): B sets pages for the draws and queries it
// sends C before it sends them, conservatively (D6); C clears a page only when the sequence it
// ran up to is at least the page's, so an older image's clean-up never clears what B set for a
// later draw. The sequence is B's draw sequence (DrawToC::seq, u64, K5); a page may keep a
// narrower copy compared with SeqAtLeast (nx2_stage_ring.h): a page left behind by half that
// range reads as later and is kept, the safe side. Owner: T8.
//
// How (T8). What B sets and what C holds are kept apart, so that no clean-up of C's can take
// back what B set:
// - B's sets (PreSet) are B's own: Holds runs on B alone. B keeps them as ranges with the seq
//   of the packet they come before (a draw's render targets are megabytes: a store per page and
//   draw would cost thousands of stores a draw), the same range set again only moving its seq.
//   A set range holds until C ran its seq (Ran): from then on what C holds there is in C's marks.
// - C's marks: per device page an atomic u64, the sources holding the page (bits 0-7: an image
//   marked GPU-modified, Set) and when they were last marked (bits 8-63, the writers' order). A
//   mark is made at once; a clean-up is queued and made at a safe point (Settle: the texture
//   cache's TickFrame), between the cache's operations, only on pages no mark since the clean-up
//   touched: an image deleted inside an operation that registers its successor (JoinImages)
//   never shows a hole. A clean-up made late only keeps a page longer (the safe side). The
//   texture cache queues only pages no other GPU-modified image covers (Nx2UnmarkUncovered).
// - The writers of C's marks hold the texture cache's lock: C, and the threads that flush or
//   unmap through it (RasterizerVulkan::FlushRegion, UnmapMemory, under texture_cache.mutex).
// - Writes around the caches (query results, the texture swizzle) mark Tegra::Nx2Untracked
//   pages, which never clear within a session; Holds reads them after its acquire, so C's marks
//   from before a seq it ran are seen. A query result's page is marked so when the query is
//   reported (QueryCacheBase::CounterReport), before it is cached: HoldsQueries' pages too.
// - The pages are lazily allocated blocks: 4096 pages (16 MiB of device memory) take 32 KiB.
//
// Memory order. B's sets are read by B alone (no order needed between threads). C publishes the
// seq it ran up to with a release store after every mark it made for the records before it;
// Holds loads it with acquire before it reads any mark or untracked page: what C did for a seq
// Holds no longer counts as set is visible. Marks C makes for a later record may be seen early
// (they only add) and clean-ups late (B's set for the record still holds). A block pointer is
// published with release and loaded with acquire (its zeroed words before it).
//
// Off (EDEN_NX2_BACK_STAGES 0 or 3, or 1 without cut c and STAGE_CHECK; until Start): the cache
// hooks test Live() (one relaxed load and a branch) and do nothing else; Holds says every page
// may hold data, the skeleton's answer.
namespace VideoCommon::Nx2Stages {

class Nx2GpuPages {
public:
    static constexpr u32 PageBits = 12;    ///< Core::DEVICE_PAGEBITS
    static constexpr u32 AddressBits = 36; ///< As Tegra::Nx2Untracked's bitmap: pages above hold
    static constexpr u32 BlockBits = 12;   ///< Pages per lazily allocated block (32 KiB)
    static constexpr u64 Pages = u64{1} << (AddressBits - PageBits);
    static constexpr u64 Blocks = Pages >> BlockBits;
    static constexpr u64 BlockPages = u64{1} << BlockBits;
    static constexpr u32 MaxPresets = 64;  ///< B's set ranges in flight; more: all pages hold
    static constexpr u32 MaxClears = 1024; ///< Clean-ups queued until C's next safe point; more:
                                           ///< dropped, the pages stay marked (the safe side)
    static constexpr u64 SourceOther = 1;  ///< Set / Clear
    static constexpr u64 SourceImage = 2;  ///< MarkImage / UnmarkImage
    static constexpr u64 SourceMask = 0xff;

    constexpr Nx2GpuPages() = default;
    Nx2GpuPages(const Nx2GpuPages&) = delete;
    Nx2GpuPages& operator=(const Nx2GpuPages&) = delete;
    ~Nx2GpuPages() {
        for (auto* const dir : {&held_blocks, &relied_blocks}) {
            for (auto& block : *dir) {
                delete[] block.load(std::memory_order_relaxed);
            }
        }
    }

    /// B, before sending draw or query `seq` to C: the render targets, depth, storage images
    /// and query results it may write. `seq` is the packet's (DrawToC::seq) or, for work sent
    /// between packets, the next packet's: C runs it before it runs that seq. A seq below B's
    /// last counts as the last (later, the safe side).
    void PreSet(DAddr begin, u64 size, u64 seq) {
        if (preset_count != 0 && seq < presets[preset_count - 1].seq) {
            seq = presets[preset_count - 1].seq;
        }
        b_next = (std::max)(b_next, seq);
        if (size == 0) {
            return;
        }
        // What C ran is in its marks now.
        const u64 done = ran.load(std::memory_order_relaxed);
        u32 expired = 0;
        while (expired < preset_count && presets[expired].seq <= done) {
            ++expired;
        }
        if (expired != 0) {
            std::copy(presets.begin() + expired, presets.begin() + preset_count, presets.begin());
            preset_count -= expired;
        }
        const DAddr end = begin + size;
        // Kept in seq order: Holds stops at the first range C ran.
        for (u32 i = preset_count; i-- > 0;) {
            if (presets[i].begin == begin && presets[i].end == end) {
                std::rotate(presets.begin() + i, presets.begin() + i + 1,
                            presets.begin() + preset_count);
                presets[preset_count - 1].seq = seq;
                return;
            }
        }
        if (preset_count == MaxPresets) {
            all_until = (std::max)(all_until, seq);
            return;
        }
        presets[preset_count++] = Preset{begin, end, seq};
    }

    /// C: pages it holds GPU-written data for, as of its `seq` (an image marked GPU-modified, a
    /// pending query, a write around the caches). Images: MarkImage; writes around the caches
    /// mark Tegra::Nx2Untracked. Marks at once. With the texture cache's lock (as every writer
    /// of C's marks).
    void Set(DAddr begin, u64 size, u64 seq) {
        Mark(begin, size, SourceOther, seq);
    }
    /// C: pages that no longer hold any, as of its `seq`; a page set for a later seq stays.
    /// Queued until the next safe point (Settle); a page marked since stays marked, and B's sets
    /// are not C's to clear (a set range holds until C ran its seq). With the texture cache's
    /// lock.
    void Clear(DAddr begin, u64 size, [[maybe_unused]] u64 seq) {
        QueueClear(begin, size, SourceOther);
    }

    /// B, lock-free: whether a page of [begin, begin + size) may hold C's GPU-written data. A
    /// relaxed and an acquire load, then per page a block pointer, the page's word and its
    /// untracked word; B's own ranges in flight are plain memory.
    [[nodiscard]] bool Holds(DAddr begin, u64 size) const {
        if (!live.load(std::memory_order_relaxed)) {
            return true;
        }
        // Acquire: C's marks for every record up to `done` (see Memory order above).
        const u64 done = ran.load(std::memory_order_acquire);
        const DAddr end = begin + (std::max)(size, u64{1});
        if (all_until > done) {
            return true;
        }
        for (u32 i = preset_count; i-- > 0;) {
            const Preset& preset = presets[i];
            if (preset.seq <= done) {
                break;
            }
            if (preset.begin < end && begin < preset.end) {
                return true;
            }
        }
        const u64 first = begin >> PageBits;
        const u64 last = (end - 1) >> PageBits;
        for (u64 page = first; page <= last; ++page) {
            if (page >= Pages || (Word(held_blocks, page) & SourceMask) != 0) {
                return true;
            }
        }
        if (Tegra::Nx2UntrackedWritten(begin, end - begin)) {
            return true;
        }
        if (check.load(std::memory_order_relaxed)) [[unlikely]] {
            NoteRelied(first, last);
        }
        return false;
    }

    // T8's own, beyond the contract (its signatures above stay as they are):

    /// Whether the cache hooks keep the summary (Start to Stop). Any thread.
    [[nodiscard]] bool Live() const {
        return live.load(std::memory_order_relaxed);
    }
    /// =2 or EDEN_NX2_STAGE_CHECK: the hazard counters (HazardPages, HazardUntracked,
    /// HazardEpoch).
    [[nodiscard]] bool Checking() const {
        return check.load(std::memory_order_relaxed);
    }

    /// Before any image exists and no stage runs (the texture cache's constructor): empty and
    /// kept by the hooks from now on.
    void Start(bool check_hazards) {
        Reset();
        check.store(check_hazards, std::memory_order_relaxed);
        live.store(true, std::memory_order_release);
    }
    /// The hooks stop keeping it; every page may hold data again.
    void Stop() {
        live.store(false, std::memory_order_release);
        check.store(false, std::memory_order_relaxed);
    }

    /// With the texture cache's lock, between its operations (its TickFrame; C may add points
    /// of its own, holding the lock between records): makes the queued clean-ups.
    void Settle() {
        for (u32 i = 0; i < clear_count; ++i) {
            const QueuedClear& clear = clears[i];
            for (u64 page = clear.first; page <= clear.last; ++page) {
                std::atomic<u64>* const block =
                    held_blocks[page >> BlockBits].load(std::memory_order_relaxed);
                if (block == nullptr) {
                    page |= BlockPages - 1;
                    continue;
                }
                std::atomic<u64>& word = block[page & (BlockPages - 1)];
                const u64 value = word.load(std::memory_order_relaxed);
                // Marked since the clean-up: it holds again.
                if ((value & clear.source) == 0 || (value >> 8) > clear.gen) {
                    continue;
                }
                const u64 left = value & ~clear.source;
                word.store((left & SourceMask) != 0 ? left : 0, std::memory_order_relaxed);
            }
        }
        clear_count = 0;
    }
    /// C, after the last command of packet `seq` (where it calls Scheduler::NoteSeqTick) and
    /// everything sent before it: publishes `seq` (release). Needs no lock: it touches nothing
    /// else (clean-ups wait for Settle).
    void Ran(u64 seq) {
        ran.store(seq, std::memory_order_release);
    }
    /// The seq C ran up to (Ran). Any thread.
    [[nodiscard]] u64 RanSeq() const {
        return ran.load(std::memory_order_acquire);
    }

    /// B, before sending work `seq` whose writes it cannot place (a dispatch or a draw with storage
    /// images it does not resolve, a copy into an image): every page holds until C ran `seq`.
    void PreSetAll(u64 seq) {
        PreSet(0, 0, seq);
        all_until = (std::max)(all_until, b_next);
    }

    /// C, the texture cache (with its lock): an image's device range now holds GPU-written data.
    void MarkImage(DAddr begin, u64 size) {
        Mark(begin, size, SourceImage, ran.load(std::memory_order_relaxed) + 1);
    }
    /// C, the texture cache (with its lock): pages no image marked GPU-modified covers any more
    /// (queued as Clear).
    void UnmarkImage(DAddr begin, u64 size) {
        QueueClear(begin, size, SourceImage);
    }

    /// C, STAGE_CHECK: C wrote [begin, begin + size) around the caches (it marked it
    /// Tegra::Nx2Untracked): counts HazardUntracked if B already relied on the pages holding
    /// nothing at a point of the stream after this write.
    void NoteUntracked(DAddr begin, u64 size) {
        if (Checking() && size != 0) {
            CheckRelied(begin >> PageBits, (begin + size - 1) >> PageBits,
                        ran.load(std::memory_order_relaxed) + 1,
                        Nx2Profile::Counter::HazardUntracked);
        }
    }
    /// STAGE_CHECK, where the caller has the cache's answer at the same point of the stream (BC
    /// in Kesim-1, a join, =2's lockstep): Holds, counting HazardPages when the caches hold data
    /// the summary says no page holds.
    bool CheckHolds(DAddr begin, u64 size, bool caches_hold) {
        const bool holds = Holds(begin, size);
        if (caches_hold && !holds && Checking()) {
            Nx2Profile::Add(Nx2Profile::Counter::HazardPages, 1);
        }
        return holds;
    }
    /// STAGE_CHECK, C running work that bumps the light epoch (RasterizerVulkan::NoteLightWork):
    /// HazardEpoch when B's copy of the epoch, as the record carries it, counted less of such
    /// work than C ran by then (T7 counts both).
    void CheckEpoch(u64 counted_by_b, u64 run_by_c) {
        if (Checking() && counted_by_b < run_by_c) {
            Nx2Profile::Add(Nx2Profile::Counter::HazardEpoch, 1);
        }
    }

    /// No stage runs: everything forgotten, the allocated blocks kept (zeroed).
    void Reset() {
        for (auto* const dir : {&held_blocks, &relied_blocks}) {
            for (auto& block_pointer : *dir) {
                std::atomic<u64>* const block = block_pointer.load(std::memory_order_relaxed);
                if (block != nullptr) {
                    for (u64 i = 0; i < BlockPages; ++i) {
                        block[i].store(0, std::memory_order_relaxed);
                    }
                }
            }
        }
        preset_count = 0;
        all_until = 0;
        b_next = 0;
        clear_count = 0;
        c_gen = 0;
        ran.store(0, std::memory_order_release);
    }

private:
    using Directory = std::array<std::atomic<std::atomic<u64>*>, Blocks>;

    struct Preset {
        DAddr begin;
        DAddr end;
        u64 seq;
    };
    struct QueuedClear {
        u64 first;
        u64 last;
        u64 gen;
        u64 source;
    };

    static u64 Word(const Directory& dir, u64 page) {
        const std::atomic<u64>* const block =
            dir[page >> BlockBits].load(std::memory_order_acquire);
        return block != nullptr ? block[page & (BlockPages - 1)].load(std::memory_order_relaxed)
                                : 0;
    }
    /// The directory's writer (one at a time): allocates a zeroed block and publishes it (release).
    static std::atomic<u64>& WordForWrite(Directory& dir, u64 page) {
        std::atomic<u64>* block = dir[page >> BlockBits].load(std::memory_order_relaxed);
        if (block == nullptr) {
            block = new std::atomic<u64>[BlockPages]();
            dir[page >> BlockBits].store(block, std::memory_order_release);
        }
        return block[page & (BlockPages - 1)];
    }

    void Mark(DAddr begin, u64 size, u64 source, u64 position) {
        if (size == 0) {
            return;
        }
        const u64 first = begin >> PageBits;
        const u64 last = (std::min)((begin + size - 1) >> PageBits, Pages - 1);
        if (first > last) {
            return;
        }
        const u64 gen = ++c_gen;
        for (u64 page = first; page <= last; ++page) {
            std::atomic<u64>& word = WordForWrite(held_blocks, page);
            const u64 value = word.load(std::memory_order_relaxed);
            word.store((gen << 8) | (value & SourceMask) | source, std::memory_order_relaxed);
        }
        if (Checking()) [[unlikely]] {
            CheckRelied(first, last, position, Nx2Profile::Counter::HazardPages);
        }
    }

    void QueueClear(DAddr begin, u64 size, u64 source) {
        if (size == 0 || clear_count == MaxClears) {
            return;
        }
        const u64 first = begin >> PageBits;
        const u64 last = (std::min)((begin + size - 1) >> PageBits, Pages - 1);
        if (first > last) {
            return;
        }
        clears[clear_count++] = QueuedClear{first, last, c_gen, source};
    }

    /// B, STAGE_CHECK: Holds said no for these pages with B about to send packet b_next.
    void NoteRelied(u64 first, u64 last) const {
        for (u64 page = first; page <= last; ++page) {
            WordForWrite(relied_blocks, page).store(b_next, std::memory_order_relaxed);
        }
    }
    /// C, STAGE_CHECK: C does something on the pages within packet `position` (or before it, after
    /// the one before). B relied on them holding nothing about to send a later packet: then C's
    /// work came first in the stream. Within the same packet the order is not known: not counted
    /// (the cache comparison, CheckHolds, sees those).
    void CheckRelied(u64 first, u64 last, u64 position, Nx2Profile::Counter counter) const {
        for (u64 page = first; page <= last && page < Pages; ++page) {
            if (Word(relied_blocks, page) > position) {
                Nx2Profile::Add(counter, 1);
                return;
            }
        }
    }

    // Any thread.
    std::atomic<bool> live{};
    std::atomic<bool> check{};
    /// The seq C ran up to (Ran): C stores (release), B loads (acquire).
    alignas(64) std::atomic<u64> ran{};

    // B's own.
    alignas(64) u32 preset_count{};
    u64 all_until{}; ///< Every page holds until C ran this seq (MaxPresets ranges in flight)
    u64 b_next{};    ///< The last seq B set pages for: where B is in the stream (STAGE_CHECK)
    std::array<Preset, MaxPresets> presets{};

    // C's own: its writers hold the texture cache's lock.
    alignas(64) u32 clear_count{};
    u64 c_gen{}; ///< C's marks so far: a clean-up queued after gen n clears no page marked later
    std::array<QueuedClear, MaxClears> clears{};

    /// C's marks: C allocates and writes, B reads.
    alignas(64) Directory held_blocks{};
    /// STAGE_CHECK: per page, b_next when Holds last said no for it. B allocates and writes, C
    /// reads (CheckRelied).
    mutable Directory relied_blocks{};
};

namespace Detail {
inline Nx2GpuPages g_gpu_pages;
}

/// The summary B asks (cut c) and C's texture and query caches keep.
inline Nx2GpuPages& GpuPages() {
    return Detail::g_gpu_pages;
}

/// B: PreSet for a GPU range, page by page through the memory manager, contiguous device pages as
/// one range (a GPU range may map to scattered device pages). It walks every 4 KiB page: B keeps
/// the ranges of its bound targets and calls it when they change, PreSet for each packet.
/// `MemoryManager` is Tegra::MemoryManager.
template <typename MemoryManager>
void PreSetGpu(Nx2GpuPages& pages, const MemoryManager& memory_manager, GPUVAddr gpu_addr,
               u64 size, u64 seq) {
    DAddr run_begin = 0;
    DAddr run_end = 0;
    for (GPUVAddr page = gpu_addr & ~GPUVAddr{0xfff}; page < gpu_addr + size; page += 0x1000) {
        const std::optional<DAddr> device = memory_manager.GpuToCpuAddress(page);
        if (device && *device == run_end && run_end != run_begin) {
            run_end += 0x1000;
            continue;
        }
        if (run_end != run_begin) {
            pages.PreSet(run_begin, run_end - run_begin, seq);
        }
        run_begin = device ? *device : 0;
        run_end = device ? *device + 0x1000 : 0;
    }
    if (run_end != run_begin) {
        pages.PreSet(run_begin, run_end - run_begin, seq);
    }
}

/// C, STAGE_CHECK: a write around the caches at a GPU address (the texture swizzle), as
/// Nx2GpuPages::NoteUntracked page by page. `MemoryManager` is Tegra::MemoryManager.
template <typename MemoryManager>
void NoteUntrackedGpu(const MemoryManager& memory_manager, GPUVAddr gpu_addr, u64 size) {
    for (GPUVAddr page = gpu_addr & ~GPUVAddr{0xfff}; page < gpu_addr + size; page += 0x1000) {
        if (const std::optional<DAddr> device = memory_manager.GpuToCpuAddress(page)) {
            GpuPages().NoteUntracked(*device, 1);
        }
    }
}

} // namespace VideoCommon::Nx2Stages
