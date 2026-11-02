// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Service {
class HLERequestContext;
}

// EDEN_NX2_IPC_TRACE=<filter> (off by default): the first time a matching (service, command,
// request data words) triple is seen, log the requesting guest thread and its backtrace with each
// frame's module, offset, raw symbol and demangled name; the SDK function a command belongs to
// tells its meaning and, through the mangled name, its parameter types. Filter: "all", or items
// "<service name prefix>[:command,command...]" separated by ';' ("nx2:vi2", "nx2:vi2:104,105").
// Data words make a request new only for commands an item lists (at most MaxWordVariants word
// lists each); "nx2:vi2" alone traces each command once. The value "1" keeps the older meaning
// (log every dispatched command, no backtraces).
namespace Service::Nx2IpcTrace {

// At most this many requests are traced; then "trace limit reached" is logged once.
constexpr u32 MaxTraced = 400;
// Frames logged per request.
constexpr u32 MaxFrames = 32;
// Request data words logged (and, for listed commands, making a request "new").
constexpr u32 MaxDataWords = 16;
// Different data word lists traced per (service, command) a filter item lists.
constexpr u32 MaxWordVariants = 4;

// Parses a command number: decimal or 0x hexadecimal, the whole text.
inline std::optional<u32> ParseNumber(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
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

struct FilterItem {
    std::string prefix;        // service name prefix; empty matches every service
    std::vector<u32> commands; // empty: every command
};

enum class MatchKind {
    None,    // no item matches
    Service, // only items without a command list match: data words do not make it new
    Command, // an item lists the command: its data words make it new
};

struct Filter {
    std::vector<FilterItem> items;

    MatchKind Classify(std::string_view service, u32 command) const {
        MatchKind kind = MatchKind::None;
        for (const auto& item : items) {
            if (!service.starts_with(item.prefix)) {
                continue;
            }
            if (item.commands.empty()) {
                kind = MatchKind::Service;
                continue;
            }
            for (const u32 c : item.commands) {
                if (c == command) {
                    return MatchKind::Command;
                }
            }
        }
        return kind;
    }

    bool Matches(std::string_view service, u32 command) const {
        return Classify(service, command) != MatchKind::None;
    }
};

// Which requests are new: a (service, command type, command) the first time, and for a listed
// command (by_words) each of its first MaxWordVariants data word lists. Not thread safe.
class Seen {
public:
    // True, and remembered, when the request is new.
    bool Admit(std::string_view service, u32 type, u32 command, const std::vector<u32>& words,
               bool by_words) {
        auto [it, inserted] = entries.try_emplace(std::make_tuple(std::string{service}, type, command));
        auto& variants = it->second;
        if (!by_words) {
            return inserted;
        }
        if (std::find(variants.begin(), variants.end(), words) != variants.end() ||
            variants.size() >= MaxWordVariants) {
            return false;
        }
        variants.push_back(words);
        return true;
    }

private:
    std::map<std::tuple<std::string, u32, u32>, std::vector<std::vector<u32>>> entries;
};

// Service names contain ':' themselves ("nx2:vi2"), so only a last ':' followed by a valid command
// list splits an item: "nx2:vi2:104,105" -> prefix "nx2:vi2", commands {104, 105}.
inline std::optional<Filter> ParseFilter(std::string_view text) {
    Filter filter;
    while (!text.empty()) {
        const std::size_t end = text.find(';');
        std::string_view item = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
            item.remove_prefix(1);
        }
        while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
            item.remove_suffix(1);
        }
        if (item.empty()) {
            continue;
        }
        if (item == "all") {
            filter.items.push_back({});
            continue;
        }
        FilterItem parsed{.prefix = std::string{item}, .commands = {}};
        const std::size_t colon = item.rfind(':');
        if (colon != std::string_view::npos && colon + 1 < item.size()) {
            std::vector<u32> commands;
            std::string_view list = item.substr(colon + 1);
            bool valid = true;
            while (valid) {
                const std::size_t comma = list.find(',');
                const auto number = ParseNumber(list.substr(0, comma));
                valid = number.has_value();
                if (valid) {
                    commands.push_back(*number);
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                list = list.substr(comma + 1);
            }
            if (valid) {
                parsed.prefix = std::string{item.substr(0, colon)};
                parsed.commands = std::move(commands);
            }
        }
        filter.items.push_back(std::move(parsed));
    }
    if (filter.items.empty()) {
        return std::nullopt;
    }
    return filter;
}

// The request's data words (at most MaxDataWords) from its command buffer. TIPC: the words after
// the header, the handle descriptor and the buffer descriptors. CMIF: the arguments after the
// command id (payload_offset: the word after the 'SFCI' header), as the client's data size less
// its 16 bytes of alignment, the 'SFCI' header, the command id and a domain header.
inline std::vector<u32> RequestDataWords(std::span<const u32> buffer, bool tipc, u32 payload_offset,
                                         bool domain) {
    std::vector<u32> words;
    if (buffer.size() < 2) {
        return words;
    }
    const u32 data_size = buffer[1] & 0x3FFU;
    u32 start = 2;
    if ((buffer[1] & 0x80000000U) != 0) {
        if (buffer.size() < 3) {
            return words;
        }
        const u32 descriptor = buffer[2];
        start = 3 + ((descriptor & 1U) ? 2U : 0U) + ((descriptor >> 1) & 0xFU) + ((descriptor >> 5) & 0xFU);
    }
    start += 2 * ((buffer[0] >> 16) & 0xFU) + 3 * (((buffer[0] >> 20) & 0xFU) + ((buffer[0] >> 24) & 0xFU) +
                                                    ((buffer[0] >> 28) & 0xFU));
    u32 first = start;
    u32 count = data_size;
    if (!tipc) {
        const u32 overhead = 4 + 4 + (domain ? 4U : 0U);
        first = payload_offset + 2;
        count = data_size > overhead ? data_size - overhead : 0;
    }
    for (u32 i = 0; i < count && i < MaxDataWords; ++i) {
        if (first + i >= buffer.size()) {
            break;
        }
        words.push_back(buffer[first + i]);
    }
    return words;
}

// A module of the traced process: [base, end).
struct ModuleRange {
    u64 base{};
    u64 end{};
    std::string name;
    bool operator==(const ModuleRange&) const = default;
};

// The module holding `address` in a list sorted by base, or nullptr.
inline const ModuleRange* FindModule(std::span<const ModuleRange> modules, u64 address) {
    const auto it = std::upper_bound(modules.begin(), modules.end(), address,
                                     [](u64 a, const ModuleRange& m) { return a < m.base; });
    if (it == modules.begin()) {
        return nullptr;
    }
    const ModuleRange& module = *std::prev(it);
    return address < module.end ? &module : nullptr;
}

// A module's ELF symbols with a size, sorted by start (offsets from the module's base).
struct SymbolTable {
    struct Entry {
        u64 start{};
        u64 end{};
        std::string name;
    };
    std::vector<Entry> entries;

