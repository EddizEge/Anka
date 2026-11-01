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

    bool Ambiguous() const {
        return output_total > outputs.size();
    }
};

using Location = std::pair<u32, u32>; ///< constant bank, byte offset

/// The input: the input-sized image whose format comes first in `preference`. The outputs: every
/// output-sized image of the same format at its own address (the result and, if the program keeps
/// one, its history: writing both is harmless when the program itself does not run); see
/// Pick::Ambiguous for when there are too many of them.
inline Pick Choose(const Sizes& sizes, std::span<const Candidate> candidates,
                   std::span<const u32> preference, std::optional<Location> input_at = {},
                   std::optional<Location> output_at = {}) {
    Pick pick;
    const auto at = [](const Candidate& c, const std::optional<Location>& where) {
        return !where || (c.slot == where->first && c.offset == where->second);
    };
    std::size_t best_rank = preference.size();
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& c = candidates[i];
        if (!c.usable || c.width != sizes.in_w || c.height != sizes.in_h || !at(c, input_at)) {
            continue;
        }
        const auto rank = static_cast<std::size_t>(
            std::find(preference.begin(), preference.end(), c.format) - preference.begin());
        if (rank < best_rank) {
            pick.input = i;
            best_rank = rank;
        }
    }
    if (!pick.input) {
        return pick;
    }
    const Candidate& input = candidates[*pick.input];
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& c = candidates[i];
        if (!c.usable || c.width != sizes.out_w || c.height != sizes.out_h ||
            c.format != input.format || c.address == input.address || !at(c, output_at)) {
            continue;
        }
        const bool seen = std::ranges::any_of(candidates.first(i), [&](const Candidate& d) {
            return d.usable && d.width == sizes.out_w && d.height == sizes.out_h &&
                   d.format == input.format && d.address == c.address && at(d, output_at);
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
