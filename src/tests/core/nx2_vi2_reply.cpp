// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_vi2_reply.h"

namespace {
using namespace Service::Nx2Vi2Reply;
using Clock = std::chrono::steady_clock;

void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << text;
}
} // Anonymous namespace

TEST_CASE("Nx2Vi2Reply: file lines, hex and decimal, comments, empty lists", "[core]") {
    const auto parsed = Parse("# DKB display queries\n"
                              "104: 1\n"
                              "\n"
                              "105:            # empty: success without data words\n"
                              "  107 : 0x3c 0 , 7,0xFFFFFFFF\r\n"
                              "0x105: 4294967295\n"
                              "\t\n"
                              "351:0X10# comment right after a word\n");
    REQUIRE(parsed.table.has_value());
    REQUIRE(parsed.error.empty());
    const Table& table = *parsed.table;
    REQUIRE(table.size() == 5);
    REQUIRE(table.at(104) == std::vector<u32>{1});
    REQUIRE(table.at(105).empty());
    REQUIRE(table.at(107) == std::vector<u32>{0x3c, 0, 7, 0xFFFFFFFF});
    REQUIRE(table.at(0x105) == std::vector<u32>{0xFFFFFFFF});
    REQUIRE(table.at(351) == std::vector<u32>{0x10});
    REQUIRE(Describe(table) == "104=[0x1] 105=[] 107=[0x3c,0x0,0x7,0xffffffff] 261=[0xffffffff] 351=[0x10]");
    REQUIRE(Describe({}) == "(empty)");
    REQUIRE(FormatWords({}) == "[]");

    // Only blank lines and comments: an empty table, not an error.
    const auto empty = Parse("# nothing yet\n\n   \n");
    REQUIRE(empty.table.has_value());
    REQUIRE(empty.table->empty());
    REQUIRE(Parse("").table.has_value());
}

TEST_CASE("Nx2Vi2Reply: invalid lines name the line and give no table", "[core]") {
    const auto check = [](const char* text, const char* error) {
        const auto parsed = Parse(text);
        REQUIRE(!parsed.table.has_value());
        REQUIRE(parsed.error == error);
    };
    check("104: 1\n105 1\n", "line 2: no ':' after the command");
    check("# x\n\nabc: 1\n", "line 3: command \"abc\" is not a number");
    check(": 1\n", "line 1: command \"\" is not a number");
    check("104: 1 2x\n", "line 1: word \"2x\" is not a 32-bit number");
    check("104: 0x100000000\n", "line 1: word \"0x100000000\" is not a 32-bit number");
    check("104: 4294967296\n", "line 1: word \"4294967296\" is not a 32-bit number");
    check("104: -1\n", "line 1: word \"-1\" is not a 32-bit number");
    check("104: 0x\n", "line 1: word \"0x\" is not a 32-bit number");
    check("104: 1\n105:\n0x68: 2\n", "line 3: command 104 is listed twice");
    check("104: 1:2\n", "line 1: word \"1:2\" is not a 32-bit number");
    std::string long_line = "104:";
    for (std::size_t i = 0; i <= MaxWords; ++i) {
        long_line += " 1";
    }
    check(long_line.c_str(), "line 1: more than 32 words");
    long_line.resize(long_line.size() - 2);
    REQUIRE(Parse(long_line).table->at(104).size() == MaxWords);
}

TEST_CASE("Nx2Vi2Reply: inline items", "[core]") {
    const auto parsed = ParseInline("104:1;105:1,0;107:0x3c");
    REQUIRE(parsed.table.has_value());
    REQUIRE(Describe(*parsed.table) == "104=[0x1] 105=[0x1,0x0] 107=[0x3c]");
    REQUIRE(ParseInline("105:").table->at(105).empty());
    REQUIRE(ParseInline(" 104 : 1 ; ; 105:;").table->size() == 2);
    const auto bad = ParseInline("104:1;105=1");
    REQUIRE(!bad.table.has_value());
    REQUIRE(bad.error == "item 2: no ':' after the command");
    REQUIRE(ParseInline("104:1;104:2").error == "item 2: command 104 is listed twice");
}

TEST_CASE("Nx2Vi2Reply: GetHdrStatus's SDR reply (104: u64 format 1, u32 0)", "[core]") {
    const std::vector<u32> sdr{1, 0, 0};
    REQUIRE(ParseInline("104:1,0,0").table->at(104) == sdr);
    REQUIRE(Parse("104: 1 0 0  # GetHdrStatus, SDR\n").table->at(104) == sdr);
}

