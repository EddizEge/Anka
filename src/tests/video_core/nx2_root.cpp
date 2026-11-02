// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/ir_opt/nx2_root.h"

namespace {
using namespace Shader::Optimization::Nx2Root;
} // Anonymous namespace

TEST_CASE("Nx2Root: the blit block's words", "[video_core]") {
    for (const u32 offset : {0x0U, 0x8U, 0x1cU, 0x30U, 0x48U, 0x50U, 0x5cU}) {
        REQUIRE(InBlitBlock(offset));
    }
    for (const u32 offset : {0x20U, 0x2cU, 0x4cU, 0x60U, 0x100U}) {
        REQUIRE(!InBlitBlock(offset));
    }
    for (const u32 offset : {0x0U, 0x1cU, 0x38U, 0x3cU, 0x58U, 0x5cU}) {
        REQUIRE(NonzeroBlitWord(offset));
    }
    for (const u32 offset : {0x30U, 0x34U, 0x40U, 0x48U, 0x50U, 0x54U}) {
        REQUIRE(!NonzeroBlitWord(offset));
    }
}

TEST_CASE("Nx2Root: which programs read a root table", "[video_core]") {
    // A fullscreen blit: texel size, size, UV clamp.
    Reads blit;
    blit.Add(0x0, true);
    blit.Add(0x8, false);
    blit.Add(0x38, true);
    REQUIRE(!blit.ReadsTable());
    REQUIRE(blit.outside == 0);
    // Material constants past the block: kept, offsets remembered.
    Reads material;
    material.Add(0x0, false);
    material.Add(0x80, false);
    material.Add(0xf0, true);
    REQUIRE(material.ReadsTable());
    REQUIRE(material.outside == 2);
    REQUIRE(material.offset_count == 2);
    REQUIRE(material.offsets[0] == 0x80);
    REQUIRE(material.offsets[1] == 0xf0);
    // A pair whose second word leaves the block.
    Reads straddle;
    straddle.Add(0x1c, true);
    REQUIRE(straddle.ReadsTable());
    // Only words the blit leaves zero (Fortune's Weave's health gauges read only 0x40).
    Reads zero_only;
    zero_only.Add(0x40, false);
    REQUIRE(zero_only.ReadsTable());
    REQUIRE(zero_only.outside == 0);
    // No immediate reads: nothing to keep (register offsets are never replaced).
    Reads none;
    none.indirect = 3;
    REQUIRE(!none.ReadsTable());
    // At most 8 offsets.
    Reads many;
    for (u32 i = 0; i < 12; ++i) {
        many.Add(0x100 + i * 4, false);
    }
    REQUIRE(many.outside == 12);
    REQUIRE(many.offset_count == 8);
}
