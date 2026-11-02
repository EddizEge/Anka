// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <string>
#include <vector>
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

TEST_CASE("Nx2IpcTrace: data words count only for listed commands", "[core]") {
    auto filter = ParseFilter("nx2:vi2");
    REQUIRE(filter->Classify("nx2:vi2", 229) == MatchKind::Service);
    REQUIRE(filter->Classify("nx2:vic", 229) == MatchKind::None);
    filter = ParseFilter("nx2:vi2;nx2:vi2:104,107");
    REQUIRE(filter->Classify("nx2:vi2", 104) == MatchKind::Command);
    REQUIRE(filter->Classify("nx2:vi2", 229) == MatchKind::Service);
    filter = ParseFilter("nx2:vi2:104");
    REQUIRE(filter->Classify("nx2:vi2", 105) == MatchKind::None);

    Seen seen;
    // Unlisted (229, a different frame number every frame): once.
    REQUIRE(seen.Admit("nx2:vi2", 4, 229, {1, 2}, false));
    REQUIRE(!seen.Admit("nx2:vi2", 4, 229, {1, 3}, false));
    REQUIRE(!seen.Admit("nx2:vi2", 4, 229, {}, false));
    // Listed: each new word list, up to MaxWordVariants.
    for (u32 i = 0; i < MaxWordVariants; ++i) {
        REQUIRE(seen.Admit("nx2:vi2", 4, 104, {i}, true));
        REQUIRE(!seen.Admit("nx2:vi2", 4, 104, {i}, true));
    }
    REQUIRE(!seen.Admit("nx2:vi2", 4, 104, {MaxWordVariants}, true));
    // Other commands, types and services are separate.
    REQUIRE(seen.Admit("nx2:vi2", 4, 105, {0}, true));
    REQUIRE(seen.Admit("nx2:vi2", 5, 229, {1, 2}, false));
    REQUIRE(seen.Admit("nx2:vic", 4, 229, {1, 2}, false));
}

TEST_CASE("Nx2IpcTrace: modules end where the loader says", "[core]") {
    const std::vector<ModuleRange> modules{
        {0x80301000, 0x80321000, "rtld"},
        {0x80321000, 0x82b31000, "main"},
        {0x82b31000, 0x83b21000, "sdk"},
        {0x90000000, 0x90001000, "after a gap"},
    };
    REQUIRE(FindModule(modules, 0x80300fff) == nullptr);
    REQUIRE(FindModule(modules, 0x8030108c)->name == "rtld");
    REQUIRE(FindModule(modules, 0x80320fff)->name == "rtld");
    REQUIRE(FindModule(modules, 0x80321000)->name == "main");
    REQUIRE(FindModule(modules, 0x82cb1d28)->name == "sdk");
    REQUIRE(FindModule(modules, 0x83b21000) == nullptr); // past sdk's end, before the next module
    REQUIRE(FindModule(modules, 0x90000fff)->name == "after a gap");
    REQUIRE(FindModule(modules, 0x90001000) == nullptr);
    REQUIRE(FindModule({}, 0x80301000) == nullptr);
}

namespace {
void PutSym64(std::vector<u8>& symtab, u32 name, u64 value, u64 size) {
    std::array<u8, 24> sym{};
    std::memcpy(sym.data(), &name, 4);
    std::memcpy(sym.data() + 8, &value, 8);
    std::memcpy(sym.data() + 16, &size, 8);
    symtab.insert(symtab.end(), sym.begin(), sym.end());
}
} // Anonymous namespace

TEST_CASE("Nx2IpcTrace: symbol tables from .dynsym and .dynstr", "[core]") {
    const std::string strings = std::string{"\0foo\0bar\0outer\0inner\0import\0empty\0", 35} + "tail";
    const std::span<const u8> strtab{reinterpret_cast<const u8*>(strings.data()), strings.size()};
    std::vector<u8> symtab;
    PutSym64(symtab, 0, 0, 0);          // the null symbol
    PutSym64(symtab, 1, 0x100, 0x20);   // foo
    PutSym64(symtab, 5, 0x80, 0x10);    // bar
    PutSym64(symtab, 9, 0x200, 0x100);  // outer
    PutSym64(symtab, 15, 0x240, 0x10);  // inner, inside outer
    PutSym64(symtab, 21, 0, 0x10);      // an import: no address
    PutSym64(symtab, 28, 0x400, 0);     // no size
    PutSym64(symtab, 999, 0x500, 0x10); // name outside the string table
    PutSym64(symtab, 35, 0x600, 0x10);  // name runs to the table's end
    const auto table = ParseSymbols(symtab, strtab, true, 24);
    REQUIRE(table.entries.size() == 5);
    REQUIRE(table.entries.front().name == "bar");
    REQUIRE(table.Find(0x80)->name == "bar");
    REQUIRE(table.Find(0x8f)->name == "bar");
    REQUIRE(table.Find(0x90) == nullptr);
    REQUIRE(table.Find(0x110)->name == "foo");
    REQUIRE(table.Find(0x120) == nullptr);
    REQUIRE(table.Find(0x245)->name == "inner");
    REQUIRE(table.Find(0x260)->name == "outer");
    REQUIRE(table.Find(0x400) == nullptr);
    REQUIRE(table.Find(0x608)->name == "tail");
    REQUIRE(table.Find(0x7f) == nullptr);

    // A larger entry size (DT_SYMENT) steps over the extra bytes; too small an entry size: nothing.
    std::vector<u8> wide;
    PutSym64(wide, 1, 0x100, 0x20);
    wide.resize(32);
    PutSym64(wide, 5, 0x80, 0x10);
    wide.resize(64);
    REQUIRE(ParseSymbols(wide, strtab, true, 32).entries.size() == 2);
    REQUIRE(ParseSymbols(symtab, strtab, true, 8).entries.empty());
    REQUIRE(ParseSymbols(symtab, {}, true, 24).entries.empty());
    REQUIRE(ParseSymbols({}, strtab, true, 24).entries.empty());

    // Elf32_Sym: name, value, size, info, other, shndx.
    std::array<u8, 16> sym32{};
    const u32 name = 5, value = 0x1000, size = 8;
    std::memcpy(sym32.data(), &name, 4);
    std::memcpy(sym32.data() + 4, &value, 4);
    std::memcpy(sym32.data() + 8, &size, 4);
    const auto table32 = ParseSymbols(sym32, strtab, false, 16);
    REQUIRE(table32.entries.size() == 1);
    REQUIRE(table32.Find(0x1007)->name == "bar");
    REQUIRE(table32.Find(0x1008) == nullptr);
}
