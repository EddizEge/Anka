// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/common_types.h"

// EDEN_NX2_VI2_REPLY_FILE=<path> / EDEN_NX2_VI2_REPLY=<inline> (off by default): reply words for
// nx2:vi2 commands, for live experiments with the display queries Donkey Kong Bananza sends every
// frame (104, 105, 107) and once at setup (261, 265, 267, 271, 351). A listed command answers
// success followed by its words; an empty list is a success without data words.
//   File, one command a line:  "<command>: <word> <word> ..."  ('#' starts a comment; words
//                               decimal or 0x hexadecimal, separated by spaces or commas)
//   Inline, ';' between items: "104:1;105:1,0;107:0x3c"
// The file is checked at most once a second while the game calls a listed command and re-read
// when its time or size changes; a table that fails to read or parse leaves the previous one.
namespace Service::Nx2Vi2Reply {

// Reply data words per command (a TIPC message is 64 words, two of them the header and result).
constexpr std::size_t MaxWords = 32;

using Table = std::map<u32, std::vector<u32>>;

struct Parsed {
    std::optional<Table> table; // empty on an error
    std::string error;          // "line 3: ..."
};

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
inline std::optional<u32> ParseWord(std::string_view text) {
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

// Lines separated by `separator` ('\n' for the file, ';' inline); `what` names them in errors.
inline Parsed Parse(std::string_view text, char separator = '\n', std::string_view what = "line") {
    Table table;
    u32 number = 0;
    const auto fail = [&](std::string message) {
        std::ostringstream out;
        out << what << ' ' << number << ": " << message;
        return Parsed{std::nullopt, out.str()};
    };
    while (!text.empty()) {
        ++number;
        const std::size_t end = text.find(separator);
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        line = Trim(line.substr(0, line.find('#')));
        if (line.empty()) {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return fail("no ':' after the command");
        }
        const auto command = ParseWord(line.substr(0, colon));
        if (!command) {
            return fail("command \"" + std::string{Trim(line.substr(0, colon))} + "\" is not a number");
        }
        if (table.contains(*command)) {
            return fail("command " + std::to_string(*command) + " is listed twice");
        }
        std::vector<u32> words;
        std::string_view rest = line.substr(colon + 1);
        while (true) {
            const std::size_t start = rest.find_first_not_of(" \t,");
            if (start == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(start);
            const std::size_t stop = rest.find_first_of(" \t,");
            const std::string_view token = rest.substr(0, stop);
            const auto word = ParseWord(token);
            if (!word) {
                return fail("word \"" + std::string{token} + "\" is not a 32-bit number");
            }
            if (words.size() == MaxWords) {
                return fail("more than " + std::to_string(MaxWords) + " words");
            }
            words.push_back(*word);
            rest = stop == std::string_view::npos ? std::string_view{} : rest.substr(stop);
        }
        table.emplace(*command, std::move(words));
    }
    return Parsed{std::move(table), {}};
}

inline Parsed ParseInline(std::string_view text) {
    return Parse(text, ';', "item");
}

inline std::string FormatWords(const std::vector<u32>& words) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < words.size(); ++i) {
        out << (i == 0 ? "0x" : ",0x") << std::hex << words[i];
    }
    out << ']';
    return out.str();
}

// "104=[0x1] 105=[]", or "(empty)".
inline std::string Describe(const Table& table) {
    if (table.empty()) {
        return "(empty)";
    }
    std::string text;
    for (const auto& [command, words] : table) {
        if (!text.empty()) {
            text += ' ';
        }
        text += std::to_string(command) + '=' + FormatWords(words);
    }
    return text;
}

// The live table: versions, and which commands changed with each version. Not thread safe.
class Source {
public:
    enum class Kind {
        None,   // nothing checked or nothing changed
        Loaded, // a new table: message is its Describe() text
        Same,   // the file changed, the table did not
        Failed, // message tells why; the previous table stays
    };
    struct Update {
        Kind kind{Kind::None};
        std::string message;
    };

    Source() = default;
    explicit Source(std::filesystem::path path_) : path{std::move(path_)} {}

    // Takes `next` as the table (a new version if it differs). Returns Loaded or Same.
    Update Apply(Table next) {
        if (version != 0 && next == table) {
            return {Kind::Same, {}};
        }
        ++version;
        for (const auto& [command, words] : table) {
            const auto it = next.find(command);
            if (it == next.end() || it->second != words) {
                changed_at[command] = version;
            }
        }
        for (const auto& [command, words] : next) {
            if (!table.contains(command)) {
                changed_at[command] = version;
            }
        }
        table = std::move(next);
        return {Kind::Loaded, Describe(table)};
    }

    // Re-reads the file when its time or size changed, at most once a second (the first call
    // always checks). A read or parse error is reported once until the file changes again.
    Update Poll(std::chrono::steady_clock::time_point now) {
        if (path.empty() || (checked && now - last_check < std::chrono::seconds{1})) {
            return {};
        }
        checked = true;
        last_check = now;
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(path, ec);
        const auto size = ec ? std::uintmax_t{} : std::filesystem::file_size(path, ec);
        if (ec) {
            stamp.reset();
            return Fail("cannot read " + path.string() + ": " + ec.message());
        }
        const Stamp current{time, size};
        if (stamp == current) {
            return {};
        }
        stamp = current;
        std::ifstream file{path, std::ios::binary};
        std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        if (!file && !file.eof()) {
            stamp.reset(); // try again next second (an editor may hold the file)
            return Fail("cannot read " + path.string());
        }
        error.clear(); // new contents: report their error even if it is the same
        auto parsed = Parse(text);
        if (!parsed.table) {
            return Fail(path.string() + ": " + parsed.error);
        }
        error.clear();
        return Apply(std::move(*parsed.table));
    }

    // The reply words of `command`, or nullptr when it is not in the table.
    const std::vector<u32>* Find(u32 command) const {
        const auto it = table.find(command);
        return it == table.end() ? nullptr : &it->second;
    }

    // True on a command's first call, and on its first call after its reply changed.
    bool FirstCallSinceChange(u32 command) {
        const auto changed = changed_at.find(command);
        const u32 at = changed == changed_at.end() ? 0 : changed->second;
        const auto [it, inserted] = logged.try_emplace(command, at);
        if (inserted) {
            return true;
        }
        if (it->second == at) {
            return false;
        }
        it->second = at;
        return true;
    }

    u32 Version() const {
        return version;
    }
    const Table& GetTable() const {
        return table;
    }

private:
    struct Stamp {
        std::filesystem::file_time_type time;
        std::uintmax_t size;
        bool operator==(const Stamp&) const = default;
    };

    Update Fail(std::string message) {
        if (message == error) {
            return {};
        }
        error = message;
        return {Kind::Failed, std::move(message)};
    }

    std::filesystem::path path;
    bool checked{};
    std::chrono::steady_clock::time_point last_check{};
    std::optional<Stamp> stamp;
    std::string error; // the last error reported
    Table table;
    u32 version{}; // 0: no table yet
    std::map<u32, u32> changed_at; // command -> version its reply last changed in
    std::map<u32, u32> logged;     // command -> changed_at value when its reply was last logged
};

} // namespace Service::Nx2Vi2Reply
