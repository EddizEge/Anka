// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"

// EDEN_NX2_AB_TOGGLE=<N> (off by default): A/B within one run. Every N presents a global phase
// flips between A and B; a feature switch set to "ab" (EDEN_NX2_FOO=ab, or ab:<value> for a switch
// with several on values) is off in A and on in B. Each phase's presents form a block, logged as
//   NX2_AB blok=<n> faz=<A|B> sunum=<presents> sure_ms=<ms> fps=<x.xx> [more fields]
// (the first five fields fixed, in this order: tools read them). Run to run drift cancels out
// when A and B blocks of the same run are paired. EDEN_NX2_AB_DISCARD=<K> (default 60): the first
// K presents after a flip go into no block, while the work in flight from the other phase drains.
// EDEN_NX2_AB_START=B: the first phase is B (a second run with the order reversed).
// The vi2 present thread (core/hle/service/nx2_gsv.h) counts the presents and flips the phase;
// features read it with Phase() (one relaxed atomic load) or, at their own safe point, a Latch.
// docs/nx2-ab-toggle.md tells how to bind a switch and which ones can't be.
namespace Common::Nx2Ab {

enum class Phase : u8 { A = 0, B = 1 };

constexpr char PhaseName(Phase phase) {
    return phase == Phase::A ? 'A' : 'B';
}

constexpr Phase Other(Phase phase) {
    return phase == Phase::A ? Phase::B : Phase::A;
}

struct Config {
    u32 toggle{};   ///< presents per phase; 0: off
    u32 discard{};  ///< presents after a flip outside every block (below toggle)
    Phase start{Phase::A};
};

constexpr u32 DefaultDiscard = 60;

namespace Detail {
/// A decimal number, nullopt if `value` is empty, has anything else or overflows 32 bits.
constexpr std::optional<u32> ParseU32(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    u64 n = 0;
    for (const char* c = value; *c != '\0'; ++c) {
        if (*c < '0' || *c > '9') {
            return std::nullopt;
        }
        n = n * 10 + static_cast<u64>(*c - '0');
        if (n > 0xffffffffULL) {
            return std::nullopt;
        }
    }
    return static_cast<u32>(n);
}
} // namespace Detail

/// The three variables' values (nullptr: unset). A bad or 0 toggle leaves A/B off; a bad discard
/// takes the default; the discard is held below the toggle so each block has a present.
constexpr Config ParseConfig(const char* toggle, const char* discard, const char* start) {
    Config config;
    config.toggle = Detail::ParseU32(toggle).value_or(0);
    if (config.toggle == 0) {
        return {};
    }
    config.discard = Detail::ParseU32(discard).value_or(DefaultDiscard);
    if (config.discard >= config.toggle) {
        config.discard = config.toggle - 1;
    }
    config.start = start != nullptr && (start[0] == 'B' || start[0] == 'b') ? Phase::B : Phase::A;
    return config;
}

/// The run's configuration, read from the environment once.
inline const Config& GetConfig() {
    static const Config config =
        ParseConfig(std::getenv("EDEN_NX2_AB_TOGGLE"), std::getenv("EDEN_NX2_AB_DISCARD"),
                    std::getenv("EDEN_NX2_AB_START"));
    return config;
}

inline bool Enabled() {
    return GetConfig().toggle != 0;
}

/// Flips so far (constant-initialised: safe to read from any static initialiser).
inline std::atomic<u32> g_flips{0};

/// The phase after `flips` flips from `start`.
constexpr Phase PhaseAfter(Phase start, u32 flips) {
    return (flips & 1) != 0 ? Other(start) : start;
}

/// The phase now. A/B off: the start phase (A unless EDEN_NX2_AB_START=B), so an "ab" switch
/// without EDEN_NX2_AB_TOGGLE stays off.
inline Phase CurrentPhase() {
    return PhaseAfter(GetConfig().start, g_flips.load(std::memory_order_relaxed));
}

/// Called by the present thread at a flip (only there).
inline void Flip() {
    g_flips.fetch_add(1, std::memory_order_relaxed);
}

/// "ab" → 1, "ab:<n>" → n (the value phase B gives), anything else → nullopt (the feature's own
/// value). "ab:0" and a bad number after "ab:" are nullopt too.
constexpr std::optional<u32> AbOnValue(const char* value) {
    if (value == nullptr || value[0] != 'a' || value[1] != 'b') {
        return std::nullopt;
    }
    if (value[2] == '\0') {
        return 1;
    }
    if (value[2] != ':') {
        return std::nullopt;
    }
    const auto n = Detail::ParseU32(value + 3);
    if (!n || *n == 0) {
        return std::nullopt;
    }
    return n;
}

/// A feature switch's value: fixed, or following the phase (0 in A, the on value in B).
class Switch {
public:
    constexpr Switch() = default;

    /// `value`: the variable's text; `fixed`: the feature's own reading of it, used unless the
    /// text is "ab" / "ab:<n>".
    static constexpr Switch Parse(const char* value, u32 fixed) {
        Switch s;
        if (const auto on = AbOnValue(value)) {
            s.follows = true;
            s.on = *on;
        } else {
            s.on = fixed;
        }
        return s;
    }

