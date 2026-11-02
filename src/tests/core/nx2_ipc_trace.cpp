// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_ipc_trace.h"

namespace {
using namespace Service::Nx2IpcTrace;
} // Anonymous namespace

TEST_CASE("Nx2IpcTrace: filter items", "[core]") {
    auto filter = ParseFilter("nx2:vi2");
    REQUIRE(filter.has_value());
    REQUIRE(filter->items.size() == 1);
    REQUIRE(filter->items[0].prefix == "nx2:vi2");
    REQUIRE(filter->items[0].commands.empty());
    REQUIRE(filter->Matches("nx2:vi2", 104));
    REQUIRE(filter->Matches("nx2:vi2", 229));
    REQUIRE(!filter->Matches("nx2:vic", 104));
    REQUIRE(!filter->Matches("nx2:vi", 104));

    filter = ParseFilter("nx2:vi2:104,105, 0x6b");
    REQUIRE(filter.has_value());
    REQUIRE(filter->items[0].prefix == "nx2:vi2");
    REQUIRE(filter->items[0].commands == std::vector<u32>{104, 105, 107});
    REQUIRE(filter->Matches("nx2:vi2", 107));
    REQUIRE(!filter->Matches("nx2:vi2", 229));

    filter = ParseFilter(" all ");
    REQUIRE(filter->Matches("hid", 1));
    REQUIRE(filter->Matches("nx2:vi2", 104));

    // Several items; a prefix ending in ':' and one whose tail is no command list.
    filter = ParseFilter("nx2:;am:1;nx2:vi2:abc;;");
    REQUIRE(filter->items.size() == 3);
    REQUIRE(filter->items[0].prefix == "nx2:");
    REQUIRE(filter->items[1].prefix == "am");
    REQUIRE(filter->items[1].commands == std::vector<u32>{1});
    REQUIRE(filter->items[2].prefix == "nx2:vi2:abc");
    REQUIRE(filter->Matches("nx2:gsv", 9));
    REQUIRE(filter->Matches("am", 1));
    REQUIRE(filter->Matches("amx", 1));
    REQUIRE(!filter->Matches("am", 2));
    REQUIRE(!filter->Matches("hid", 1));

    // "104,x": not a command list, so the whole item is a prefix.
    filter = ParseFilter("nx2:vi2:104,x");
    REQUIRE(filter->items[0].prefix == "nx2:vi2:104,x");
    REQUIRE(filter->items[0].commands.empty());

    REQUIRE(!ParseFilter(""));
    REQUIRE(!ParseFilter(" ; ;"));
    REQUIRE(ParseNumber("0x68") == 104u);
    REQUIRE(!ParseNumber("68h"));
}

TEST_CASE("Nx2IpcTrace: request data words", "[core]") {
    // TIPC 104 with the display name "default" (2 words), no handle descriptor.
    std::array<u32, 64> buffer{};
    buffer[0] = 104 + 16;
    buffer[1] = 2;
    buffer[2] = 0x61666564;
    buffer[3] = 0x746c75;
    buffer[4] = 0xdeadbeef; // stale
    REQUIRE(RequestDataWords(buffer, true, 0, false) == std::vector<u32>{0x61666564, 0x746c75});

    // TIPC with a handle descriptor: pid (2 words) and one copied handle come first.
    buffer = {};
    buffer[0] = 261 + 16;
    buffer[1] = 0x80000000U | 3;
    buffer[2] = 1 | (1 << 1);
    buffer[3] = 0x11;
    buffer[4] = 0x22;
    buffer[5] = 0xabcd; // handle
    buffer[6] = 7;
    buffer[7] = 8;
    buffer[8] = 9;
    REQUIRE(RequestDataWords(buffer, true, 0, false) == std::vector<u32>{7, 8, 9});

    // TIPC with one A buffer descriptor (3 words) before the data.
    buffer = {};
    buffer[0] = (1 << 20) | (5 + 16);
    buffer[1] = 1;
    buffer[5] = 42;
    REQUIRE(RequestDataWords(buffer, true, 0, false) == std::vector<u32>{42});

    // CMIF: header, 2 padding words to 16 bytes, 'SFCI', version, command (u64), 2 arguments,
    // then the rest of the 16 alignment bytes.
    buffer = {};
    buffer[0] = 4;
    buffer[1] = 4 + 4 + 2;
    buffer[4] = 0x49434653;
    buffer[6] = 1010;
    buffer[8] = 5;
    buffer[9] = 6;
    REQUIRE(RequestDataWords(buffer, false, 6, false) == std::vector<u32>{5, 6});
    // A CMIF command without arguments.
    buffer[1] = 8;
    REQUIRE(RequestDataWords(buffer, false, 6, false).empty());

    // Never past the buffer, never more than MaxDataWords.
    buffer = {};
    buffer[0] = 16;
    buffer[1] = 0x3FF;
    REQUIRE(RequestDataWords(buffer, true, 0, false).size() == MaxDataWords);
    REQUIRE(RequestDataWords(std::span{buffer}.first(10), true, 0, false).size() == 8);
    REQUIRE(RequestDataWords(std::span{buffer}.first(1), true, 0, false).empty());
    buffer[1] = 0x80000000U | 4;
    buffer[2] = 0xFFFFFFFF; // 2 + 15 + 15 words of handles
    REQUIRE(RequestDataWords(std::span{buffer}.first(36), true, 0, false).size() == 1);
    REQUIRE(RequestDataWords(std::span{buffer}.first(2), true, 0, false).empty());
}
