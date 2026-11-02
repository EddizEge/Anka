// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Back stages, cut c: what B's draw path keeps from draw to draw to do less of it (the K2 speed
// round after run 85). Shim-built like nx2_stage_split.cpp (not in CMakeLists: YUZU_TESTS=OFF).

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/nx2_profile.h"

using VideoCommon::UsageMarkMemo;
using VideoCommon::UsageTracker;

namespace {

/// A small deterministic generator (the tests must not vary from run to run).
struct Lcg {
    u64 state;
    u32 Next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<u32>(state >> 33);
    }
};

/// Whether two trackers answer IsUsed alike over the probes (64 B granules, 1 MiB).
bool SameUse(const UsageTracker& a, const UsageTracker& b) {
    for (u64 offset = 0; offset < (u64{1} << 20); offset += 4096) {
        for (const u64 size : {u64{64}, u64{4096}, u64{65536}}) {
            if (a.IsUsed(offset, size) != b.IsUsed(offset, size)) {
                return false;
            }
        }
    }
    for (u64 offset = 0; offset < 8192; offset += 64) {
        if (a.IsUsed(offset, 64) != b.IsUsed(offset, 64)) {
            return false;
        }
    }
    return true;
}

} // Anonymous namespace

TEST_CASE("Nx2StageCosts: a usage tracker's generation is new at each reset", "[video_core]") {
    UsageTracker a{1 << 20};
    UsageTracker b{1 << 20};
    // Unique over trackers: a buffer made in a reused slot never matches the old one's memo.
    REQUIRE(a.Generation() != b.Generation());
    REQUIRE(a.Generation() != 0);
    const u64 first = a.Generation();
    a.Track(0, 4096);
    a.Track(8192, 100);
    REQUIRE(a.Generation() == first);
    a.Reset();
    REQUIRE(a.Generation() != first);
    REQUIRE(a.Generation() != b.Generation());
    REQUIRE(!a.IsUsed(0, 4096));
}

TEST_CASE("Nx2StageCosts: the stream mark memo holds the same range in one generation only",
          "[video_core]") {
    UsageMarkMemo memo;
    // The default holds nothing a tracker has (generations start at 1).
    REQUIRE(!memo.Holds(0, UsageTracker{4096}.Generation(), 0, 0));
    REQUIRE(!memo.Holds(~u32{0}, UsageTracker{4096}.Generation(), 0, 0));
    memo.Note(3, 17, 4096, 256);
    REQUIRE(memo.Holds(3, 17, 4096, 256));
    REQUIRE(!memo.Holds(4, 17, 4096, 256));  // another buffer slot
    REQUIRE(!memo.Holds(3, 18, 4096, 256));  // its tracker reset since
    REQUIRE(!memo.Holds(3, 17, 4160, 256));  // another offset
    REQUIRE(!memo.Holds(3, 17, 4096, 320));  // another size
}

TEST_CASE("Nx2StageCosts: skipping held marks leaves the tracker as tracking each",
          "[video_core]") {
    // MarkVertexStreamsUsed's rule against tracking every stream every packet: the same use
    // answers (IsUsed: CanReorderUpload) after any sequence of marks and resets. Several
    // streams share a buffer, ranges repeat and move, TickFrame resets between packets.
    for (u64 seed = 1; seed <= 6; ++seed) {
        Lcg rng{seed};
        UsageTracker every{1 << 20};
        UsageTracker memoed{1 << 20};
        std::array<UsageMarkMemo, 12> memos{};
        std::array<std::pair<u64, u64>, 12> ranges{};
        for (u32 stream = 0; stream < ranges.size(); ++stream) {
            ranges[stream] = {u64{stream} * 40000 + 64 * (rng.Next() % 8), 1000 + rng.Next() % 60000};
        }
        u32 skipped = 0;
        for (u32 packet = 0; packet < 400; ++packet) {
            const u32 roll = rng.Next() % 100;
            if (roll < 4) {
                // TickFrame: the buffer was free, its marks reset.
                every.Reset();
                memoed.Reset();
            } else if (roll < 30) {
                // A stream moved (an index buffer's first, a mesh switch).
                const u32 stream = rng.Next() % ranges.size();
                ranges[stream] = {64 * (rng.Next() % 16000), 64 + rng.Next() % 100000};
            }
            for (u32 stream = 0; stream < ranges.size(); ++stream) {
                const auto [offset, size] = ranges[stream];
                every.Track(offset, size);
                if (memos[stream].Holds(0, memoed.Generation(), offset, size)) {
                    ++skipped;
                    continue;
                }
                memoed.Track(offset, size);
                memos[stream].Note(0, memoed.Generation(), offset, size);
            }
            if (packet % 16 == 0 || roll < 4) {
                REQUIRE(SameUse(every, memoed));
            }
        }
        REQUIRE(SameUse(every, memoed));
        // Most marks repeat (the point of the memo).
        REQUIRE(skipped > 400 * 12 / 2);
    }
}

