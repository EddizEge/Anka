// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_vi2_events.h"

namespace {
using namespace Service::Nx2Vi2Events;

KindModes Modes(KindMode k1, KindMode k2, KindMode k3) {
    KindModes modes;
    modes.modes = {KindMode::Vsync, k1, k2, k3};
    return modes;
}
} // Anonymous namespace

TEST_CASE("Nx2Vi2Events: kind event switch, the short forms", "[core]") {
    const auto once = ParseKindEvents("1");
    REQUIRE(once.modes.has_value());
    REQUIRE(*once.modes == Modes(KindMode::Once, KindMode::Once, KindMode::Once));
    REQUIRE(once.modes->Of(0) == KindMode::Vsync);
    REQUIRE(once.modes->AnyChanged());
    const auto never = ParseKindEvents(" 0 ");
    REQUIRE(never.modes.has_value());
    REQUIRE(*never.modes == Modes(KindMode::Never, KindMode::Never, KindMode::Never));
    REQUIRE(DescribeKinds(*never.modes) == "1:never 2:never 3:never");
}

TEST_CASE("Nx2Vi2Events: kind event switch, per kind", "[core]") {
    const auto parsed = ParseKindEvents("1:once,2:never,3:vsync");
    REQUIRE(parsed.modes.has_value());
    REQUIRE(*parsed.modes == Modes(KindMode::Once, KindMode::Never, KindMode::Vsync));
    REQUIRE(DescribeKinds(*parsed.modes) == "1:once 2:never 3:vsync");
    // Unlisted kinds stay on the vblanks; ';' and blanks work too.
    const auto two = ParseKindEvents(" 2 : once ; ");
    REQUIRE(two.modes.has_value());
    REQUIRE(*two.modes == Modes(KindMode::Vsync, KindMode::Once, KindMode::Vsync));
    // All vsync: valid, nothing changed.
    const auto same = ParseKindEvents("1:vsync");
    REQUIRE(same.modes.has_value());
    REQUIRE(!same.modes->AnyChanged());
    // Out of range kinds stay on the vblanks.
    REQUIRE(parsed.modes->Of(4) == KindMode::Vsync);
    REQUIRE(parsed.modes->Of(0xffffffffU) == KindMode::Vsync);
}

TEST_CASE("Nx2Vi2Events: kind event switch errors", "[core]") {
    for (const char* text : {"", "  ", "2", "yes", "0:once", "4:once", "1:sometimes", "1:once,1:never",
                             "1once", "x:once", "1:", "-1:once"}) {
        const auto parsed = ParseKindEvents(text);
        REQUIRE(!parsed.modes.has_value());
        REQUIRE(!parsed.error.empty());
    }
}

TEST_CASE("Nx2Vi2Events: known commands", "[core]") {
    for (const u32 command : {1U, 3U, 5U, 11U, 21U, 31U, 32U, 51U, 101U, 104U, 105U, 107U, 111U, 201U,
                              212U, 229U, 251U, 261U, 265U, 267U, 271U, 351U}) {
        REQUIRE(IsKnownCommand(command));
    }
    // Handled but never seen from Bananza, or not handled at all.
    for (const u32 command : {0U, 2U, 13U, 102U, 103U, 106U, 112U, 202U, 228U, 252U, 262U, 1000U}) {
        REQUIRE(!IsKnownCommand(command));
    }
}

TEST_CASE("Nx2Vi2Events: REPLY_OWN command list", "[core]") {
    const auto parsed = ParseCommandList("1,3,101");
    REQUIRE(parsed.error.empty());
    REQUIRE(parsed.commands == std::set<u32>{1, 3, 101});
    REQUIRE(parsed.refused.empty());
    const auto mixed = ParseCommandList(" 0x65 ; 5, 31,229 ,11,21,32");
    REQUIRE(mixed.error.empty());
    REQUIRE(mixed.commands == std::set<u32>{5, 101});
    REQUIRE(mixed.refused == std::set<u32>{11, 21, 31, 32, 229});
    for (const char* text : {"", " , ", "1,x", "1;;abc", "0x", "-3"}) {
        const auto bad = ParseCommandList(text);
        REQUIRE(!bad.error.empty());
        REQUIRE(bad.commands.empty());
    }
    REQUIRE(OwnReplyFixed(11));
    REQUIRE(OwnReplyFixed(229));
    REQUIRE(!OwnReplyFixed(101));
    REQUIRE(!OwnReplyFixed(1));
}

TEST_CASE("Nx2Vi2Events: TIPC message words", "[core]") {
    // Reply of TipcReplyWords(ctx, {0x7}): header (type, 2 data words), result, the word.
    const std::array<u32, 4> plain{0x10, 0x2, 0x0, 0x7};
    REQUIRE(TipcWords(plain) == std::vector<u32>{0x0, 0x7});
    // Reply with one copied handle (11/31/32): descriptor at 2, the handle at 3, then the result.
    const std::array<u32, 5> handle{0x1f, 0x80000001U, 0x2, 0xabcd, 0x0};
    REQUIRE(TipcWords(handle) == std::vector<u32>{0x0});
    // A process id moves the data two words on.
    const std::array<u32, 8> pid{0x10, 0x80000002U, 0x3, 0x11, 0x12, 0x99, 0x0, 0x5};
    REQUIRE(TipcWords(pid) == std::vector<u32>{0x0, 0x5});
    // Truncated buffers and the word limit.
    REQUIRE(TipcWords(std::span<const u32>{plain.data(), 1}).empty());
    REQUIRE(TipcWords(std::span<const u32>{plain.data(), 3}) == std::vector<u32>{0x0});
    const std::array<u32, 6> many{0x10, 0x4, 1, 2, 3, 4};
    REQUIRE(TipcWords(many, 2) == std::vector<u32>{1, 2});
    const std::array<u32, 2> flag_only{0x10, 0x80000001U};
    REQUIRE(TipcWords(flag_only).empty());
}

