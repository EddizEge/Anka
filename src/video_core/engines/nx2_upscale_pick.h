// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>

#include "common/common_types.h"

namespace Tegra::Engines::Nx2UpscalePick {

// Donkey Kong Bananza upscales its dynamic resolution with tensor-core compute programs (one per
// resolution step, rejected: "native matrix multiply"). Their constant bank 0 holds the work size
// (word 0 the block's threads, words 1-2 the grid) and the input and output sizes minus one
// (words 6-7 and 10-11), e.g. 128, 60, 34, 1, 0, 0, 1919, 1079, 0, 0, 2879, 1619.
struct Sizes {
    u32 in_w{}, in_h{}, out_w{}, out_h{};
    bool operator==(const Sizes&) const = default;
};

inline std::optional<Sizes> SizesFromBank0(std::span<const u32> words, u32 block_x, u32 grid_x,
                                           u32 grid_y) {
    if (words.size() < 12 || words[0] != block_x || words[1] != grid_x || words[2] != grid_y) {
        return std::nullopt;
    }
    const Sizes sizes{words[6] + 1, words[7] + 1, words[10] + 1, words[11] + 1};
    const auto plausible = [](u32 value) { return value >= 64 && value <= 8192; };
    if (!plausible(sizes.in_w) || !plausible(sizes.in_h) || !plausible(sizes.out_w) ||
        !plausible(sizes.out_h) || sizes.out_w <= sizes.in_w || sizes.out_h <= sizes.in_h) {
        return std::nullopt;
    }
    // The same aspect ratio within 1% (2095x1179 -> 2880x1620 is off by 0.05%).
    const u64 a = u64{sizes.in_w} * sizes.out_h;
    const u64 b = u64{sizes.out_w} * sizes.in_h;
    if ((a > b ? a - b : b - a) * 100 > b) {
        return std::nullopt;
    }
    return sizes;
}

/// Bank 0's first 12 words are the work size and the sizes (SizesFromBank0), not handles: word 10,
/// the output width minus one (0xb3f), read as a handle names texture 2879, one of the game's
/// 1440x810 half-resolution buffers, which the 1440x810 step took as its input (white sky, grey
/// ground).
constexpr u32 Bank0HeaderBytes = 0x30;

/// An image a word of a bound constant bank names (texture handle: pool index in bits 0-19).
struct Candidate {
    u32 slot{};
    u32 offset{};
    u32 index{}; ///< texture pool entry
    u64 address{};
    u32 width{};
    u32 height{};
    u32 format{};   ///< Tegra::Texture::TextureFormat
    bool usable{};  ///< a 2D block-linear image the blit can read and write
};

struct Pick {
    std::optional<std::size_t> input;
    std::array<std::size_t, 3> outputs{};
    std::size_t output_count{};
    /// Output-sized images of the input's format at distinct addresses, also those past `outputs`:
    /// more than outputs.size() means the banks name images beyond the program's own (a scene's
    /// constant buffer), and blitting to the first few could overwrite another pass's image.
    std::size_t output_total{};
    /// Input-sized images of an input format at distinct addresses (more than one: the pick was
    /// by format order or by the preferred location).
    std::size_t input_matches{};

    bool Ambiguous() const {
        return output_total > outputs.size();
    }
};

using Location = std::pair<u32, u32>; ///< constant bank, byte offset

/// Choose with or without the preferred input location (one pass).
inline Pick ChooseOnce(const Sizes& sizes, std::span<const Candidate> candidates,
                       std::span<const u32> preference, std::optional<Location> input_at,
                       std::optional<Location> output_at, std::optional<Location> prefer_input,
                       bool skip_header) {
    Pick pick;
    const auto at = [](const Candidate& c, const std::optional<Location>& where) {
        return !where || (c.slot == where->first && c.offset == where->second);
    };
    const auto header = [skip_header](const Candidate& c, const std::optional<Location>& pinned) {
        return skip_header && !pinned && c.slot == 0 && c.offset < Bank0HeaderBytes;
    };
    const auto input_ok = [&](const Candidate& c) {
        return c.usable && c.width == sizes.in_w && c.height == sizes.in_h && at(c, input_at) &&
               !header(c, input_at) &&
               std::find(preference.begin(), preference.end(), c.format) != preference.end();
    };
    std::size_t best_rank = preference.size();
    std::optional<std::size_t> preferred;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& c = candidates[i];
        if (!input_ok(c)) {
            continue;
        }
        const bool seen = std::ranges::any_of(candidates.first(i), [&](const Candidate& d) {
            return input_ok(d) && d.address == c.address;
        });
        pick.input_matches += seen ? 0 : 1;
        if (prefer_input && !preferred && at(c, prefer_input)) {
            preferred = i;
        }
        const auto rank = static_cast<std::size_t>(
            std::find(preference.begin(), preference.end(), c.format) - preference.begin());
        if (rank < best_rank) {
            pick.input = i;
            best_rank = rank;
        }
    }
    if (preferred) {
        pick.input = preferred;
    }
    if (!pick.input) {
        return pick;
    }
    const Candidate& input = candidates[*pick.input];
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& c = candidates[i];
        if (!c.usable || c.width != sizes.out_w || c.height != sizes.out_h ||
            c.format != input.format || c.address == input.address || !at(c, output_at) ||
            header(c, output_at)) {
            continue;
        }
        const bool seen = std::ranges::any_of(candidates.first(i), [&](const Candidate& d) {
            return d.usable && d.width == sizes.out_w && d.height == sizes.out_h &&
                   d.format == input.format && d.address == c.address && at(d, output_at) &&
                   !header(d, output_at);
        });
        if (seen) {
            continue;
        }
        ++pick.output_total;
        if (pick.output_count < pick.outputs.size()) {
            pick.outputs[pick.output_count++] = i;
        }
    }
    return pick;
}

