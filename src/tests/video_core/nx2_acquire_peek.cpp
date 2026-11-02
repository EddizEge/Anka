// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <deque>
#include <map>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_acquire_peek.h"

using namespace VideoCommon::Nx2AcquirePeek;

namespace {

constexpr u64 Word = 0x1000;
constexpr s32 Channel = 2;
constexpr s32 OtherChannel = 4;

// A word read MinSightings times with `value`: past the New verdict.
void Settle(Tracker& tracker, u64 address, u32 value) {
    for (u32 i = 0; i < Tracker::MinSightings; ++i) {
        tracker.Observe(address, value);
    }
}

} // Anonymous namespace

TEST_CASE("Nx2AcquirePeek: the switch", "[video_core]") {
    REQUIRE(ParseMode(nullptr) == Mode::Off);
    REQUIRE(ParseMode("") == Mode::Off);
    REQUIRE(ParseMode("0") == Mode::Off);
    REQUIRE(ParseMode("1") == Mode::Skip);
    REQUIRE(ParseMode("2") == Mode::Verify);
    REQUIRE(ParseMode("3") == Mode::Off);
}

TEST_CASE("Nx2AcquirePeek: operations as the puller evaluates them", "[video_core]") {
    REQUIRE(Matches(OpEqual, 5, 5));
    REQUIRE(!Matches(OpEqual, 6, 5));
    REQUIRE(!Matches(OpEqual, 4, 5));
    REQUIRE(Matches(OpGequal, 5, 5));
    REQUIRE(Matches(OpGequal, 6, 5));
    REQUIRE(!Matches(OpGequal, 4, 5));
    REQUIRE(Matches(OpMask, 6, 2));
    REQUIRE(!Matches(OpMask, 4, 2));
    REQUIRE(Matches(0x2, 0, 1)); // Others are not acquires: met
}

TEST_CASE("Nx2AcquirePeek: a counter met in memory skips", "[video_core]") {
    Tracker tracker;
    Settle(tracker, Word, 7);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 7, 7) == Verdict::Skip);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 5, 7) == Verdict::Skip);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 7, 7) == Verdict::Skip);
    // Unmet in memory: drains (and may wait) as before.
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 8, 7) == Verdict::Unmet);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 8, 7) == Verdict::Unmet);
    REQUIRE(tracker.Decide(Word, Channel, OpMask, 8, 7) == Verdict::Unmet);
}

TEST_CASE("Nx2AcquirePeek: a mask acquire always drains", "[video_core]") {
    Tracker tracker;
    Settle(tracker, Word, 6);
    REQUIRE(tracker.Decide(Word, Channel, OpMask, 2, 6) == Verdict::Operation);
}

TEST_CASE("Nx2AcquirePeek: greater or equal by 2^31 or more drains", "[video_core]") {
    Tracker tracker;
    Settle(tracker, Word, 0xffffffffU);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 0, 0xffffffffU) == Verdict::Wrapped);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 0x7fffffffU, 0xffffffffU) ==
            Verdict::Wrapped);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 0x80000000U, 0xffffffffU) == Verdict::Skip);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 0xffffffffU, 0xffffffffU) == Verdict::Skip);
}

TEST_CASE("Nx2AcquirePeek: a word read only a few times drains", "[video_core]") {
    Tracker tracker;
    for (u32 i = 1; i < Tracker::MinSightings; ++i) {
        tracker.Observe(Word, i);
        REQUIRE(tracker.Decide(Word, Channel, OpGequal, 1, i) == Verdict::New);
    }
    tracker.Observe(Word, Tracker::MinSightings);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 1, Tracker::MinSightings) == Verdict::Skip);
    // Another word knows nothing of this one's sightings.
    tracker.Observe(Word + 4, 9);
    REQUIRE(tracker.Decide(Word + 4, Channel, OpGequal, 1, 9) == Verdict::New);
}

