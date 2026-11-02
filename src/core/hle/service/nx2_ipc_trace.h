// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <charconv>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
// The value "1" keeps the older meaning (log every dispatched command, no backtraces).
namespace Service::Nx2IpcTrace {

// At most this many requests are traced; then "trace limit reached" is logged once.
constexpr u32 MaxTraced = 400;
// Frames logged per request.
constexpr u32 MaxFrames = 32;
// Request data words that make a request "new" (and are logged).
constexpr u32 MaxDataWords = 16;

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

struct Filter {
    std::vector<FilterItem> items;

    bool Matches(std::string_view service, u32 command) const {
        for (const auto& item : items) {
            if (!service.starts_with(item.prefix)) {
                continue;
            }
            if (item.commands.empty()) {
                return true;
            }
            for (const u32 c : item.commands) {
                if (c == command) {
                    return true;
                }
            }
        }
        return false;
    }
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

} // namespace Service::Nx2IpcTrace