TEST_CASE("Nx2Vi2Reply: versions and the first call after a change", "[core]") {
    Source source;
    REQUIRE(source.Version() == 0);
    REQUIRE(source.Find(104) == nullptr);
    // A command never in a table is logged on its first call only.
    REQUIRE(source.FirstCallSinceChange(107));
    REQUIRE(!source.FirstCallSinceChange(107));

    auto update = source.Apply({{104, {1}}, {105, {}}});
    REQUIRE(update.kind == Source::Kind::Loaded);
    REQUIRE(update.message == "104=[0x1] 105=[]");
    REQUIRE(source.Version() == 1);
    REQUIRE(*source.Find(104) == std::vector<u32>{1});
    REQUIRE(source.Find(105)->empty());
    REQUIRE(source.FirstCallSinceChange(104));
    REQUIRE(!source.FirstCallSinceChange(104));
    REQUIRE(source.FirstCallSinceChange(105));
    REQUIRE(!source.FirstCallSinceChange(107));

    // The same table: no new version.
    REQUIRE(source.Apply({{104, {1}}, {105, {}}}).kind == Source::Kind::Same);
    REQUIRE(source.Version() == 1);

    // v2: 104 changes, 105 stays, 107 is added.
    update = source.Apply({{104, {2}}, {105, {}}, {107, {0x3c}}});
    REQUIRE(update.kind == Source::Kind::Loaded);
    REQUIRE(source.Version() == 2);
    REQUIRE(source.FirstCallSinceChange(104));
    REQUIRE(!source.FirstCallSinceChange(105));
    REQUIRE(source.FirstCallSinceChange(107));
    REQUIRE(!source.FirstCallSinceChange(107));

    // v3: 104 is removed (back to its default reply), which is a change too.
    source.Apply({{105, {}}, {107, {0x3c}}});
    REQUIRE(source.Version() == 3);
    REQUIRE(source.Find(104) == nullptr);
    REQUIRE(source.FirstCallSinceChange(104));
    REQUIRE(!source.FirstCallSinceChange(105));
    REQUIRE(!source.FirstCallSinceChange(107));

    // A source without a file never polls.
    REQUIRE(source.Poll(Clock::now()).kind == Source::Kind::None);
}

TEST_CASE("Nx2Vi2Reply: hot reload from a file", "[core]") {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "eden_nx2_vi2_reply_test.txt";
    fs::remove(path);
    Source source{path};
    auto t = Clock::now();

    // Missing file: one warning, no table; not repeated while nothing changes.
    auto update = source.Poll(t);
    REQUIRE(update.kind == Source::Kind::Failed);
    REQUIRE(update.message.starts_with("cannot read "));
    REQUIRE(source.Version() == 0);
    t += std::chrono::seconds{2};
    REQUIRE(source.Poll(t).kind == Source::Kind::None);

    // The file appears: checked again after a second, not sooner.
    WriteFile(path, "104: 1\n105:\n");
    REQUIRE(source.Poll(t + std::chrono::milliseconds{500}).kind == Source::Kind::None);
    t += std::chrono::seconds{1};
    update = source.Poll(t);
    REQUIRE(update.kind == Source::Kind::Loaded);
    REQUIRE(update.message == "104=[0x1] 105=[]");
    REQUIRE(source.Version() == 1);

    // Unchanged file: nothing.
    t += std::chrono::seconds{1};
    REQUIRE(source.Poll(t).kind == Source::Kind::None);

    // A broken edit keeps v1 and warns once.
    WriteFile(path, "104: 1\n105 2\n");
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds{1});
    t += std::chrono::seconds{1};
    update = source.Poll(t);
    REQUIRE(update.kind == Source::Kind::Failed);
    REQUIRE(update.message.ends_with("line 2: no ':' after the command"));
    REQUIRE(source.Version() == 1);
    REQUIRE(*source.Find(104) == std::vector<u32>{1});
    t += std::chrono::seconds{1};
    REQUIRE(source.Poll(t).kind == Source::Kind::None);

    // The same mistake saved again is reported again (new contents).
    WriteFile(path, "104: 1\n105 2\n\n");
    t += std::chrono::seconds{1};
    REQUIRE(source.Poll(t).kind == Source::Kind::Failed);

    // Fixed: v2.
    WriteFile(path, "104: 0x2\n105: 1 0\n107: 0x3c\n");
    t += std::chrono::seconds{1};
    update = source.Poll(t);
    REQUIRE(update.kind == Source::Kind::Loaded);
    REQUIRE(source.Version() == 2);
    REQUIRE(*source.Find(105) == std::vector<u32>{1, 0});

    // Saved again with only a comment added: the same table, no new version.
    WriteFile(path, "104: 0x2\n105: 1 0\n107: 0x3c # frame time?\n");
    t += std::chrono::seconds{1};
    REQUIRE(source.Poll(t).kind == Source::Kind::Same);
    REQUIRE(source.Version() == 2);

    // Deleted: a warning, the table stays.
    fs::remove(path);
    t += std::chrono::seconds{1};
    REQUIRE(source.Poll(t).kind == Source::Kind::Failed);
    REQUIRE(source.Version() == 2);
    REQUIRE(source.Find(107) != nullptr);
}