TEST_CASE("Nx2AcquirePeek: a word seen going down never skips again", "[video_core]") {
    Tracker tracker;
    Settle(tracker, Word, 1);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 1, 1) == Verdict::Skip);
    // A flag cleared (or a counter reset) and set again.
    tracker.Observe(Word, 0);
    tracker.Observe(Word, 1);
    Settle(tracker, Word, 1);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 1, 1) == Verdict::NotMonotonic);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 1, 1) == Verdict::NotMonotonic);
    REQUIRE(tracker.WordsNotMonotonic() == 1);
    // The same value again is no step down.
    Settle(tracker, Word + 4, 3);
    REQUIRE(tracker.Decide(Word + 4, Channel, OpGequal, 3, 3) == Verdict::Skip);
    REQUIRE(tracker.WordsNotMonotonic() == 1);
}

TEST_CASE("Nx2AcquirePeek: queued releases of the same channel", "[video_core]") {
    Tracker tracker;
    Settle(tracker, Word, 7);
    // A queued release that would still meet the acquire: no matter.
    tracker.NoteRelease(Word, Channel, 9);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 7, 7) == Verdict::Skip);
    // One that would not (a reset, or another value for an equal acquire): drains.
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 7, 7) == Verdict::QueuedRelease);
    tracker.NoteRelease(Word, Channel, 0);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 7, 7) == Verdict::QueuedRelease);
    // The drain ran them.
    tracker.Drained();
    REQUIRE(tracker.QueuedReleases() == 0);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 7, 7) == Verdict::Skip);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 7, 7) == Verdict::Skip);
}

TEST_CASE("Nx2AcquirePeek: queued releases of other channels or words", "[video_core]") {
    Tracker tracker;
    Settle(tracker, Word, 7);
    // Not ordered before this channel's acquire on the console either.
    tracker.NoteRelease(Word, OtherChannel, 0);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 7, 7) == Verdict::Skip);
    REQUIRE(tracker.Decide(Word, OtherChannel, OpEqual, 7, 7) == Verdict::QueuedRelease);
    // Another word.
    tracker.NoteRelease(Word + 4, Channel, 0);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 7, 7) == Verdict::Skip);
}

TEST_CASE("Nx2AcquirePeek: a queued value the puller cannot tell drains", "[video_core]") {
    Tracker tracker;
    Settle(tracker, Word, 7);
    tracker.NoteRelease(Word, Channel, 0xffffffffU, false);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 7, 7) == Verdict::QueuedRelease);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 7, 7) == Verdict::QueuedRelease);
}

TEST_CASE("Nx2AcquirePeek: too many queued releases drain until a drain", "[video_core]") {
    Tracker tracker;
    Settle(tracker, Word, 7);
    for (size_t i = 0; i < Tracker::MaxReleases; ++i) {
        tracker.NoteRelease(Word + 8 + i * 4, OtherChannel, 1);
    }
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 7, 7) == Verdict::Skip);
    tracker.NoteRelease(Word + 4, OtherChannel, 1); // Not kept: any of them may be this word
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 7, 7) == Verdict::Untracked);
    tracker.Drained();
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 7, 7) == Verdict::Skip);
}

TEST_CASE("Nx2AcquirePeek: too many words: the others drain", "[video_core]") {
    Tracker tracker;
    for (u64 i = 0; i < Tracker::MaxWords; ++i) {
        tracker.Observe(i * 4, 1);
    }
    REQUIRE(tracker.Words() == Tracker::MaxWords);
    Settle(tracker, Tracker::MaxWords * 4, 1);
    REQUIRE(tracker.Words() == Tracker::MaxWords);
    REQUIRE(tracker.Decide(Tracker::MaxWords * 4, Channel, OpGequal, 1, 1) == Verdict::Untracked);
    Settle(tracker, 0, 1);
    REQUIRE(tracker.Decide(0, Channel, OpGequal, 1, 1) == Verdict::Skip);
}

