// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_stage_bind.h"

namespace {
using VideoCommon::Nx2Stages::StagedBind;
const int memory_a{};
const int memory_b{};
} // Anonymous namespace

TEST_CASE("Nx2StageBind: the first bind of a session joins, the same channel again does not",
          "[video_core]") {
    StagedBind bind;
    REQUIRE(bind.Join(0, 3, &memory_a, true));
    REQUIRE(!bind.Join(0, 3, &memory_a, true));
    REQUIRE(!bind.Join(0, 3, &memory_a, true));
    // Staged by InstallProxy already: the first bind still joins (InitChannel may have bound the
    // real rasterizer after it).
    StagedBind fresh;
    REQUIRE(fresh.Join(0, 3, &memory_a, true));
}

TEST_CASE("Nx2StageBind: a channel change joins both ways", "[video_core]") {
    StagedBind bind;
    REQUIRE(bind.Join(0, 1, &memory_a, true));
    REQUIRE(bind.Join(0, 2, &memory_a, true));
    REQUIRE(!bind.Join(0, 2, &memory_a, true));
    REQUIRE(bind.Join(0, 1, &memory_a, true));
    REQUIRE(!bind.Join(0, 1, &memory_a, true));
}

TEST_CASE("Nx2StageBind: a new session, address space or an unstaged channel joins",
          "[video_core]") {
    StagedBind bind;
    REQUIRE(bind.Join(4, 1, &memory_a, true));
    REQUIRE(bind.Join(5, 1, &memory_a, true));
    REQUIRE(!bind.Join(5, 1, &memory_a, true));
    REQUIRE(bind.Join(5, 1, &memory_b, true));
    REQUIRE(!bind.Join(5, 1, &memory_b, true));
    REQUIRE(bind.Join(5, 1, &memory_b, false));
    REQUIRE(bind.Join(5, 1, &memory_b, false));
    REQUIRE(!bind.Join(5, 1, &memory_b, true));
}
