// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/range_sets.h"

namespace VideoCommon {

/// Bumped each time held CPU writes are uploaded (HeldCpuWrites::OnUploaded). The CPU's write path
/// (Memory::HandleRasterizerWrite) skips OnCPUWrite for the page it last held only while this is
/// unchanged: the page is then still in HeldCpuWrites::pages.
inline std::atomic<u32> g_held_cpu_write_epoch{0};

/// Guest CPU writes the buffer cache held (OnCPUWrite returning true: GPU-modified memory) and
/// has not uploaded yet (CachedWriteMemory), by device page. A fence download must not write
/// those pages back when it pops: guest memory holds the CPU's newer bytes there, which the
/// pending upload reads. Pages, not bytes: the CPU write path skips OnCPUWrite for further writes
/// to the page it last held. Their other bytes stay GPU-modified: a later download brings them.
/// The buffer cache's mutex guards it.
template <size_t PageSize>
struct HeldCpuWrites {
    Common::RangeSet<DAddr> pages;

    static DAddr PageBegin(DAddr addr) {
        return Common::AlignDown(addr, PageSize);
    }
    static DAddr PageEnd(DAddr addr, u64 size) {
        return Common::AlignUp(addr + size, PageSize);
    }

    /// A held write: its pages leave every pending download (and its superseded set).
    template <typename Downloads>
    void OnHeldWrite(DAddr addr, u64 size, Downloads& pending_ranges, Downloads& superseded) {
        const DAddr begin = PageBegin(addr);
        const u64 length = PageEnd(addr, size) - begin;
        pages.Add(begin, length);
        for (auto& ranges : pending_ranges) {
            ranges.Subtract(begin, length);
        }
        for (auto& ranges : superseded) {
            ranges.Subtract(begin, length);
        }
    }

    /// A download taken now copies the buffer before the held writes' upload: it must not write
    /// their pages back either.
    void OnCommit(Common::RangeSet<DAddr>& new_ranges, DAddr addr, u64 size) const {
        pages.ForEachInRange(addr, size, [&](DAddr begin, DAddr end) {
            new_ranges.Subtract(begin, end - begin);
        });
    }

    /// The held bytes in [addr, addr + size) were uploaded: downloads taken from now on copy them.
    /// downloadable(begin, length): whether a fence download can cover those pages at all. Only
    /// then do the CPU write paths' short circuits for them have to end (the epoch moves): moving
    /// it on every upload sent each core's next write through OnCPUWrite (the buffer cache's
    /// lock) once per fence.
    template <typename Downloadable>
    void OnUploaded(DAddr addr, u64 size, Downloadable&& downloadable) {
        if (pages.Empty()) {
            return;
        }
        const DAddr begin = PageBegin(addr);
        const u64 length = PageEnd(addr, size) - begin;
        if (pages.Intersects(begin, length)) {
            pages.Subtract(begin, length);
            if (downloadable(begin, length)) {
                BumpEpoch();
            }
        }
    }

    /// Ends every short circuit (pages became downloadable).
    static void BumpEpoch() {
        g_held_cpu_write_epoch.fetch_add(1, std::memory_order_release);
    }
};

} // namespace VideoCommon
