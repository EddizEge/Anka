// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
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

// The word read with `raw` and an acquire of `met` on `channel` met before.
void Prime(Tracker& tracker, u64 address, u32 raw, u32 met, s32 channel = Channel) {
    tracker.Observe(address, raw);
    tracker.Met(address, channel, met);
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

TEST_CASE("Nx2AcquirePeek: a wait on a counter met in memory skips", "[video_core]") {
    Tracker tracker;
    Prime(tracker, Word, 7, 4);
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
    Prime(tracker, Word, 6, 1);
    REQUIRE(tracker.Decide(Word, Channel, OpMask, 2, 6) == Verdict::Operation);
}

TEST_CASE("Nx2AcquirePeek: greater or equal by 2^31 or more drains", "[video_core]") {
    Tracker tracker;
    Prime(tracker, Word, 0xffffffffU, 0xfffffff0U);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 0, 0xffffffffU) == Verdict::Wrapped);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 0x7fffffffU, 0xffffffffU) ==
            Verdict::Wrapped);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 0xfffffff1U, 0xffffffffU) == Verdict::Skip);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 0xffffffffU, 0xffffffffU) == Verdict::Skip);
}

TEST_CASE("Nx2AcquirePeek: a channel's first wait on a word drains", "[video_core]") {
    Tracker tracker;
    tracker.Observe(Word, 9);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 1, 9) == Verdict::New);
    // Reads alone (polls, other acquires' drains) do not make it known.
    for (u32 i = 0; i < 16; ++i) {
        tracker.Observe(Word, 9);
    }
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 1, 9) == Verdict::New);
    tracker.Met(Word, Channel, 1);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 2, 9) == Verdict::Skip);
    // Another channel's met waits are its own.
    REQUIRE(tracker.Decide(Word, OtherChannel, OpGequal, 2, 9) == Verdict::New);
}

TEST_CASE("Nx2AcquirePeek: a wait for no more than the last met one drains", "[video_core]") {
    Tracker tracker;
    Prime(tracker, Word, 1, 1);
    // A flag: the same value every round. Memory may hold last round's 1 while this channel's
    // own reset is queued.
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 1, 1) == Verdict::NotAdvanced);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 1, 1) == Verdict::NotAdvanced);
    // A counter reset between rounds waits for lower values again.
    Prime(tracker, Word + 4, 40, 40);
    REQUIRE(tracker.Decide(Word + 4, Channel, OpGequal, 1, 40) == Verdict::NotAdvanced);
    REQUIRE(tracker.Decide(Word + 4, Channel, OpEqual, 39, 39) == Verdict::NotAdvanced);
    REQUIRE(tracker.Decide(Word + 4, Channel, OpGequal, 41, 41) == Verdict::Skip);
    // Half the range or more ahead is no step forward either.
    REQUIRE(tracker.Decide(Word + 4, Channel, OpEqual, 40 + 0x80000000U, 40 + 0x80000000U) ==
            Verdict::NotAdvanced);
    REQUIRE(tracker.Decide(Word + 4, Channel, OpEqual, 40 + 0x7fffffffU, 40 + 0x7fffffffU) ==
            Verdict::Skip);
    // Met again: the next skip must wait for more than that.
    tracker.Met(Word + 4, Channel, 45);
    REQUIRE(tracker.Decide(Word + 4, Channel, OpGequal, 45, 50) == Verdict::NotAdvanced);
    REQUIRE(tracker.Decide(Word + 4, Channel, OpGequal, 46, 50) == Verdict::Skip);
}

TEST_CASE("Nx2AcquirePeek: a word keeps the last met wait of a few channels", "[video_core]") {
    Tracker tracker;
    tracker.Observe(Word, 100);
    for (s32 channel = 0; channel < static_cast<s32>(Tracker::MaxChannels) + 2; ++channel) {
        tracker.Met(Word, channel, 10);
    }
    for (s32 channel = 0; channel < static_cast<s32>(Tracker::MaxChannels); ++channel) {
        REQUIRE(tracker.Decide(Word, channel, OpGequal, 11, 100) == Verdict::Skip);
    }
    REQUIRE(tracker.Decide(Word, static_cast<s32>(Tracker::MaxChannels), OpGequal, 11, 100) ==
            Verdict::New);
    // A word never read is not kept.
    tracker.Met(Word + 4, Channel, 1);
    REQUIRE(tracker.Words() == 1);
}

TEST_CASE("Nx2AcquirePeek: a word seen going down never skips again", "[video_core]") {
    Tracker tracker;
    Prime(tracker, Word, 5, 4);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 5, 5) == Verdict::Skip);
    tracker.Observe(Word, 0);
    tracker.Observe(Word, 6);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 6, 6) == Verdict::NotMonotonic);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 6, 6) == Verdict::NotMonotonic);
    REQUIRE(tracker.WordsNotMonotonic() == 1);
    // The same value again is no step down.
    Prime(tracker, Word + 4, 3, 2);
    tracker.Observe(Word + 4, 3);
    REQUIRE(tracker.Decide(Word + 4, Channel, OpGequal, 3, 3) == Verdict::Skip);
    REQUIRE(tracker.WordsNotMonotonic() == 1);
}

