// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <iterator>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "yuzu_cmd/nx2_auto_script.h"

using namespace Nx2AutoScript;

TEST_CASE("Nx2AutoScript: keys, sticks and comments", "[yuzu_cmd]") {
    const Script script = ParseScript("# menu\n"
                                      "20 A 0.2   # continue\n"
                                      "\n"
                                      "  21.5\tplus 0.1\r\n"
                                      "22 zl 1\n"
                                      "23 Down 0.05\n"
                                      "30 stick L 0 1 2.5\n"
                                      "31 STICK r -1 -0.5 1\n");
    REQUIRE(script.errors.empty());
    REQUIRE(script.anchor == Anchor::Present);
    REQUIRE(script.events.size() == 6);
    const Event& a = script.events[0];
    REQUIRE((a.start == 20.0 && a.duration == 0.2 && !a.stick && a.id == 0 && a.name == "A"));
    REQUIRE((script.events[1].id == 10 && script.events[1].name == "Plus" &&
             script.events[1].start == 21.5));
    REQUIRE((script.events[2].id == 8 && script.events[2].name == "ZL"));
    REQUIRE((script.events[3].id == 15 && script.events[3].duration == 0.05));
    const Event& left = script.events[4];
    REQUIRE((left.stick && left.id == 0 && left.x == 0.0f && left.y == 1.0f &&
             left.duration == 2.5 && left.name == "stick L"));
    const Event& right = script.events[5];
    REQUIRE((right.stick && right.id == 1 && right.x == -1.0f && right.y == -0.5f));
}

TEST_CASE("Nx2AutoScript: every key name", "[yuzu_cmd]") {
    const Script script = ParseScript("0 A 1\n0 B 1\n0 X 1\n0 Y 1\n0 L 1\n0 R 1\n0 ZL 1\n0 ZR 1\n"
                                      "0 Plus 1\n0 Minus 1\n0 Left 1\n0 Up 1\n0 Right 1\n0 Down 1\n");
    REQUIRE(script.errors.empty());
    const int ids[] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    REQUIRE(script.events.size() == std::size(ids));
    for (size_t i = 0; i < script.events.size(); ++i) {
        REQUIRE(script.events[i].id == ids[i]);
    }
}

TEST_CASE("Nx2AutoScript: bad lines are reported and skipped", "[yuzu_cmd]") {
    const Script script = ParseScript("1 A 0.1\n"
                                      "2 Home 0.1\n"        // 2 unknown key
                                      "3 A\n"               // 3 no held time
                                      "4 A 0\n"             // 4 held time 0
                                      "-1 A 0.1\n"          // 5 negative time
                                      "5,5 A 0.1\n"         // 6 decimal comma
                                      "6 A 0.1 extra\n"     // 7 extra word
                                      "7 stick L 0 1\n"     // 8 no held time
                                      "8 stick C 0 1 1\n"   // 9 unknown stick
                                      "9 stick R 1.5 0 1\n" // 10 out of range
                                      "10 stick R x 0 1\n"  // 11 not a number
                                      "nan A 1\n"           // 12 not finite
                                      "inf A 1\n"           // 13
                                      "11 B -2\n"           // 14 negative held time
                                      "12 B 0.3\n");
    REQUIRE(script.events.size() == 2);
    REQUIRE((script.events[0].start == 1.0 && script.events[1].start == 12.0 &&
             script.events[1].name == "B"));
    REQUIRE(script.errors.size() == 13);
    REQUIRE(script.errors[0].rfind("line 2: unknown key", 0) == 0);
    REQUIRE(script.errors[0].find("'2 Home 0.1'") != std::string::npos);
    REQUIRE(script.errors[1].rfind("line 3: ", 0) == 0);
    REQUIRE(script.errors[12].rfind("line 14: ", 0) == 0);
}

TEST_CASE("Nx2AutoScript: anchors", "[yuzu_cmd]") {
    REQUIRE(ParseScript("").anchor == Anchor::Present);
    REQUIRE(ParseScript("").events.empty());
    REQUIRE(ParseScript("anchor boot\n1 A 1\n").anchor == Anchor::Boot);
    REQUIRE(ParseScript("1 A 1\nANCHOR Present # late is fine\n").anchor == Anchor::Present);
    REQUIRE(ParseScript("1 A 1\nanchor boot\n").anchor == Anchor::Boot);
    REQUIRE(ParseScript("anchor Album\n2 A 0.3\n").anchor == Anchor::Album);
    REQUIRE(std::string_view{AnchorName(Anchor::Album)} == "album");
    REQUIRE(std::string_view{AnchorName(Anchor::Present)} == "present");
    REQUIRE(std::string_view{AnchorName(Anchor::Boot)} == "boot");

    const Script twice = ParseScript("anchor boot\nanchor present\n");
    REQUIRE(twice.anchor == Anchor::Boot);
    REQUIRE(twice.errors.size() == 1);
    REQUIRE(twice.errors[0].rfind("line 2: a second anchor line", 0) == 0);

    const Script bad = ParseScript("anchor start\nanchor\nanchor boot now\n");
    REQUIRE(bad.anchor == Anchor::Present);
    REQUIRE(bad.errors.size() == 3);
}

TEST_CASE("Nx2AutoScript: schedule order", "[yuzu_cmd]") {
    const Script script = ParseScript("5 B 1\n"
                                      "1 A 1\n" // A 1..2
                                      "2 A 1\n" // A again at 2: released first
                                      "1 stick L 0 1 0.5\n");
    const auto actions = Schedule(script);
    REQUIRE(actions.size() == 8);
    for (size_t i = 1; i < actions.size(); ++i) {
        REQUIRE(actions[i - 1].time <= actions[i].time);
    }
    REQUIRE((actions[0].time == 1.0 && actions[0].down && actions[0].event == 1));
    REQUIRE((actions[1].time == 1.0 && actions[1].down && actions[1].event == 3));
    REQUIRE((actions[2].time == 1.5 && !actions[2].down && actions[2].event == 3));
    REQUIRE((actions[3].time == 2.0 && !actions[3].down && actions[3].event == 1));
    REQUIRE((actions[4].time == 2.0 && actions[4].down && actions[4].event == 2));
    REQUIRE((actions[7].time == 6.0 && !actions[7].down && actions[7].event == 0));
}

TEST_CASE("Nx2AutoScript: exit switch", "[yuzu_cmd]") {
    const ExitSpec plain = ParseExit("150");
    REQUIRE((plain.enabled && plain.seconds == 150.0 && plain.anchor == Anchor::Boot &&
             plain.error.empty()));
    const ExitSpec present = ParseExit(" 1560.5@present ");
    REQUIRE((present.enabled && present.seconds == 1560.5 && present.anchor == Anchor::Present));
    const ExitSpec boot = ParseExit("90@BOOT");
    REQUIRE((boot.enabled && boot.anchor == Anchor::Boot));
    const ExitSpec album = ParseExit("150@album");
    REQUIRE((album.enabled && album.seconds == 150.0 && album.anchor == Anchor::Album));

    REQUIRE((!ParseExit("").enabled && ParseExit("").error.empty()));
    REQUIRE((!ParseExit("0").enabled && ParseExit("0").error.empty()));
    REQUIRE((!ParseExit("0@present").enabled && ParseExit("0@present").error.empty()));
    for (const char* bad : {"-5", "abc", "10s", "10@later", "10 20", "@present", "1e999"}) {
        const ExitSpec spec = ParseExit(bad);
        REQUIRE(!spec.enabled);
        REQUIRE(!spec.error.empty());
    }
}
