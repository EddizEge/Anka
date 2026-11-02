// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <unordered_map>
#include <vector>

#include "common/common_types.h"

// EDEN_NX2_ACQUIRE_PEEK (puller.cpp): a Switch 2 semaphore acquire whose value guest memory
// already holds goes on without draining the split GPU thread's back. What the puller decides
// with, kept apart from it so it can be tested.
//
// Why a met word needs no drain: the back (and its stages) runs everything the front sent in
// order, so the work after the acquire runs after all the work sent before it either way. The
// drain only adds the writes still queued (or held by the caches) to what the acquire reads.
// The word in memory was written by a release the back already ran, and the work before that
// release ran before it; queued writes of other channels are not ordered before this acquire on
// the console either. What would change the answer is a write of this channel's own, sent before
// the acquire and not run yet, that takes a met value back: a counter that only goes up never
// does. So only the words that behaved like such counters skip (Decide): no mask acquires (flag
// bits are cleared again), no word seen going down or seen only a few times yet (a drained
// acquire reads it twice, so a reset that lands in its drain shows), no queued release of the
// puller's on this channel that would not meet the acquire.
namespace VideoCommon::Nx2AcquirePeek {

/// Puller GpuSemaphoreOperation values (Nx2SemaphoreWaiters' too).
inline constexpr u32 OpEqual = 0x1;
inline constexpr u32 OpGequal = 0x4;
inline constexpr u32 OpMask = 0x8;

enum class Mode : u8 {
    Off,    ///< =0 or unset: every acquire drains (as before)
    Skip,   ///< =1: an acquire Decide lets skip goes on without a drain
    Verify, ///< =2: every acquire drains; where =1 would skip, the word after it is compared
};

[[nodiscard]] constexpr Mode ParseMode(const char* value) noexcept {
    if (value == nullptr) {
        return Mode::Off;
    }
    switch (value[0]) {
    case '1':
        return Mode::Skip;
    case '2':
        return Mode::Verify;
    default:
        return Mode::Off;
    }
}

[[nodiscard]] constexpr bool Matches(u32 operation, u32 current, u32 value) noexcept {
    switch (operation) {
    case OpEqual:
        return current == value;
    case OpGequal:
        return current >= value;
    case OpMask:
        return (current & value) != 0;
    default:
        return true;
    }
}

/// What Decide says about an acquire: it may skip the drain, or why not.
enum class Verdict : u8 {
    Skip,          ///< Met in memory and nothing below applies
    Unmet,         ///< Memory does not meet it
    Operation,     ///< A mask acquire (or another operation): bits a queued write may clear
    Wrapped,       ///< Greater-or-equal met by 2^31 or more (a wrapped counter or another value)
    QueuedRelease, ///< A release this channel's puller queued to the word would not meet it
    NotMonotonic,  ///< The word was seen going down (a flag, a counter reset or reused)
    New,           ///< The word was read fewer than MinSightings times
    Untracked,     ///< No device address, or too many words or releases to track
    Count,
};

/// Front thread state: the words acquires read (device addresses) and the releases the puller
/// queued for the back since its last drain.
class Tracker {
public:
    static constexpr size_t MaxWords = 4096;
    static constexpr size_t MaxReleases = 256;
    static constexpr u32 MinSightings = 4;

    /// A release the puller queued for the back: `payload` to the u32 at `address` on `channel`,
    /// or a value it cannot tell (`known` false: the timestamp of a 16-byte release).
    void NoteRelease(u64 address, s32 channel, u32 payload, bool known = true) {
        if (releases.size() >= MaxReleases) {
            releases_full = true;
            return;
        }
        releases.push_back({address, channel, payload, known});
    }

    /// The puller drained the back: every release noted ran, written or (behind a fence) in
    /// the query cache's pending releases, which a drain's read does not see either.
    void Drained() {
        releases.clear();
        releases_full = false;
    }

    /// Guest memory held `value` at `address` (reads in time order: before and after drains).
    void Observe(u64 address, u32 value) {
        const auto it = words.find(address);
        if (it == words.end()) {
            if (words.size() < MaxWords) {
                words.emplace(address, Word{value, 1, false});
            }
            return;
        }
        if (value < it->second.last) {
            it->second.decreased = true;
        }
        it->second.last = value;
        if (it->second.seen < MinSightings) {
            ++it->second.seen;
        }
    }

    /// Whether an acquire of `value` (`operation`) on `channel` may go on without a drain with
    /// `raw` in guest memory now. Observe the read first: a word going down now counts.
    [[nodiscard]] Verdict Decide(u64 address, s32 channel, u32 operation, u32 value,
                                 u32 raw) const {
        if (!Matches(operation, raw, value)) {
            return Verdict::Unmet;
        }
        if (operation != OpEqual && operation != OpGequal) {
            return Verdict::Operation;
        }
        if (operation == OpGequal && raw - value >= 0x80000000U) {
            return Verdict::Wrapped;
        }
        const auto it = words.find(address);
        if (it == words.end() || releases_full) {
            return Verdict::Untracked;
        }
        if (it->second.decreased) {
            return Verdict::NotMonotonic;
        }
        if (it->second.seen < MinSightings) {
            return Verdict::New;
        }
        for (const Release& release : releases) {
            if (release.address == address && release.channel == channel &&
                (!release.known || !Matches(operation, release.payload, value))) {
                return Verdict::QueuedRelease;
            }
        }
        return Verdict::Skip;
    }

    [[nodiscard]] size_t Words() const {
        return words.size();
    }
    [[nodiscard]] size_t WordsNotMonotonic() const {
        size_t count = 0;
        for (const auto& [address, word] : words) {
            count += word.decreased ? 1 : 0;
        }
        return count;
    }
    [[nodiscard]] size_t QueuedReleases() const {
        return releases.size();
    }

private:
    struct Word {
        u32 last;       ///< The value last seen
        u32 seen;       ///< Times read, up to MinSightings
        bool decreased; ///< Seen going down once: never skips again
    };
    struct Release {
        u64 address;
        s32 channel;
        u32 payload;
        bool known;
    };

    std::unordered_map<u64, Word> words;
    std::vector<Release> releases;
    bool releases_full{};
};

} // namespace VideoCommon::Nx2AcquirePeek