    // The innermost symbol holding `offset`, or nullptr.
    const Entry* Find(u64 offset) const {
        auto it = std::upper_bound(entries.begin(), entries.end(), offset,
                                   [](u64 o, const Entry& e) { return o < e.start; });
        // Symbols may nest or overlap; look back a little for one that holds the offset.
        for (int i = 0; i < 16 && it != entries.begin(); ++i) {
            --it;
            if (offset < it->end) {
                return &*it;
            }
        }
        return nullptr;
    }
};

// Builds a table from raw .dynsym and .dynstr bytes (Elf64_Sym or Elf32_Sym entries of
// entry_size bytes). Entries without a size, an address or a name inside strtab are left out.
inline SymbolTable ParseSymbols(std::span<const u8> symtab, std::span<const u8> strtab, bool is_64,
                                u64 entry_size) {
    SymbolTable table;
    const u64 min_size = is_64 ? 24 : 16;
    if (entry_size < min_size) {
        return table;
    }
    for (u64 at = 0; at + min_size <= symtab.size(); at += entry_size) {
        const u8* const sym = symtab.data() + at;
        u32 name{};
        u64 value{};
        u64 size{};
        std::memcpy(&name, sym, 4);
        if (is_64) {
            std::memcpy(&value, sym + 8, 8);
            std::memcpy(&size, sym + 16, 8);
        } else {
            u32 value32{};
            u32 size32{};
            std::memcpy(&value32, sym + 4, 4);
            std::memcpy(&size32, sym + 8, 4);
            value = value32;
            size = size32;
        }
        if (value == 0 || size == 0 || name >= strtab.size() || value + size < value) {
            continue;
        }
        const auto* const text = reinterpret_cast<const char*>(strtab.data()) + name;
        const void* const nul = std::memchr(text, 0, strtab.size() - name);
        const std::size_t length = nul == nullptr ? strtab.size() - name
                                                  : static_cast<std::size_t>(static_cast<const char*>(nul) - text);
        if (length == 0) {
            continue;
        }
        table.entries.push_back({value, value + size, std::string{text, length}});
    }
    std::stable_sort(table.entries.begin(), table.entries.end(),
                     [](const auto& a, const auto& b) { return a.start < b.start; });
    return table;
}

// EDEN_NX2_IPC_TRACE=1: the older per-command log (InvokeRequest / InvokeRequestTipc).
inline bool LogsEveryCommand() {
    const char* text = std::getenv("EDEN_NX2_IPC_TRACE");
    return text != nullptr && std::string_view{text} == "1";
}

// Whether EDEN_NX2_IPC_TRACE holds a filter (read once).
bool Enabled();

// Logs the request's backtrace if it matches the filter and is new. Never throws.
void OnRequest(Core::System& system, std::string_view service, const char* handler,
               HLERequestContext& ctx);

// Whether the filter traces this service's command (OnRequest logs it the first time).
bool Traces(std::string_view service, u32 command);

// At most this many TraceNow backtraces.
constexpr u32 MaxForced = 32;

// Logs the request's backtrace now, whatever the filter (EDEN_NX2_VI2_DIAG: a vi2 command not seen
// before). Never throws.
void TraceNow(Core::System& system, std::string_view service, const char* handler,
              HLERequestContext& ctx);

} // namespace Service::Nx2IpcTrace