TEST_CASE("Nx2Vi2Reply: byte order marks, UTF-16, tabs, signs, empty files", "[core]") {
    // Notepad's "UTF-8 with BOM": the mark is skipped (Parse alone sees it in the command).
    auto parsed = ParseFile("\xEF\xBB\xBF" "104: 1\n");
    REQUIRE(parsed.table.has_value());
    REQUIRE(*parsed.table == Table{{104, {1}}});
    REQUIRE(!Parse("\xEF\xBB\xBF" "104: 1\n").table.has_value());
    REQUIRE(ParseFile("\xEF\xBB\xBF").table->empty());
    REQUIRE(ParseFile("").table->empty());

    // UTF-16 either way round: refused, saying why.
    for (const std::string text : {std::string{"\xFF\xFE" "1\0" "0\0" "4\0" ":\0" "1\0", 12},
                                   std::string{"\xFE\xFF" "\0" "1" "\0" "0" "\0" "4", 8}}) {
        parsed = ParseFile(text);
        REQUIRE(!parsed.table.has_value());
        REQUIRE(parsed.error == "UTF-16 file, save it as UTF-8");
    }

    // Tabs anywhere a space may be.
    parsed = Parse("104:\t1\t0x2\n\t105\t:\t3\t# tab\n");
    REQUIRE(parsed.table.has_value());
    REQUIRE(*parsed.table == Table{{104, {1, 2}}, {105, {3}}});

    // Signs are not part of a word.
    parsed = Parse("104: +1\n");
    REQUIRE(!parsed.table.has_value());
    REQUIRE(parsed.error == "line 1: word \"+1\" is not a 32-bit number");
    REQUIRE(Parse("104: -1\n").error == "line 1: word \"-1\" is not a 32-bit number");
    REQUIRE(Parse("+104: 1\n").error == "line 1: command \"+104\" is not a number");
}

TEST_CASE("Nx2Vi2Reply: empty, BOM and UTF-16 files on reload", "[core]") {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "eden_nx2_vi2_reply_bom_test.txt";
    WriteFile(path, "");
    Source source{path};
    auto t = Clock::now();

    // A 0 byte file: an empty table, every command an empty success.
    auto update = source.Poll(t);
    REQUIRE(update.kind == Source::Kind::Loaded);
    REQUIRE(update.message == "(empty)");
    REQUIRE(source.Version() == 1);

    WriteFile(path, "\xEF\xBB\xBF" "104: 1\n");
    t += std::chrono::seconds{1};
    update = source.Poll(t);
    REQUIRE(update.kind == Source::Kind::Loaded);
    REQUIRE(update.message == "104=[0x1]");

    // Saved as UTF-16: a clear error, the table stays.
    WriteFile(path, std::string{"\xFF\xFE" "1\0" "0\0" "4\0" ":\0" " \0" "2\0", 14});
    t += std::chrono::seconds{1};
    update = source.Poll(t);
    REQUIRE(update.kind == Source::Kind::Failed);
    REQUIRE(update.message.ends_with(": UTF-16 file, save it as UTF-8"));
    REQUIRE(source.Version() == 2);
    REQUIRE(*source.Find(104) == std::vector<u32>{1});
    fs::remove(path);
}

TEST_CASE("Nx2Vi2Reply: table commands no session answers", "[core]") {
    const Table table{{1, {}}, {104, {1}}, {105, {}}, {300, {2}}, {301, {}}};
    REQUIRE(Unanswered(table, {1, 104, 105, 300, 301}, {}).empty());
    REQUIRE(Unanswered({}, {}, {}).empty());
    REQUIRE(Unanswered(table, {104, 105}, {1}) ==
            "300, 301 not answered: commands get a table handler when a vi2 session opens (restart "
            "the game); 1 not answered: a handler of its own");
    REQUIRE(Unanswered(table, {104, 105, 300, 301}, {1}) == "1 not answered: a handler of its own");
}
