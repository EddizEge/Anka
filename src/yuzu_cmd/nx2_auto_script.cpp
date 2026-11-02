// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/nx2_auto_script.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>

namespace Nx2AutoScript {
namespace {

struct KeyName {
    std::string_view name;
    int id;
};

// Ids are VirtualGamepad::VirtualButton values (yuzu.cpp checks them).
constexpr std::array<KeyName, 14> Keys{{
    {"A", 0},
    {"B", 1},
    {"X", 2},
    {"Y", 3},
    {"L", 6},
    {"R", 7},
    {"ZL", 8},
    {"ZR", 9},
    {"Plus", 10},
    {"Minus", 11},
    {"Left", 12},
    {"Up", 13},
    {"Right", 14},
    {"Down", 15},
}};

bool SameText(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
               return lower(x) == lower(y);
           });
}

std::vector<std::string_view> Split(std::string_view line) {
    std::vector<std::string_view> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
            ++i;
        }
        const size_t begin = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
            ++i;
        }
        if (i > begin) {
            words.push_back(line.substr(begin, i - begin));
        }
    }
    return words;
}

std::optional<double> Number(std::string_view word) {
    double value{};
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
    if (error != std::errc{} || end != word.data() + word.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// Returns an error message, or an empty string when the line was taken.
std::string ParseLine(const std::vector<std::string_view>& words, Script& script,
                      bool& anchor_set) {
    if (SameText(words[0], "anchor")) {
        if (words.size() != 2) {
            return "expected 'anchor boot' or 'anchor present'";
        }
        Anchor anchor;
        if (SameText(words[1], "boot")) {
            anchor = Anchor::Boot;
        } else if (SameText(words[1], "present")) {
            anchor = Anchor::Present;
        } else {
            return "unknown anchor (boot or present)";
        }
        if (anchor_set) {
            return "a second anchor line";
        }
        anchor_set = true;
        script.anchor = anchor;
        return {};
    }
    const auto start = Number(words[0]);
    if (!start || *start < 0.0) {
        return "the time is not a number of seconds >= 0";
    }
    Event event{.start = *start, .duration = 0.0, .stick = false, .id = 0, .x = 0.0f, .y = 0.0f,
                .name = {}};
    size_t duration_word;
    if (words.size() >= 2 && SameText(words[1], "stick")) {
        if (words.size() != 6) {
            return "expected '<seconds> stick <L|R> <x> <y> <held seconds>'";
        }
        if (SameText(words[2], "L")) {
            event.id = 0;
        } else if (SameText(words[2], "R")) {
            event.id = 1;
        } else {
            return "unknown stick (L or R)";
        }
        const auto x = Number(words[3]);
        const auto y = Number(words[4]);
        if (!x || !y || std::abs(*x) > 1.0 || std::abs(*y) > 1.0) {
            return "stick x and y must be numbers in [-1, 1]";
        }
        event.stick = true;
        event.x = static_cast<float>(*x);
        event.y = static_cast<float>(*y);
        event.name = event.id == 0 ? "stick L" : "stick R";
        duration_word = 5;
    } else {
        if (words.size() != 3) {
            return "expected '<seconds> <key> <held seconds>'";
        }
        const auto key = std::find_if(Keys.begin(), Keys.end(), [&](const KeyName& k) {
            return SameText(k.name, words[1]);
        });
        if (key == Keys.end()) {
            return "unknown key (A B X Y L R ZL ZR Plus Minus Up Down Left Right)";
        }
        event.id = key->id;
        event.name = std::string{key->name};
        duration_word = 2;
    }
    const auto duration = Number(words[duration_word]);
    if (!duration || *duration <= 0.0) {
        return "the held time is not a number of seconds > 0";
    }
    event.duration = *duration;
    script.events.push_back(std::move(event));
    return {};
}

} // Anonymous namespace

Script ParseScript(std::string_view text) {
    Script script;
    bool anchor_set = false;
    size_t line_number = 0;
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t end = std::min(text.find('\n', begin), text.size());
        std::string_view line = text.substr(begin, end - begin);
        ++line_number;
        begin = end + 1;
        if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        const auto words = Split(line);
        if (words.empty()) {
            continue;
        }
        if (std::string error = ParseLine(words, script, anchor_set); !error.empty()) {
            std::string shown{line};
            while (!shown.empty() && (shown.back() == ' ' || shown.back() == '\t' ||
                                      shown.back() == '\r')) {
                shown.pop_back();
            }
            script.errors.push_back("line " + std::to_string(line_number) + ": " + error + ": '" +
                                    shown + "'");
        }
    }
    return script;
}

std::vector<Action> Schedule(const Script& script) {
    std::vector<Action> actions;
    actions.reserve(script.events.size() * 2);
    for (size_t i = 0; i < script.events.size(); ++i) {
        const Event& event = script.events[i];
        actions.push_back({event.start, true, i});
        actions.push_back({event.start + event.duration, false, i});
    }
    std::stable_sort(actions.begin(), actions.end(), [](const Action& a, const Action& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        return !a.down && b.down;
    });
    return actions;
}

ExitSpec ParseExit(std::string_view text) {
    ExitSpec spec;
    const auto words = Split(text);
    if (words.empty()) {
        return spec;
    }
    if (words.size() != 1) {
        spec.error = "expected '<seconds>[@boot|@present]'";
        return spec;
    }
    std::string_view seconds = words[0];
    if (const size_t at = seconds.find('@'); at != std::string_view::npos) {
        const std::string_view anchor = seconds.substr(at + 1);
        if (SameText(anchor, "boot")) {
            spec.anchor = Anchor::Boot;
        } else if (SameText(anchor, "present")) {
            spec.anchor = Anchor::Present;
        } else {
            spec.error = "unknown anchor after '@' (boot or present)";
            return spec;
        }
        seconds = seconds.substr(0, at);
    }
    const auto value = Number(seconds);
    if (!value || *value < 0.0) {
        spec.error = "the time is not a number of seconds >= 0";
        return spec;
    }
    spec.seconds = *value;
    spec.enabled = *value > 0.0;
    return spec;
}

const char* AnchorName(Anchor anchor) {
    return anchor == Anchor::Boot ? "boot" : "present";
}

} // namespace Nx2AutoScript