TEST_CASE("Nx2Vi2Events: VRR vsync switch", "[core]") {
    const auto standard = ParseVrrVsync("1");
    REQUIRE(standard.rates.has_value());
    REQUIRE(standard.rates->max_hz == 120);
    REQUIRE(standard.rates->min_hz == 40);
    const auto custom = ParseVrrVsync(" 144:48 ");
    REQUIRE(custom.rates.has_value());
    REQUIRE(custom.rates->max_hz == 144);
    REQUIRE(custom.rates->min_hz == 48);
    for (const char* text : {"", "0", "2", "120", "120:", ":40", "40:120", "120:120", "120:29", "481:40",
                             "a:b"}) {
        const auto parsed = ParseVrrVsync(text);
        REQUIRE(!parsed.rates.has_value());
        REQUIRE(!parsed.error.empty());
    }
}

TEST_CASE("Nx2Vi2Events: VRR vsync clock", "[core]") {
    auto clock = VrrVsyncClock::FromRates(VrrVsyncRates{});
    REQUIRE(clock.min_ns == 8'333'333);
    REQUIRE(clock.max_ns == 25'000'000);
    // Nothing scanned yet: a present scans at once, the timer too.
    REQUIRE(clock.PresentDue(1'000) == 1'000);
    REQUIRE(clock.DeadlineDue(1'000));
    REQUIRE(clock.NextDeadlineFrom(1'000) == 25'000'000);
    clock.Scanned(100'000'000);
    // 1/120 s at the soonest.
    REQUIRE(clock.PresentDue(100'000'000) == 108'333'333);
    REQUIRE(clock.PresentDue(105'000'000) == 108'333'333);
    REQUIRE(clock.PresentDue(108'333'333) == 108'333'333);
    REQUIRE(clock.PresentDue(116'700'000) == 116'700'000);
    // 1/40 s at the latest.
    REQUIRE(!clock.DeadlineDue(124'999'999));
    REQUIRE(clock.DeadlineDue(125'000'000));
    REQUIRE(clock.DeadlineDue(200'000'000));
    // The bottom rate timer fired early (a present scanned in between): wait for the rest.
    REQUIRE(clock.NextDeadlineFrom(110'000'000) == 15'000'000);
    REQUIRE(clock.NextDeadlineFrom(125'000'000) == 1'000);
    REQUIRE(clock.NextDeadlineFrom(130'000'000) == 1'000);
    // Present at 60 fps: a scan per present, the timer never due.
    s64 now = 200'000'000;
    for (int i = 0; i < 10; ++i) {
        REQUIRE(clock.PresentDue(now) == now);
        clock.Scanned(now);
        now += 16'666'667;
        REQUIRE(!clock.DeadlineDue(now));
    }
    // 45 fps: still a scan per present.
    clock.Scanned(now);
    for (int i = 0; i < 10; ++i) {
        now += 22'222'222;
        REQUIRE(clock.PresentDue(now) == now);
        REQUIRE(!clock.DeadlineDue(now));
        clock.Scanned(now);
    }
    // 30 fps: the panel scans by itself 25 ms after each present.
    clock.Scanned(now);
    REQUIRE(clock.DeadlineDue(now + 25'000'000));
    clock.Scanned(now + 25'000'000);
    REQUIRE(clock.PresentDue(now + 33'333'333) == now + 33'333'333);
    // A present 2 ms after the panel's own scan waits for 1/120 s from it.
    REQUIRE(clock.PresentDue(now + 27'000'000) == now + 25'000'000 + 8'333'333);
    // Custom rates.
    const auto fast = VrrVsyncClock::FromRates(VrrVsyncRates{144, 48});
    REQUIRE(fast.min_ns == 6'944'444);
    REQUIRE(fast.max_ns == 20'833'333);
}

TEST_CASE("Nx2Vi2Events: number parsing", "[core]") {
    REQUIRE(ParseNumber("101") == 101U);
    REQUIRE(ParseNumber(" 0x65 ") == 0x65U);
    REQUIRE(ParseNumber("0X3ff") == 0x3ffU);
    REQUIRE(ParseNumber("4294967295") == 0xffffffffU);
    REQUIRE(!ParseNumber("4294967296").has_value());
    REQUIRE(!ParseNumber("").has_value());
    REQUIRE(!ParseNumber("0x").has_value());
    REQUIRE(!ParseNumber("12a").has_value());
    REQUIRE(!ParseNumber("-1").has_value());
    REQUIRE(SplitItems(" a, b ;;c ,") == std::vector<std::string_view>{"a", "b", "c"});
}