    constexpr bool FollowsPhase() const {
        return follows;
    }

    /// The value in `phase`.
    constexpr u32 At(Phase phase) const {
        return !follows || phase == Phase::B ? on : 0;
    }

    /// The value now (the phase can change between two calls: see Latch).
    u32 Now() const {
        return follows ? At(CurrentPhase()) : on;
    }

    /// Can it be on in some phase (does the feature need its setup at all).
    constexpr bool EverOn() const {
        return on != 0;
    }

private:
    bool follows{};
    u32 on{};
};

/// Logs a switch's binding once, where it is parsed: "<name>=ab follows the A/B phase", and a
/// warning when EDEN_NX2_AB_TOGGLE is off (the phase never flips: the switch keeps its start
/// phase's value for the whole run, which for "ab" and START=A turns the feature off).
inline void LogBinding(const char* name, const Switch& s) {
    if (!s.FollowsPhase()) {
        return;
    }
    LOG_INFO(Common, "{}=ab: follows the A/B phase (0 in A, {} in B)", name, s.At(Phase::B));
    if (!Enabled()) {
        LOG_WARNING(Common,
                    "{}=ab without EDEN_NX2_AB_TOGGLE: the phase never flips, {} all run", name,
                    s.At(GetConfig().start));
    }
}

/// The phase as one thread last took it at its safe point (a frame boundary, a drained queue):
/// between two Update() calls the phase it reports stays the same, so a frame isn't half one
/// phase and half the other.
class Latch {
public:
    /// Takes the current phase; true if it changed since the last Update (the first call: false).
    bool Update() {
        const u32 flips = g_flips.load(std::memory_order_relaxed);
        if (!taken) {
            taken = true;
            seen = flips;
            return false;
        }
        if (flips == seen) {
            return false;
        }
        const bool changed = ((flips ^ seen) & 1) != 0;
        seen = flips;
        return changed;
    }

    Phase Get() const {
        return PhaseAfter(GetConfig().start, seen);
    }

    u32 Value(const Switch& s) const {
        return s.FollowsPhase() ? s.At(Get()) : s.Now();
    }

private:
    u32 seen{};
    bool taken{};
};

/// A finished block: one phase's presents after the discard.
struct Block {
    u32 index{};          ///< 0, 1, 2... in the run
    Phase phase{};
    u32 presents{};       ///< frame intervals in the block
    long long us{};       ///< their total time
    long long max_us{};   ///< the longest
    u32 over_100ms{};     ///< intervals over 100 ms (loading, hitches)

    double Ms() const {
        return static_cast<double>(us) / 1000.0;
    }
    double Fps() const {
        return us > 0 ? static_cast<double>(presents) * 1e6 / static_cast<double>(us) : 0.0;
    }
};

/// The log line (the first five fields fixed).
inline std::string FormatLine(const Block& b) {
    return fmt::format("NX2_AB blok={} faz={} sunum={} sure_ms={:.1f} fps={:.2f} max_ms={:.1f} "
                       "ust100={}",
                       b.index, PhaseName(b.phase), b.presents, b.Ms(), b.Fps(),
                       static_cast<double>(b.max_us) / 1000.0, b.over_100ms);
}

/// `b` numbered and labelled by the run's global flips (taken before the flip that ends it): the
/// phase the features read during the block. A game that opened a second present session would
/// restart that session's own count at 0, and its labels could be the wrong way round.
constexpr Block Globally(Block b, Phase start, u32 flips) {
    b.index = flips;
    b.phase = PhaseAfter(start, flips);
    return b;
}

/// The present counting, apart from the clock and the globals (tests feed it times). Phase n
/// runs from the present that flipped into it (present 0) to its `toggle`-th present, which ends
/// its block and flips; the block holds the intervals after present `discard`. The run's first
/// present is phase 0's present 0.
class Blocks {
public:
    explicit constexpr Blocks(Config config_) : config{config_} {}

    struct Step {
        std::optional<Block> block; ///< set at a flip: the block that ended
        bool flipped{};
    };

    /// One present at `now_us` (a monotonic clock).
    Step Present(long long now_us) {
        Step step;
        if (config.toggle == 0) {
            return step;
        }
        if (!started) {
            started = true;
            StartPhase();
            last_us = now_us;
            return step;
        }
        ++count;
        if (count > config.discard) {
            const long long frame = now_us - last_us;
            current.us += frame;
            current.max_us = frame > current.max_us ? frame : current.max_us;
            current.over_100ms += frame > 100'000 ? 1 : 0;
            ++current.presents;
        }
        last_us = now_us;
        if (count == config.toggle) {
            step.block = current;
            step.flipped = true;
            ++index;
            StartPhase();
        }
        return step;
    }

    Phase CurrentPhase() const {
        return PhaseAfter(config.start, index);
    }

    u32 Index() const {
        return index;
    }

private:
    void StartPhase() {
        count = 0;
        current = {.index = index, .phase = CurrentPhase()};
    }

    Config config;
    bool started{};
    u32 count{};  ///< presents since the phase's present 0
    u32 index{};  ///< the phase (= block) number
    long long last_us{};
    Block current{};
};

} // namespace Common::Nx2Ab