TEST_CASE("Nx2AcquirePeek: queued releases of the same channel", "[video_core]") {
    Tracker tracker;
    Prime(tracker, Word, 7, 6);
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
    Prime(tracker, Word, 7, 6);
    tracker.Met(Word, OtherChannel, 6);
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
    Prime(tracker, Word, 7, 6);
    tracker.NoteRelease(Word, Channel, 0xffffffffU, false);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 7, 7) == Verdict::QueuedRelease);
    REQUIRE(tracker.Decide(Word, Channel, OpEqual, 7, 7) == Verdict::QueuedRelease);
}

TEST_CASE("Nx2AcquirePeek: too many queued releases drain until a drain", "[video_core]") {
    Tracker tracker;
    Prime(tracker, Word, 7, 6);
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
    Prime(tracker, Tracker::MaxWords * 4, 2, 1);
    REQUIRE(tracker.Words() == Tracker::MaxWords);
    REQUIRE(tracker.Decide(Tracker::MaxWords * 4, Channel, OpGequal, 2, 2) == Verdict::Untracked);
    Prime(tracker, 0, 2, 1);
    REQUIRE(tracker.Decide(0, Channel, OpGequal, 2, 2) == Verdict::Skip);
}

// A front and a back as the split runs them. Releases are queued in order: the puller's are noted,
// the engines' are not (3D report semaphores, compute and DMA releases). The back runs some of them
// now and then; acquires read guest memory, rechecks of a suspended one poll it, and a drain runs
// everything. A skip is right where the console could have met the acquire: with every write this
// channel queued before it done (theirs come first on its channel) and those of other channels in
// any state. Kinds of words:
// - counters only go up, from a producer channel that does not acquire (a 3D queue ticking a counter
//   others acquire value by value) and from the puller of acquiring channels; acquiring channels
//   wait for growing values, as each wait follows the last;
// - flags (0 and 1): set by the producer's engine, cleared by an acquiring channel's own engine (not
//   noted) or its puller (noted); each round waits for 1 again;
// - counters a channel resets each round with its own engine (not noted), the producer counting
//   again from 0, the channel waiting for 1, 2, ... again;
// - counters a channel resets with its puller (noted) and then waits for more than before;
// - the same reset by the channel's engine (not noted): the tracker cannot tell before it saw the
//   word go down once. Those skips are counted as the known limit, not checked.
TEST_CASE("Nx2AcquirePeek: model: a skip never runs ahead of its own channel", "[video_core]") {
    struct Write {
        u64 address;
        s32 channel;
        u32 value;
    };
    enum Kind : u32 { Counter, Flag, Restart, NotedReset, EngineReset, Kinds };
    constexpr std::array<u32, Kinds> words_of{3, 2, 2, 2, 2};
    constexpr s32 Channels = 3;
    constexpr s32 Producer = 3;
    const auto word_of = [](u32 kind, u32 index) { return u64{kind} * 0x100 + index * 4; };
    std::mt19937 rng{12345};
    const auto pick = [&rng](u32 count) {
        return std::uniform_int_distribution<u32>{0, count - 1}(rng);
    };
    std::array<u64, Kinds> skips{};
    u64 checks = 0;
    u64 known_limit = 0;
    u64 queued_release_decided = 0;
    u64 not_monotonic_decided = 0;
    for (u32 round = 0; round < 300; ++round) {
        Tracker tracker;
        std::map<u64, u32> memory;
        std::map<u64, u32> issued; // Counting words: the last value released
        // What each channel's program waits for next on a word (its own count, not the tracker's).
        std::map<std::pair<u64, s32>, u32> next_wait;
        std::map<u64, u32> seen;
        std::map<u64, bool> dipped; // The tracker read the word going down
        std::deque<Write> queue;
        const auto run_back = [&](size_t count) {
            for (; count != 0 && !queue.empty(); --count) {
                memory[queue.front().address] = queue.front().value;
                queue.pop_front();
            }
        };
        const auto observe = [&](u64 address) {
            const u32 value = memory[address];
            if (const auto it = seen.find(address); it != seen.end() && value < it->second) {
                dipped[address] = true;
            }
            seen[address] = value;
            tracker.Observe(address, value);
        };
        for (u32 step = 0; step < 3000; ++step) {
            const s32 channel = static_cast<s32>(pick(Channels));
            const u32 action = pick(20);
            if (action < 10) {
                const u32 kind = action / 2;
                const u64 address = word_of(kind, pick(words_of[kind]));
                if (kind == Counter) {
                    // The puller's release (noted) on an acquiring channel, or the producer's.
                    issued[address] += 1 + pick(3);
                    const bool noted = pick(2) == 0;
                    queue.push_back({address, noted ? channel : Producer, issued[address]});
                    if (noted) {
                        tracker.NoteRelease(address, channel, issued[address]);
                    }
                } else if (kind == Flag) {
                    const u32 how = pick(3);
                    queue.push_back({address, how == 0 ? Producer : channel, how == 0 ? 1U : 0U});
                    if (how == 1) {
                        tracker.NoteRelease(address, channel, 0);
                    }
                } else if (pick(4) != 0) {
                    issued[address] += 1 + pick(2);
                    queue.push_back({address, Producer, issued[address]});
                } else {
                    // A channel resets the word: with its engine (restarting its waits, or not)
                    // or with its puller.
                    issued[address] = 0;
                    queue.push_back({address, channel, 0});
                    if (kind == NotedReset) {
                        tracker.NoteRelease(address, channel, 0);
                    } else if (kind == Restart) {
                        next_wait[{address, channel}] = 1;
                    }
                }
            } else if (action < 13) {
                run_back(pick(4));
            } else {
                const u32 kind = pick(Kinds);
                const u64 address = word_of(kind, pick(words_of[kind]));
                u32& wait = next_wait[{address, channel}];
                wait = wait == 0 ? 1 : wait;
                const u32 raw = memory[address];
                u32 operation = OpGequal;
                u32 value = wait;
                if (kind == Counter) {
                    // Equal waits for the value there is, or the next one; on a value this
                    // channel's own queue already passes the console never meets it (no program
                    // waits so), so those come only as greater or equal.
                    bool own_ahead = false;
                    for (const Write& write : queue) {
                        own_ahead |= write.address == address && write.channel == channel;
                    }
                    operation = !own_ahead && pick(2) == 0 ? OpEqual : OpGequal;
                    value = operation == OpEqual ? (raw > wait ? raw : wait) : wait + pick(3);
                } else if (kind == Flag) {
                    operation = pick(3) == 0 ? OpMask : (pick(2) == 0 ? OpEqual : OpGequal);
                    value = 1;
                } else if (kind != Restart) {
                    value = wait + pick(2);
                }
                observe(address);
                const Verdict verdict = tracker.Decide(address, channel, operation, value, raw);
                // The console's view at the acquire: this channel's queued writes ran first.
                u32 console = raw;
                for (const Write& write : queue) {
                    if (write.address == address && write.channel == channel) {
                        console = write.value;
                    }
                }
                ++checks;
                queued_release_decided += verdict == Verdict::QueuedRelease ? 1 : 0;
                not_monotonic_decided +=
                    verdict == Verdict::NotMonotonic && kind == EngineReset ? 1 : 0;
                bool met = false;
                if (verdict == Verdict::Skip) {
                    ++skips[kind];
                    if (kind == EngineReset && !dipped[address] &&
                        !Matches(operation, console, value)) {
                        ++known_limit;
                    } else {
                        REQUIRE(Matches(operation, console, value));
                    }
                    met = true;
                } else {
                    // The drain, and the polls of a suspended acquire after it.
                    run_back(queue.size());
                    tracker.Drained();
                    observe(address);
                    met = Matches(operation, memory[address], value);
                    for (u32 poll = pick(3); poll != 0; --poll) {
                        observe(address);
                    }
                }
                if (met) {
                    tracker.Met(address, channel, value);
                    if (kind != Flag) {
                        wait = value + 1;
                    }
                }
            }
        }
    }
    // The model must let a good share of the counter waits skip, or it checks nothing; a flag's
    // wait for 1 again never does. The queued release and going-down rules must decide some.
    REQUIRE(skips[Counter] > checks / 40);
    REQUIRE(skips[Flag] == 0);
    REQUIRE(queued_release_decided > 0);
    REQUIRE(not_monotonic_decided > 0);
    // The known limit (an engine's reset on the acquire's own channel the tracker has not seen
    // go down yet, the channel then waiting for more than before) shows up in this model. It
    // stays the first round of such a word: after a dip it is checked above.
    REQUIRE(known_limit > 0);
    REQUIRE(known_limit < skips[EngineReset]);
}

// What the tracker cannot see: an engine's write of the acquire's own channel taking a counter
// back while the channel waits for more than before (a program that resets a word and goes on
// counting from where it was). The skip runs ahead; a drain that lands the reset marks the word.
TEST_CASE("Nx2AcquirePeek: an unseen reset is caught after its first drain", "[video_core]") {
    Tracker tracker;
    Prime(tracker, Word, 5, 3);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 4, 5) == Verdict::Skip);
    tracker.Met(Word, Channel, 4);
    // A later acquire drains (unmet in memory) and reads the reset after it.
    tracker.Observe(Word, 5);
    REQUIRE(tracker.Decide(Word, Channel, OpGequal, 6, 5) == Verdict::Unmet);
    tracker.Drained();
    tracker.Observe(Word, 0);
    for (u32 value = 7; value <= 16; ++value) {
        tracker.Observe(Word, value);
        REQUIRE(tracker.Decide(Word, Channel, OpGequal, value, value) == Verdict::NotMonotonic);
    }
}
