// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/native_qmd.h"

namespace {
using Tegra::NativeQmdScheduler;
using Descriptor = NativeQmdScheduler::Descriptor;

/// A QMD 3.0 with a grid (words 12-14) and one release (slot 24: address, flags, payload).
Descriptor Plain(u32 payload) {
    Descriptor qmd{};
    qmd[12] = 4;
    qmd[13] = 2;
    qmd[14] = 1;
    qmd[18] = 0x30;
    qmd[24] = 0x1000;                          // release address, low
    qmd[25] = (1U << 30) | (1U << 23) | 0x81; // write release, address high 0x81
    qmd[26] = payload;
    return qmd;
}

/// DK Bananza's second layout: words 38-43 first (2-7), words 2-37 six words later.
Descriptor Shifted(const Descriptor& plain) {
    Descriptor qmd = plain;
    for (std::size_t i = 0; i < 6; ++i) qmd[2 + i] = plain[38 + i];
    for (std::size_t i = 2; i < 38; ++i) qmd[i + 6] = plain[i];
    return qmd;
}

struct Released {
    u64 address;
    u32 payload;
};

std::string_view Run(u64 address, const Descriptor& qmd, std::vector<Released>& released) {
    NativeQmdScheduler scheduler;
    return scheduler.Signal(
        address, 2,
        [&](u64, Descriptor& out) {
            out = qmd;
            return true;
        },
        [&](u64 destination, u32 payload) {
            released.push_back({destination, payload});
            return true;
        });
}
} // Anonymous namespace

TEST_CASE("NativeQmd: the shifted layout releases like the plain one, without a dispatch",
          "[video_core]") {
    std::vector<Released> released;
    // The plain layout with a grid needs a dispatch (none here).
    REQUIRE(Run(0x10000, Plain(7), released) == "native shader dispatch required");
    REQUIRE(released.empty());
    // The shifted one: version word at 24, moved back, grid cleared, its release made.
    const Descriptor shifted = Shifted(Plain(9));
    REQUIRE((shifted[18] & 0xff) != 0x30);
    REQUIRE((shifted[24] & 0xff) == 0x30);
    REQUIRE(Run(0x20000, shifted, released).empty());
    REQUIRE(released.size() == 1);
    REQUIRE(released[0].address == ((u64{0x81} << 32) | 0x1000));
    REQUIRE(released[0].payload == 9);
    // Neither layout's version word: rejected as before.
    Descriptor wrong = Plain(1);
    wrong[18] = 0x20;
    released.clear();
    REQUIRE(Run(0x30000, wrong, released) == "unsupported QMD version or queue");
    REQUIRE(released.empty());
}
