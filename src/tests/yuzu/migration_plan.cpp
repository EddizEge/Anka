// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "yuzu/migration_plan.h"

namespace {

namespace fs = std::filesystem;
using namespace MigrationPlan;

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("anka_migration_plan_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void Touch(const fs::path& file) {
    fs::create_directories(file.parent_path());
    std::ofstream{file} << "x";
}

constexpr const char* ProfilesDat = "nand/system/save/8000000000000010/su/avators/profiles.dat";

} // Anonymous namespace

TEST_CASE("Migration: the radio buttons map to the strategies they name", "[yuzu][migration]") {
    // Anka #6: ids 1, 2, 3 made "Keep Old Data" a move and "Clear Old Data" a link
    REQUIRE(FromButton(0, false) == Strategy::Copy);
    REQUIRE(FromButton(1, false) == Strategy::Move);
    REQUIRE(FromButton(2, false) == Strategy::Link);
    // None checked, or an id past the enum: a copy
    REQUIRE(FromButton(-1, false) == Strategy::Copy);
    REQUIRE(FromButton(3, false) == Strategy::Copy);
}

TEST_CASE("Migration: portable offers only a copy, the shader cache kept", "[yuzu][migration]") {
    REQUIRE(ChoicesFor(true) == Choices{false, false, false});
    REQUIRE(ChoicesFor(false) == Choices{true, true, true});
    for (int id = -1; id <= 3; ++id) {
        REQUIRE(FromButton(id, true) == Strategy::Copy);
    }
}

TEST_CASE("Migration: an empty or missing data folder may be replaced", "[yuzu][migration]") {
    TempDir t;
    REQUIRE(Classify(t.path / "missing") == Target::Missing);
    REQUIRE(Classify(t.path) == Target::Empty);
    fs::create_directories(t.path / "keys");
    fs::create_directories(t.path / "nand" / "system");
    REQUIRE(Classify(t.path) == Target::Empty);
    REQUIRE(MayReplace(Target::Missing));
    REQUIRE(MayReplace(Target::Empty));
}

TEST_CASE("Migration: the first-run profiles.dat alone may be replaced", "[yuzu][migration]") {
    TempDir t;
    Touch(t.path / ProfilesDat);
    REQUIRE(Classify(t.path) == Target::FirstRunOnly);
    REQUIRE(MayReplace(Target::FirstRunOnly));
}

TEST_CASE("Migration: a data folder with the user's files is never replaced", "[yuzu][migration]") {
    for (const char* file : {"keys/prod.keys", "nand/user/save/0000000000000000/1/a.bin",
                             "sdmc/atmosphere/contents/x", "config/qt-config.ini",
                             "nand/system/save/8000000000000010/su/avators/other.jpg",
                             "profiles.dat"}) {
        TempDir t;
        Touch(t.path / ProfilesDat);
        Touch(t.path / file);
        INFO(file);
        REQUIRE(Classify(t.path) == Target::HasData);
    }
    REQUIRE_FALSE(MayReplace(Target::HasData));
    REQUIRE_FALSE(MayReplace(Target::Unreadable));
}

TEST_CASE("Migration: files named in any script are read, not thrown on", "[yuzu][migration]") {
    // Japanese and an emoji: no ANSI code page has them (generic_string() threw)
    for (const char16_t* file : {u"日本/セーブ.bin", u"mods/\U0001F34C.txt"}) {
        TempDir t;
        Touch(t.path / ProfilesDat);
        Touch(t.path / fs::path{file});
        REQUIRE(Classify(t.path) == Target::HasData);
    }
    REQUIRE(IsFirstRunFile(fs::path{ProfilesDat}));
    REQUIRE_FALSE(IsFirstRunFile(fs::path{u"nand/日本/profiles.dat"}));
}

TEST_CASE("Migration: a file where the data folder should be is not removed", "[yuzu][migration]") {
    TempDir t;
    Touch(t.path / "user");
    REQUIRE(Classify(t.path / "user") == Target::HasData);
}