// A front and a back as the split runs them: releases queued in order (the puller's noted, the
// engines' not), the back running some of them now and then, acquires reading guest memory, and
// sometimes a drain. A skip is right where the console could have met the acquire: with every
// release this channel queued before it written (theirs come first on its channel) and those of
// other channels in any state. Counters go up; the engines' writes come from a channel that does
// not acquire (a 3D queue ticking a counter others acquire); flags (0 and 1) are set by it too
// and cleared by the puller's releases on the acquiring channels.
TEST_CASE("Nx2AcquirePeek: model: a skip never runs ahead of its own channel", "[video_core]") {
    struct Write {
        u64 address;
        s32 channel;
        u32 value;
        bool noted;
    };
    constexpr u64 Counters = 3;
    constexpr u64 Flags = 2;
    constexpr s32 Producer = 3;
    std::mt19937 rng{12345};
    const auto pick = [&rng](u32 count) {
        return std::uniform_int_distribution<u32>{0, count - 1}(rng);
    };
    u64 skips = 0;
    u64 checks = 0;
    for (u32 round = 0; round < 200; ++round) {
        Tracker tracker;
        std::map<u64, u32> memory;
        std::map<u64, u32> issued; // Counters: the last value released
        std::deque<Write> queue;
        const auto run_back = [&](size_t count) {
            for (; count != 0 && !queue.empty(); --count) {
                memory[queue.front().address] = queue.front().value;
                queue.pop_front();
            }
        };
        for (u32 step = 0; step < 2000; ++step) {
            const s32 channel = static_cast<s32>(pick(3));
            const u32 action = pick(10);
            if (action < 3) {
                // A counter release: the puller's (noted) or an engine's of another channel.
                const u64 address = pick(static_cast<u32>(Counters)) * 4;
                issued[address] += 1 + pick(3);
                const bool noted = pick(2) == 0;
                const s32 writer = noted ? channel : Producer;
                queue.push_back({address, writer, issued[address], noted});
                if (noted) {
                    tracker.NoteRelease(address, writer, issued[address]);
                }
            } else if (action < 4) {
                // A flag: set by another channel's engine, cleared by the puller on this one.
                const u64 address = 0x100 + pick(static_cast<u32>(Flags)) * 4;
                if (pick(2) == 0) {
                    queue.push_back({address, Producer, 1, false});
                } else {
                    queue.push_back({address, channel, 0, true});
                    tracker.NoteRelease(address, channel, 0);
                }
            } else if (action < 6) {
                run_back(pick(4));
            } else {
                const bool flag = pick(4) == 0;
                const u64 address = flag ? 0x100 + pick(static_cast<u32>(Flags)) * 4
                                         : pick(static_cast<u32>(Counters)) * 4;
                const u32 operation = flag ? (pick(2) == 0 ? OpEqual : OpMask)
                                           : (pick(2) == 0 ? OpEqual : OpGequal);
                const u32 now = memory[address];
                const u32 value = flag ? 1 : (pick(3) == 0 ? now + 1 : now - pick(3));
                const u32 raw = memory[address];
                tracker.Observe(address, raw);
                const Verdict verdict = tracker.Decide(address, channel, operation, value, raw);
                // The console's view at the acquire: this channel's queued writes ran first.
                u32 console = raw;
                for (const Write& write : queue) {
                    if (write.address == address && write.channel == channel) {
                        console = write.value;
                    }
                }
                ++checks;
                if (verdict == Verdict::Skip) {
                    ++skips;
                    REQUIRE(Matches(operation, console, value));
                }
                if (verdict != Verdict::Skip || pick(4) == 0) {
                    run_back(queue.size());
                    tracker.Drained();
                    tracker.Observe(address, memory[address]);
                }
            }
        }
    }
    // The model must let a good share skip, or it checks nothing.
    REQUIRE(skips > checks / 10);
}

// What the tracker cannot see: an engine's write of the acquire's own channel taking the value
// back (a reset). The first such acquire may skip; once a drain lands the reset the word drains
// for good.
TEST_CASE("Nx2AcquirePeek: an unseen reset is caught after its first drain", "[video_core]") {
    Tracker tracker;
    Settle(tracker, Word, 5);
    // Queued, not noted: an engine's reset on this channel; memory still holds 5.
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 3, 5) == Verdict::Skip);
    // A later acquire drains (unmet in memory) and reads the reset after it.
    tracker.Observe(Word, 5);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 6, 5) == Verdict::Unmet);
    tracker.Drained();
    tracker.Observe(Word, 0);
    for (u32 value = 1; value <= 10; ++value) {
        tracker.Observe(Word, value);
        REQUIRE(tracker.Decide(Word, Channel, OpGequal, 1, value) == Verdict::NotMonotonic);
    }
}