TEST_CASE("Nx2StageCosts: the fine zones are off with the profile off", "[video_core]") {
    // perf/tam: EDEN_NX2_GPU_PROFILE is read at the first Enabled() (g_enabled is -1 before);
    // EDEN_NX2_PROFILE_FINE follows it, so B's draw path times nothing without the profile.
    namespace Profile = VideoCommon::Nx2Profile;
    const char* profile = std::getenv("EDEN_NX2_GPU_PROFILE");
    const bool profiling = profile != nullptr && profile[0] != '0';
    const char* fine = std::getenv("EDEN_NX2_PROFILE_FINE");
    const bool fine_on = fine == nullptr || fine[0] != '0';
    REQUIRE(Profile::FineEnabled() == (profiling && fine_on));
    REQUIRE(Profile::Enabled() == profiling);
    REQUIRE(Profile::FineEnabled() == (profiling && fine_on));
}

TEST_CASE("Nx2StageCosts: a reset USAGE_RESET_SKIP skips keeps no held mark", "[video_core]") {
    // perf/tam: b-hiz's EDEN_NX2_USAGE_RESET_SKIP (default on) leaves a tracker nothing was
    // tracked in since its last reset, generation and all. A memo notes a generation only after
    // its Track, so the reset after it clears and starts a new one: no memo holds what a skipped
    // reset kept. With the switch at 0 every reset starts a new generation (the plain rule).
    const char* env = std::getenv("EDEN_NX2_USAGE_RESET_SKIP");
    const bool skip = env == nullptr || env[0] != '0';
    UsageTracker tracker{1 << 16};
    UsageMarkMemo memo;
    tracker.Track(0, 4096);
    memo.Note(2, tracker.Generation(), 0, 4096);
    const u64 noted = tracker.Generation();
    tracker.Reset();
    const u64 cleared = tracker.Generation();
    REQUIRE(cleared != noted);
    REQUIRE(!memo.Holds(2, tracker.Generation(), 0, 4096));
    REQUIRE(!tracker.IsUsed(0, 4096));
    tracker.Reset(); // Nothing tracked since
    REQUIRE((tracker.Generation() == cleared) == skip);
    REQUIRE(!memo.Holds(2, tracker.Generation(), 0, 4096));
    tracker.Track(0, 4096);
    REQUIRE(!memo.Holds(2, tracker.Generation(), 0, 4096));
    memo.Note(2, tracker.Generation(), 0, 4096);
    tracker.Reset();
    REQUIRE(!memo.Holds(2, tracker.Generation(), 0, 4096));
}

TEST_CASE("Nx2StageCosts: held marks across skipped resets leave the tracker as tracking each",
          "[video_core]") {
    // As "skipping held marks" with b-hiz's reset skip in play: packets that mark only some
    // streams or none, and resets back to back (a free buffer TickFrame resets again with
    // nothing tracked since), so many of the resets are skipped.
    for (u64 seed = 1; seed <= 6; ++seed) {
        Lcg rng{seed * 7919};
        UsageTracker every{1 << 20};
        UsageTracker memoed{1 << 20};
        std::array<UsageMarkMemo, 12> memos{};
        std::array<std::pair<u64, u64>, 12> ranges{};
        for (u32 stream = 0; stream < ranges.size(); ++stream) {
            ranges[stream] = {u64{stream} * 40000 + 64 * (rng.Next() % 8), 1000 + rng.Next() % 60000};
        }
        u32 skipped = 0;
        u32 resets = 0;
        for (u32 packet = 0; packet < 600; ++packet) {
            const u32 roll = rng.Next() % 100;
            if (roll < 15) {
                every.Reset();
                memoed.Reset();
                ++resets;
                REQUIRE(SameUse(every, memoed));
                continue;
            }
            if (roll < 30) {
                const u32 stream = rng.Next() % ranges.size();
                ranges[stream] = {64 * (rng.Next() % 16000), 64 + rng.Next() % 100000};
            }
            // Which streams this packet marks: all, some, or none.
            const u32 marks = roll < 50 ? 0 : roll < 75 ? rng.Next() : ~u32{0};
            for (u32 stream = 0; stream < ranges.size(); ++stream) {
                if (((marks >> stream) & 1) == 0) {
                    continue;
                }
                const auto [offset, size] = ranges[stream];
                every.Track(offset, size);
                if (memos[stream].Holds(0, memoed.Generation(), offset, size)) {
                    ++skipped;
                    continue;
                }
                memoed.Track(offset, size);
                memos[stream].Note(0, memoed.Generation(), offset, size);
            }
            if (packet % 8 == 0) {
                REQUIRE(SameUse(every, memoed));
            }
        }
        REQUIRE(SameUse(every, memoed));
        REQUIRE(resets > 50);
        REQUIRE(skipped > 0);
    }
}

TEST_CASE("Nx2StageCosts: a mark in a reused buffer slot tracks again", "[video_core]") {
    // A buffer deleted and another made in its slot (SlotVector reuses ids): the memo's slot and
    // range match, the new tracker's generation does not.
    UsageMarkMemo memo;
    UsageTracker old_buffer{1 << 16};
    old_buffer.Track(0, 4096);
    memo.Note(5, old_buffer.Generation(), 0, 4096);
    UsageTracker new_buffer{1 << 16};
    REQUIRE(!memo.Holds(5, new_buffer.Generation(), 0, 4096));
    REQUIRE(!new_buffer.IsUsed(0, 4096));
}
