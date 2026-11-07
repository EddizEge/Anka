// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

// How much of a vi2 present image is shown (nx2_gsv.h Composite; the crop rect of its
// FramebufferConfig), without the settings (src/tests/core/nx2_present_crop.cpp).
namespace Service::Nx2PresentCrop {

constexpr u64 MarioKartWorld = 0x0400C3F00006E000ULL;

/// The handheld panel's resolution: the console's display scans out only this top-left region.
constexpr u32 HandheldWidth = 1920;
constexpr u32 HandheldHeight = 1080;

struct Crop {
    u32 width{};
    u32 height{};

    constexpr bool operator==(const Crop&) const = default;
};

/// `layout_*`: the image's size; `docked`: Settings::IsDockedMode().
///
/// Handheld titles keep their docked present images (up to 3840x2160) and draw the frame into
/// their top-left 1920x1080, the handheld panel's resolution: Donkey Kong Bananza draws into the
/// top-left of its 2880x1620 images, and showing them whole put its frame in the top-left two
/// thirds of the window inside black margins (Anka 1.0.x cut only Mario Kart World's; upstream
/// 0a47ba9567 cuts every handheld present). Docked, the whole image is shown, except Mario Kart
/// World's 4K images, of which it draws the top-left 2560x1440.
constexpr Crop Choose(u32 layout_width, u32 layout_height, bool docked, u64 program_id) {
    Crop crop{layout_width, layout_height};
    if (!docked) {
        crop.width = (std::min)(crop.width, HandheldWidth);
        crop.height = (std::min)(crop.height, HandheldHeight);
    } else if (program_id == MarioKartWorld && layout_width == 3840 && layout_height == 2160) {
        crop = {2560, 1440};
    }
    return crop;
}

} // namespace Service::Nx2PresentCrop
