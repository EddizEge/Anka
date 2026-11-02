// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/nx2_draw_memo.h"

namespace {

using VideoCommon::SyncMemoSets;

/// The memory tracker as SynchronizeBuffer sees it: per 4 KiB page a dirty (CPU or untracked)
/// bit, per 4 MiB region the epoch of the last call that may have set one there.
struct TrackerModel {
    static constexpr u64 PAGES = 4096; // 16 MiB: four regions
    std::vector<bool> dirty = std::vector<bool>(PAGES, true);
    std::vector<u64> stamps = std::vector<u64>(PAGES >> 10, 1);
    u64 epoch = 1;

    void MarkDirty(u64 addr, u64 size) {
        ++epoch;
        for (u64 page = addr >> 12; page < (addr + size + 4095) >> 12; ++page) {
            dirty[page] = true;
            stamps[page >> 10] = epoch;
        }
    }
    /// ForEachUploadRange: whole pages; returns whether any was dirty.
    bool Upload(u64 addr, u64 size) {
        bool any = false;
        for (u64 page = addr >> 12; page < (addr + size + 4095) >> 12; ++page) {
            any |= dirty[page];
            dirty[page] = false;
        }
        return any;
    }
    bool UnsetSince(u64 addr, u64 size, u64 since) const {
        if (since == epoch || size == 0) {
            return true;
        }
        for (u64 region = addr >> 22; region <= (addr + size - 1) >> 22; ++region) {
            if (stamps[region] > since) {
                return false;
            }
        }
        return true;
    }
};

/// SynchronizeBuffer's EDEN_NX2_SYNC_SETS path. Returns whether the call was skipped; a skipped
/// call must have had nothing to upload.
bool Sync(SyncMemoSets& memo, TrackerModel& tracker, u64 addr, u64 size, bool& would_upload) {
    u32 first{};
    u32 end{};
    REQUIRE(SyncMemoSets::Pages(addr, size, first, end));
    SyncMemoSets::Set& set = memo.SetOf(addr);
    const u64 epoch = tracker.epoch;
    if (SyncMemoSets::Find(set, first, end, [&](u64 since) {
            return tracker.UnsetSince(addr, size, since);
        }) != nullptr) {
        would_upload = tracker.Upload(addr, size);
        return true;
    }
    would_upload = tracker.Upload(addr, size);
    SyncMemoSets::Store(set, first, end, epoch, [&](u64 begin, u64 bytes, u64 since) {
        return tracker.UnsetSince(begin, bytes, since);
    });
    return false;
}

} // namespace

