// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_present_crop.h"

namespace {

using namespace Service::Nx2PresentCrop;

constexpr u64 Bananza = 0x040074A01BF12000ULL;
constexpr u64 Ocarina = 0x0400486024DEC000ULL;

} // Anonymous namespace

TEST_CASE("Nx2PresentCrop: Donkey Kong Bananza handheld shows the top-left 1920x1080",
          "[core]") {
    // Logged by Bananza in handheld: layout 2880x1620
    REQUIRE(Choose(2880, 1620, false, Bananza) == Crop{1920, 1080});
}

TEST_CASE("Nx2PresentCrop: docked Bananza shows the whole image, as in Anka 1.0.1", "[core]") {
    REQUIRE(Choose(2880, 1620, true, Bananza) == Crop{2880, 1620});
    REQUIRE(Choose(3840, 2160, true, Bananza) == Crop{3840, 2160});
    REQUIRE(Choose(1920, 1080, true, Bananza) == Crop{1920, 1080});
    REQUIRE(Choose(1280, 720, true, Bananza) == Crop{1280, 720});
}

TEST_CASE("Nx2PresentCrop: an image within the panel is shown whole in handheld", "[core]") {
    REQUIRE(Choose(1920, 1080, false, Bananza) == Crop{1920, 1080});
    // A 1280x720 image (Fire Emblem) is smaller than the panel
    REQUIRE(Choose(1280, 720, false, 0) == Crop{1280, 720});
    // Only the side past the panel is cut
    REQUIRE(Choose(2560, 900, false, Ocarina) == Crop{1920, 900});
    REQUIRE(Choose(1600, 1440, false, Ocarina) == Crop{1600, 1080});
}

TEST_CASE("Nx2PresentCrop: Mario Kart World as in Anka 1.0.1", "[core]") {
    REQUIRE(Choose(3840, 2160, true, MarioKartWorld) == Crop{2560, 1440});
    REQUIRE(Choose(3840, 2160, false, MarioKartWorld) == Crop{1920, 1080});
    REQUIRE(Choose(2560, 1440, true, MarioKartWorld) == Crop{2560, 1440});
    // Only Mario Kart World's 4K docked images
    REQUIRE(Choose(3840, 2160, true, Bananza) == Crop{3840, 2160});
    REQUIRE(Choose(3840, 2160, true, Ocarina) == Crop{3840, 2160});
}

TEST_CASE("Nx2PresentCrop: usable at compile time", "[core]") {
    static_assert(Choose(2880, 1620, false, Bananza) == Crop{1920, 1080});
    static_assert(Choose(2880, 1620, true, Bananza) == Crop{2880, 1620});
}
