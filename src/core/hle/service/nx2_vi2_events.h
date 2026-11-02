// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// The parts of the nx2:vi2 event and setup switches (nx2_gsv.h) that need no kernel: parsing,
// the commands Donkey Kong Bananza is known to send, reply words and the VRR vsync clock.
namespace Service::Nx2Vi2Events {

inline std::string_view Trim(std::string_view text) {
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// A decimal or 0x hexadecimal u32, the whole text.
inline std::optional<u32> ParseNumber(std::string_view text) {
    text = Trim(text);
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) {
        return std::nullopt;
    }
    u32 value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// Items separated by ',' or ';' (blank items skipped).
inline std::vector<std::string_view> SplitItems(std::string_view text) {
    std::vector<std::string_view> items;
    while (!text.empty()) {
        const std::size_t end = text.find_first_of(",;");
        const std::string_view item = Trim(text.substr(0, end));
        if (!item.empty()) {
            items.push_back(item);
        }
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
    return items;
}

// EDEN_NX2_VI2_KIND_EVENTS: what signals the event vi2 31 hands out for a kind. Kinds 1-3 only;
// kind 0 (and 32's event) stays on the vblanks.
enum class KindMode : u8 {
    Vsync, // every vblank (as before)
    Once,  // once, at the first present after the 31 call that created it
    Never, // never
};

constexpr u32 KindCount = 4; // 31 takes kinds 0-3 (Bananza: 0 at boot, 1-3 before its first present)

struct KindModes {
    std::array<KindMode, KindCount> modes{}; // all Vsync

    bool AnyChanged() const {
        return std::ranges::any_of(modes, [](KindMode m) { return m != KindMode::Vsync; });
    }
    // Vsync for a kind out of range.
    KindMode Of(u32 kind) const {
        return kind < KindCount ? modes[kind] : KindMode::Vsync;
    }
    bool operator==(const KindModes&) const = default;
};

inline std::string_view ModeName(KindMode mode) {
    switch (mode) {
    case KindMode::Once:
        return "once";
    case KindMode::Never:
        return "never";
    case KindMode::Vsync:
    default:
        return "vsync";
    }
}

inline std::optional<KindMode> ParseMode(std::string_view text) {
    text = Trim(text);
    if (text == "vsync") {
        return KindMode::Vsync;
    }
    if (text == "once") {
        return KindMode::Once;
    }
    if (text == "never") {
        return KindMode::Never;
    }
    return std::nullopt;
}

struct ParsedKinds {
    std::optional<KindModes> modes; // empty on an error
    std::string error;
};

// "1": kinds 1-3 once; "0": kinds 1-3 never; "<kind>:<mode>[,<kind>:<mode>...]" with kinds 1-3
// and modes vsync / once / never (',' or ';' between items; a kind not listed stays vsync).
inline ParsedKinds ParseKindEvents(std::string_view text) {
    text = Trim(text);
    KindModes result;
    if (text == "1" || text == "0") {
        const KindMode mode = text == "1" ? KindMode::Once : KindMode::Never;
        for (u32 kind = 1; kind < KindCount; ++kind) {
            result.modes[kind] = mode;
        }
        return {result, {}};
    }
    const auto items = SplitItems(text);
    if (items.empty()) {
        return {std::nullopt, "empty"};
    }
    std::array<bool, KindCount> listed{};
    for (const std::string_view item : items) {
        const std::size_t colon = item.find(':');
        if (colon == std::string_view::npos) {
            return {std::nullopt, "\"" + std::string{item} + "\": no ':' after the kind"};
        }
        const auto kind = ParseNumber(item.substr(0, colon));
        if (!kind || *kind == 0 || *kind >= KindCount) {
            return {std::nullopt, "\"" + std::string{item} + "\": the kind is not 1, 2 or 3"};
        }
        const auto mode = ParseMode(item.substr(colon + 1));
        if (!mode) {
            return {std::nullopt, "\"" + std::string{item} + "\": the mode is not vsync, once or never"};
        }
        if (listed[*kind]) {
            return {std::nullopt, "kind " + std::to_string(*kind) + " is listed twice"};
        }
        listed[*kind] = true;
        result.modes[*kind] = *mode;
    }
    return {result, {}};
}

// "1:once 2:never 3:vsync" (kinds 1-3).
inline std::string DescribeKinds(const KindModes& modes) {
    std::string text;
    for (u32 kind = 1; kind < KindCount; ++kind) {
        text += (kind == 1 ? "" : " ") + std::to_string(kind) + ':' + std::string{ModeName(modes.modes[kind])};
    }
    return text;
}

// The commands Bananza sent in every logged session (02.11.2026, V1-V4, 97a, 97c): the first call
// of any other vi2 command is logged as new (EDEN_NX2_VI2_DIAG).
constexpr std::array<u32, 22> KnownCommands{1,   3,   5,   11,  21,  31,  32,  51,
                                            101, 104, 105, 107, 111, 201, 212, 229,
                                            251, 261, 265, 267, 271, 351};

inline bool IsKnownCommand(u32 command) {
    return std::ranges::find(KnownCommands, command) != KnownCommands.end();
}

// Commands whose own reply carries a handle (11, 31, 32), fills a buffer (21) or presents (229):
// EDEN_NX2_VI2_REPLY_OWN never answers them from the table.
inline bool OwnReplyFixed(u32 command) {
    return command == 11 || command == 21 || command == 31 || command == 32 || command == 229;
}

struct ParsedCommands {
    std::set<u32> commands;
    std::set<u32> refused; // OwnReplyFixed ones, left out
    std::string error;     // the whole list is refused when set
};

// EDEN_NX2_VI2_REPLY_OWN: "1,3,101" (decimal or 0x hexadecimal; ',' or ';' between them).
inline ParsedCommands ParseCommandList(std::string_view text) {
    ParsedCommands result;
    const auto items = SplitItems(text);
    if (items.empty()) {
        result.error = "empty";
        return result;
    }
    for (const std::string_view item : items) {
        const auto command = ParseNumber(item);
        if (!command) {
            result = {};
            result.error = "\"" + std::string{item} + "\" is not a command number";
            return result;
        }
        (OwnReplyFixed(*command) ? result.refused : result.commands).insert(*command);
    }
    return result;
}

// A TIPC message's data words after the header and its handle descriptor (for a reply, the result
// word first), at most `max_words`.
inline std::vector<u32> TipcWords(std::span<const u32> buffer, u32 max_words = 16) {
    std::vector<u32> words;
    if (buffer.size() < 2) {
        return words;
    }
    const u32 size = buffer[1] & 0x3FFU;
    std::size_t start = 2;
    if ((buffer[1] & 0x80000000U) != 0) {
        if (buffer.size() < 3) {
            return words;
        }
        const u32 descriptor = buffer[2];
        start = 3 + ((descriptor & 1U) ? 2U : 0U) + ((descriptor >> 1) & 0xFU) + ((descriptor >> 5) & 0xFU);
    }
    for (u32 i = 0; i < size && i < max_words && start + i < buffer.size(); ++i) {
        words.push_back(buffer[start + i]);
    }
    return words;
}

// EDEN_NX2_VRR_VSYNC: "1" (120 and 40 Hz) or "<max Hz>:<min Hz>" (30 <= min < max <= 480).
struct VrrVsyncRates {
    u32 max_hz{120};
    u32 min_hz{40};
};

struct ParsedRates {
    std::optional<VrrVsyncRates> rates;
    std::string error;
};

inline ParsedRates ParseVrrVsync(std::string_view text) {
    text = Trim(text);
    if (text == "1") {
        return {VrrVsyncRates{}, {}};
    }
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return {std::nullopt, "not 1 or <max Hz>:<min Hz>"};
    }
    const auto max_hz = ParseNumber(text.substr(0, colon));
    const auto min_hz = ParseNumber(text.substr(colon + 1));
    if (!max_hz || !min_hz) {
        return {std::nullopt, "not 1 or <max Hz>:<min Hz>"};
    }
    if (*min_hz < 30 || *min_hz >= *max_hz || *max_hz > 480) {
        return {std::nullopt, "needs 30 <= min < max <= 480 Hz"};
    }
    return {VrrVsyncRates{*max_hz, *min_hz}, {}};
}

// A VRR panel's scan clock: a present starts a scan at once, but no sooner than min_ns after the
// previous scan starts (the panel's top rate); without a present the panel scans the old frame
// again max_ns after the previous scan (its bottom rate). Times are CoreTiming ns. Not thread safe.
struct VrrVsyncClock {
    s64 min_ns{1'000'000'000LL / 120};
    s64 max_ns{1'000'000'000LL / 40};
    s64 last{-1}; // the previous scan, -1: none yet

    static VrrVsyncClock FromRates(const VrrVsyncRates& rates) {
        return VrrVsyncClock{
            .min_ns = 1'000'000'000LL / rates.max_hz,
            .max_ns = 1'000'000'000LL / rates.min_hz,
        };
    }

    // When the scan of a frame presented at `now` starts (`now` or later).
    s64 PresentDue(s64 now) const {
        return last < 0 ? now : (std::max)(now, last + min_ns);
    }
    // Whether the panel scans again by itself at `now`.
    bool DeadlineDue(s64 now) const {
        return last < 0 || now - last >= max_ns;
    }
    // The bottom rate timer, firing at `when` (now): the time to its next firing, max_ns after
    // the previous scan (at least 1 us, so it never fires twice at one time).
    s64 NextDeadlineFrom(s64 when) const {
        const s64 next = last < 0 ? when + max_ns : last + max_ns;
        return (std::max<s64>)(next - when, 1'000);
    }
    void Scanned(s64 now) {
        last = now;
    }
};

} // namespace Service::Nx2Vi2Events