TEST_CASE("SyncMemoSets pages", "[video_core][nx2]") {
    u32 first{};
    u32 end{};
    REQUIRE(SyncMemoSets::Pages(0x1234, 0x10, first, end));
    REQUIRE((first == 1 && end == 2));
    REQUIRE(SyncMemoSets::Pages(0x1FF0, 0x20, first, end));
    REQUIRE((first == 1 && end == 3));
    REQUIRE(SyncMemoSets::Pages(0x2000, 0x1000, first, end));
    REQUIRE((first == 2 && end == 3));
    REQUIRE(!SyncMemoSets::Pages(0x2000, 0, first, end));
    REQUIRE(!SyncMemoSets::Pages(u64{1} << 44, 0x10, first, end));
    REQUIRE(!SyncMemoSets::Pages((u64{1} << 44) - 0x1000, 0x1000, first, end));
    REQUIRE(SyncMemoSets::Pages((u64{1} << 44) - 0x2000, 0x1000, first, end));
    REQUIRE(end == 0xFFFF'FFFFU);
}

TEST_CASE("SyncMemoSets neighbours on a page", "[video_core][nx2]") {
    SyncMemoSets memo;
    TrackerModel tracker;
    bool uploaded{};
    REQUIRE(!Sync(memo, tracker, 0x10040, 0x40, uploaded));
    REQUIRE(uploaded);
    // Another range on the same page: clean since the first call cleared the whole page.
    REQUIRE(Sync(memo, tracker, 0x10800, 0x100, uploaded));
    REQUIRE(!uploaded);
    // The next page is not covered.
    REQUIRE(!Sync(memo, tracker, 0x11000, 0x10, uploaded));
    // Touching ranges joined: one range over both pages hits now.
    REQUIRE(Sync(memo, tracker, 0x10f00, 0x200, uploaded));
    REQUIRE(!uploaded);
    // A CPU write in the region makes it miss and upload.
    tracker.MarkDirty(0x10900, 4);
    REQUIRE(!Sync(memo, tracker, 0x10800, 0x100, uploaded));
    REQUIRE(uploaded);
    // A write in another region leaves it.
    tracker.MarkDirty(0x800000, 4);
    REQUIRE(Sync(memo, tracker, 0x10800, 0x100, uploaded));
    REQUIRE(!uploaded);
}

TEST_CASE("SyncMemoSets ways", "[video_core][nx2]") {
    SyncMemoSets::Set set{};
    const auto holds = [](u64, u64, u64) { return true; };
    const auto never = [](u64, u64, u64) { return false; };
    SyncMemoSets::Store(set, 10, 11, 5, holds);
    SyncMemoSets::Store(set, 20, 21, 6, holds);
    SyncMemoSets::Store(set, 30, 31, 7, holds);
    SyncMemoSets::Store(set, 40, 41, 8, holds);
    // Full: the oldest (epoch 5) goes.
    SyncMemoSets::Store(set, 50, 51, 9, holds);
    const auto found = [&](u32 first, u32 end) {
        return SyncMemoSets::Find(set, first, end, [](u64) { return true; }) != nullptr;
    };
    REQUIRE(!found(10, 11));
    REQUIRE(found(20, 21));
    REQUIRE(found(50, 51));
    // Touching and holding: joined.
    SyncMemoSets::Store(set, 21, 23, 10, holds);
    REQUIRE(found(20, 23));
    // Overlapping but no longer holding: replaced, not joined.
    SyncMemoSets::Store(set, 22, 24, 11, never);
    REQUIRE(found(22, 24));
    REQUIRE(!found(20, 21));
    REQUIRE(SyncMemoSets::MissKind(set, 20, 21) == 3);
    REQUIRE(SyncMemoSets::MissKind(set, 21, 22) == 2);
    REQUIRE(SyncMemoSets::MissKind(set, 60, 61) == 3);
    REQUIRE(SyncMemoSets::MissKind(set, 22, 23) == 1);
    const SyncMemoSets::Set empty{};
    REQUIRE(SyncMemoSets::MissKind(empty, 22, 23) == 0);
    // A way that covers but no longer holds is not found.
    REQUIRE(SyncMemoSets::Find(set, 22, 23, [](u64) { return false; }) == nullptr);
}

TEST_CASE("SyncMemoSets random against the tracker model", "[video_core][nx2]") {
    std::mt19937_64 rng{12345};
    for (int round = 0; round < 40; ++round) {
        SyncMemoSets memo;
        TrackerModel tracker;
        u64 hits = 0;
        // Addresses within 16 MiB, clustered around a few objects so that sets and pages are
        // shared.
        std::vector<u64> objects(48);
        for (u64& object : objects) {
            object = rng() % (TrackerModel::PAGES * 4096 - 0x80000);
        }
        for (int step = 0; step < 4000; ++step) {
            const u64 addr = (objects[rng() % objects.size()] + rng() % 0x6000) & ~u64{0xF};
            const u64 size = 1 + rng() % ((rng() & 7) == 0 ? 0x30000 : 0x300);
            if (rng() % 8 == 0) {
                tracker.MarkDirty(addr, size);
                continue;
            }
            bool uploaded{};
            if (Sync(memo, tracker, addr, size, uploaded)) {
                ++hits;
                REQUIRE(!uploaded);
            }
        }
        REQUIRE(hits > 0);
    }
}
