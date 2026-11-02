// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

#include "common/common_types.h"
#include "common/settings.h"
#include "video_core/query_cache/types.h"

// Back stages, cut c (T7): what B decides when the Vulkan rasterizer splits an event between B and
// C, kept apart from the rasterizer so it can be tested without a device.
namespace VideoCommon::Nx2Stages {

/// Whether B sends a query report's query cache half to C through ring 3. A report a fence
/// carries stays on B, the old body in a join (C parked): CounterReport signals a fence for it,
/// whose CommitCacheFlushes and query sync take the buffer cache mutex (C takes it only inside a
/// ParkB, K3), or, a payload release with high GPU accuracy, queues the write for the next fence
/// and keeps the release for PendingSemaphoreRelease. Every other report goes to C: those are the
/// releases that go to C (§7/19 counts them with this).
[[nodiscard]] constexpr bool QueryGoesToC(QueryType type, QueryPropertiesFlags flags,
                                          bool gpu_level_high) noexcept {
    if (True(flags & QueryPropertiesFlags::IsAFence)) {
        return false;
    }
    return !(type == QueryType::Payload && gpu_level_high);
}
[[nodiscard]] inline bool QueryGoesToC(QueryType type, QueryPropertiesFlags flags) {
    return QueryGoesToC(type, flags, Settings::IsGPULevelHigh());
}

/// Whether a query report or counter reset running its old body notes B's "a query since the last
/// WaitForIdle join" (F3: the next WaitForIdle may sync query writes, which takes the buffer cache
/// mutex, and joins). With cut c on every thread but C: A in its join (KeplerCompute's report) and
/// B in a join of its own, the other stages parked. C runs what B sent it, which B noted already;
/// it must not write B's flag.
[[nodiscard]] constexpr bool QueryNotesWfi(bool split_draws, bool on_stage_c) noexcept {
    return split_draws && !on_stage_c;
}

/// B: address ranges [begin, end) of work it sent C that C may not have run yet, each with the
/// last seq it goes with (a packet's, or the next packet's for work sent between packets). A
/// range is done once C ran its seq (`ran`: Nx2GpuPages::RanSeq), all are once B joined (C
/// parked having run everything): Clear.
class InFlightRanges {
public:
    void Note(u64 begin, u64 end, u64 seq, u64 ran) {
        // The same ranges come again and again (a draw's pools, a semaphore): the last ones.
        const size_t count = ranges.size();
        for (size_t i = count; i-- > 0 && i + RecentLooks >= count;) {
            Range& range = ranges[i];
            if (range.begin == begin && range.end == end) {
                range.seq = (std::max)(range.seq, seq);
                return;
            }
        }
        if (count >= prune_at) {
            Drop(ran);
            prune_at = (std::max)(MinPrune, ranges.size() * 2);
        }
        ranges.push_back({.begin = begin, .end = end, .seq = seq});
    }

    /// Whether [begin, end) meets a range C may not have run yet.
    [[nodiscard]] bool Overlaps(u64 begin, u64 end, u64 ran) {
        Drop(ran);
        return std::ranges::any_of(ranges, [begin, end](const Range& range) {
            return begin < range.end && range.begin < end;
        });
    }

    /// Whether any range may be in flight.
    [[nodiscard]] bool Any(u64 ran) {
        Drop(ran);
        return !ranges.empty();
    }

    void Clear() noexcept {
        ranges.clear();
        prune_at = MinPrune;
    }

    [[nodiscard]] size_t Size() const noexcept {
        return ranges.size();
    }

private:
    struct Range {
        u64 begin;
        u64 end;
        u64 seq;
    };
    static constexpr size_t RecentLooks = 2;
    static constexpr size_t MinPrune = 16;

    void Drop(u64 ran) {
        std::erase_if(ranges, [ran](const Range& range) { return range.seq <= ran; });
    }

    std::vector<Range> ranges;
    size_t prune_at = MinPrune;
};

} // namespace VideoCommon::Nx2Stages
