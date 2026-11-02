// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/settings.h"
#include "video_core/cache_types.h"
#include "video_core/nx2_stage_draw.h"
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

/// For a report that goes to C (QueryGoesToC): whether C writes it at once (a payload release:
/// CounterReport's memcpy) or from a fence after it (a counter: SyncOperation), which an
/// invalidation of its bytes before then cancels (QueryCacheBase::InvalidateRegion).
[[nodiscard]] constexpr bool QueryWritesAtOnce(QueryType type) noexcept {
    return type == QueryType::Payload;
}

/// Whether a query report or counter reset running its old body notes B's "a query since the last
/// WaitForIdle join" (F3: the next WaitForIdle may sync query writes, which takes the buffer cache
/// mutex, and joins). With cut c on every thread but C: A in its join (KeplerCompute's report) and
/// B in a join of its own, the other stages parked. C runs what B sent it, which B noted already;
/// it must not write B's flag.
[[nodiscard]] constexpr bool QueryNotesWfi(bool split_draws, bool on_stage_c) noexcept {
    return split_draws && !on_stage_c;
}

/// The fence calls that reach B's rasterizer outside a join: the puller's deferred ones come
/// without the proxy (T9c, R2 F3).
enum class FenceCall : u8 { Signal, Sync, SyncPoint, Reference, Release };

/// Whether B runs a fence call in a join of its own (C parked having run everything B sent
/// before, B the record owner). The fence manager records C's caches' and the query cache's async
/// flushes and the scheduler's flush, and CommitCacheFlushes takes the buffer cache mutex. A
/// release without `force` checks only fences that passed, which an async check
/// (FenceManagerParams::HAS_ASYNC_CHECK) leaves to the fence thread: nothing to join for.
[[nodiscard]] constexpr bool FenceJoinsB(FenceCall call, bool force, bool async_check) noexcept {
    return call != FenceCall::Release || force || !async_check;
}

/// The caches a write the CPU made invalidates (OnCachedWrite, OnCacheInvalidation): every one but
/// the query cache, as their old bodies.
inline constexpr CacheType CpuWriteCaches = CacheType::NoQueryCache;

/// An invalidation's halves with cut c (InvalidateRegionB): the caches C invalidates, in ring 3's
/// order before anything B sends after, and those B does (the buffer cache's copies in a packet
/// of B's, the shaders).
struct CacheHalves {
    CacheType c;
    CacheType b;
};
[[nodiscard]] constexpr CacheHalves SplitCaches(CacheType which) noexcept {
    constexpr CacheType on_c = CacheType::TextureCache | CacheType::QueryCache;
    return {.c = which & on_c, .b = which & CacheType(~static_cast<u32>(on_c))};
}

/// The end of a texture header or sampler pool of `limit` + 1 entries (32 bytes each) at `pool`.
[[nodiscard]] constexpr u64 PoolEnd(u64 pool, u32 limit) noexcept {
    return pool + (u64{limit} + 1) * 32;
}

/// Whether a write of [address, address + size) meets that pool (none at address 0).
[[nodiscard]] constexpr bool WritesPool(u64 pool, u32 limit, u64 address, u64 size) noexcept {
    return pool != 0 && address < PoolEnd(pool, limit) && pool < address + size;
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

    /// Drops the ranges meeting [begin, end) (their work cancelled).
    void Forget(u64 begin, u64 end) {
        std::erase_if(ranges, [begin, end](const Range& range) {
            return begin < range.end && range.begin < end;
        });
    }

    /// Drops the ranges `done(begin, end)` says are done.
    template <typename Done>
    void DropIf(Done&& done) {
        std::erase_if(ranges, [&done](const Range& range) { return done(range.begin, range.end); });
        prune_at = (std::max)(MinPrune, ranges.size() * 2);
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

/// B: whether a draw packet binds every vertex stream again (ForceVertexBinds) instead of only
/// those B's replica has dirty. `reset`: B's flags were cleared by binds C's geometry mirror did
/// not see (the stages began, a join's old setup bound straight to C's command buffer, a light
/// attempt fell back); `tick` / `forced_tick`: C's command buffer now and at the last forced
/// packet. Once per command buffer B marks the streams' use (Buffer::MarkUsage) as the old path
/// does after each submission, which keeps uploads from being reordered ahead of draws C binds
/// from the mirror (CanReorderUpload); packets B made before it saw the new command buffer are
/// not covered.
[[nodiscard]] constexpr bool GeometryForced(bool reset, u64 tick, u64 forced_tick) noexcept {
    return reset || tick != forced_tick;
}

/// C: the geometry mirror (§3.6/6; R2 kayit-4, T3 R2 geometri-2 / geometri-3). The vertex streams
/// B's packets bound, kept while B keeps them live (DrawToC::vertex_live), and the index buffer of
/// the packet in hand. Right before the draw C binds again what its command buffer may have lost
/// since the packet's records were replayed: a submission, or InvalidateState (a blit's pipeline
/// leaves the dynamic strides undefined). Only live streams bind again, never a stream B stopped
/// using (its buffer may be gone), and only the packet's own index buffer (every indexed or quad
/// draw binds it in its packet).
class GeometryMirror {
public:
    static constexpr u32 VertexSlots = 32;
    static constexpr u8 IndexSlot = 32;

    /// After the packet's records were replayed at `tick`, or in a join before (`early`: B's
    /// record owner hook, maybe in an older command buffer). Returns the vertex streams the packet
    /// bound: clean where it bound them in this command buffer, to bind again where `early`.
    u32 Apply(std::span<const GeometrySlot> slots, u32 live, u64 tick, bool early) {
        u32 bound = 0;
        has_index = false;
        for (const GeometrySlot& slot : slots) {
            if (slot.slot < VertexSlots) {
                vertex[slot.slot] = slot;
                bound |= 1u << slot.slot;
            } else if (slot.slot == IndexSlot) {
                index = slot;
                has_index = true;
            }
        }
        known = (known | bound) & live;
        index_tick = early ? NoTick : tick;
        return bound;
    }

    /// Before the draw: the live streams to bind again (`dirty`: the command buffer may not hold
    /// them).
    [[nodiscard]] u32 Rebind(u32 live, u32 dirty) const noexcept {
        return live & dirty & known;
    }
    /// Live dirty streams the mirror does not know (none expected: every bind B makes is in a
    /// packet, and B sends every stream again after binds outside one).
    [[nodiscard]] u32 Missing(u32 live, u32 dirty) const noexcept {
        return live & dirty & ~known;
    }
    /// Whether the packet's index buffer binds again: replayed in another command buffer.
    [[nodiscard]] bool RebindIndex(u64 tick) const noexcept {
        return has_index && index_tick != tick;
    }

    [[nodiscard]] const GeometrySlot& Vertex(u32 slot) const noexcept {
        return vertex[slot];
    }
    [[nodiscard]] const GeometrySlot& Index() const noexcept {
        return index;
    }
    [[nodiscard]] u32 Known() const noexcept {
        return known;
    }

    void Clear() noexcept {
        known = 0;
        has_index = false;
        index_tick = NoTick;
    }

private:
    static constexpr u64 NoTick = ~u64{0};

    std::array<GeometrySlot, VertexSlots> vertex{};
    GeometrySlot index{};
    u64 index_tick = NoTick;
    u32 known = 0;
    bool has_index = false;
};

} // namespace VideoCommon::Nx2Stages