/// Whether a pick can be blitted: an input and outputs, not too many of them.
inline bool Usable(const Pick& pick) {
    return pick.input && pick.output_count != 0 && !pick.Ambiguous();
}

/// The input: the input-sized image whose format comes first in `preference`, or the one at
/// `prefer_input` (the location other programs' only input was found at) when that word names
/// one and the pick with it is usable (else the format order: the preferred image may have no
/// output of its format). The outputs: every output-sized image of the same format at its own
/// address (the result and, if the program keeps one, its history: writing both is harmless
/// when the program itself does not run); see Pick::Ambiguous for when there are too many of
/// them. Bank 0's header words are skipped (`skip_header`, EDEN_NX2_UPSCALE_HEADER=0: not) unless
/// `input_at` / `output_at` pin a word.
inline Pick Choose(const Sizes& sizes, std::span<const Candidate> candidates,
                   std::span<const u32> preference, std::optional<Location> input_at = {},
                   std::optional<Location> output_at = {},
                   std::optional<Location> prefer_input = {}, bool skip_header = true) {
    const Pick pick =
        ChooseOnce(sizes, candidates, preference, input_at, output_at, prefer_input, skip_header);
    if (!prefer_input || Usable(pick)) {
        return pick;
    }
    return ChooseOnce(sizes, candidates, preference, input_at, output_at, std::nullopt,
                      skip_header);
}

/// The input location a full scan may teach the other programs (EDEN_NX2_UPSCALE_LEARN): its
/// only input-sized image, named by a word of bank 0 past the header, with an unambiguous output.
inline std::optional<Location> Learnable(const Pick& pick, std::span<const Candidate> candidates) {
    if (!pick.input || pick.input_matches != 1 || pick.output_count == 0 || pick.Ambiguous()) {
        return std::nullopt;
    }
    const Candidate& c = candidates[*pick.input];
    if (c.slot != 0 || c.offset < Bank0HeaderBytes) {
        return std::nullopt;
    }
    return Location{c.slot, c.offset};
}

/// What a dispatch teaches (Learnable), if it may: only from a full scan (a scan of the
/// remembered words alone holds just the word picked before, its one input says nothing), with
/// learning on, no pinned input (EDEN_NX2_UPSCALE_IN) and nothing learned yet.
inline std::optional<Location> Teach(const Pick& pick, std::span<const Candidate> candidates,
                                     bool full_scan, bool learn, bool input_pinned, bool learned) {
    if (!full_scan || !learn || input_pinned || learned) {
        return std::nullopt;
    }
    return Learnable(pick, candidates);
}

/// Once a location is learned, a program whose remembered words (input first) take its input
/// from elsewhere scans again: its pick was made before, by format order, and the preferred
/// location only works in a scan (its remembered words do not hold it).
inline bool RescanAfterLearning(std::span<const Location> words, Location learned) {
    return !words.empty() && words.front() != learned;
}

/// "<bank>:<offset>" (offset in hex), e.g. "3:1a0".
inline std::optional<Location> ParseLocation(const char* text) {
    if (text == nullptr) {
        return std::nullopt;
    }
    u32 slot = 0;
    u32 offset = 0;
    const char* p = text;
    if (*p < '0' || *p > '7') {
        return std::nullopt;
    }
    slot = static_cast<u32>(*p++ - '0');
    if (*p++ != ':') {
        return std::nullopt;
    }
    bool digits = false;
    for (; *p != '\0'; ++p) {
        const char c = *p;
        const int value = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
        if (value < 0 || offset > 0xfffffff) {
            return std::nullopt;
        }
        offset = offset * 16 + static_cast<u32>(value);
        digits = true;
    }
    if (!digits) {
        return std::nullopt;
    }
    return Location{slot, offset};
}

} // namespace Tegra::Engines::Nx2UpscalePick
