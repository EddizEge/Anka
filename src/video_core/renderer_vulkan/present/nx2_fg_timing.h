// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <optional>

#include "common/common_types.h"

namespace Vulkan::Nx2FgTiming {

// EDEN_NX2_FG_TIMED=1 (off by default): the weight of each generated frame for FSR3 frame gen,
// without clocks, settings or logging (src/tests/video_core/nx2_fg_timing.cpp).
//
// With frame gen the swapchain is FIFO, and RendererVulkan::Composite presents the n generated
// frames of a burst and then the real frame N right after each other: the burst goes out one
// refresh V apart and N stays on screen until the next burst, B - n * V later (B: the game's frame
// interval). Generated frame k (1..n) is on screen (n - k + 1) * V before N, so the content that
// belongs there is t_k = 1 - (n - k + 1) * V / B of the way from N - 1 to N. The even weights
// k / (n + 1) assume the presents are spread over B; with B = 33.3 ms, n = 3 and V = 5.88 ms
// (170 Hz) they are 0.25 / 0.50 / 0.75 against 0.47 / 0.65 / 0.82, so the picture stops for
// 15.7 ms and then sweeps through three quarters of the motion in 17.6 ms.
//
// Anchored on N (the content of the presents right before it): the spacing between real frames is
// what it is (FIFO, the game's own pacing) and this only fixes the frames in between. B is the
// interval of this very frame (FrameGenPacer::LastInterval): N - 1 went out that long before N.
// When the presents cannot go out faster than they come (B <= (n + 1) * V, the queue never drains)
// they are one refresh apart all the time and the even weights are right; both forms meet there.
// Strict FIFO is modelled; with FIFO_RELAXED a late first present shows at once, its lead on N lies
// between (n - 1) * V and n * V, and the weight used stays between the even and the right one.

/// The weight the frame gen has always used: generation 0..count-1 of count.
[[nodiscard]] inline f32 EvenBlend(size_t generation, size_t count) {
    return static_cast<f32>(generation + 1) / static_cast<f32>(count + 1);
}

/// True when the timed weights apply: count generated frames, the frame interval and the refresh
/// period known (seconds) and the presents of a burst draining before the next one. Intervals of
/// more than 1000 refreshes (the pacer gives none above 0.1 s) are left alone too.
[[nodiscard]] inline bool TimedApplies(size_t count, f32 frame_interval, f32 refresh_period) {
    if (count == 0 || !std::isfinite(frame_interval) || !std::isfinite(refresh_period) ||
        frame_interval <= 0.0f || refresh_period <= 0.0f) {
        return false;
    }
    return frame_interval > static_cast<f32>(count + 1) * refresh_period &&
           frame_interval <= 1000.0f * refresh_period;
}

/// The timed weight of generation 0..count-1 (k = generation + 1): 1 - (count - generation) * V / B,
/// or EvenBlend when it does not apply. Inside (0, 1), increasing in generation and never below the
/// even weight (B > (n + 1) * V).
[[nodiscard]] inline f32 TimedBlend(size_t generation, size_t count, f32 frame_interval,
                                    f32 refresh_period) {
    if (generation >= count || !TimedApplies(count, frame_interval, refresh_period)) {
        return EvenBlend(generation, count);
    }
    const f32 before_real = static_cast<f32>(count - generation) * refresh_period;
    // Never below the even weight, also by a rounding step right above B = (n + 1) * V
    return std::max(EvenBlend(generation, count), 1.0f - before_real / frame_interval);
}

/// '1' first turns a switch on (EDEN_NX2_FG_TIMED, EDEN_NX2_FG_DIAG); unset, empty or anything
/// else leaves it off.
[[nodiscard]] inline bool SwitchOn(const char* text) {
    return text != nullptr && text[0] == '1';
}

/// EDEN_NX2_FG_REFRESH_HZ: the display refresh in Hz (20-1000, fractions allowed), overriding the
/// one read from the display. Unset or empty: nullopt. Anything else: nullopt and *invalid set.
[[nodiscard]] inline std::optional<f32> ParseRefreshHz(const char* text, bool* invalid = nullptr) {
    if (invalid != nullptr) {
        *invalid = false;
    }
    if (text == nullptr || text[0] == '\0') {
        return std::nullopt;
    }
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || value < 20.0 || value > 1000.0) {
        if (invalid != nullptr) {
            *invalid = true;
        }
        return std::nullopt;
    }
    return static_cast<f32>(value);
}

/// A refresh read from the display (Hz, 0 = unknown): Windows reports 0 and 1 for the hardware
/// default, and nothing below 20 Hz is a refresh the burst can be paced by.
[[nodiscard]] inline std::optional<f32> UsableDisplayHz(f32 hz) {
    if (!std::isfinite(hz) || hz < 20.0f || hz > 1000.0f) {
        return std::nullopt;
    }
    return hz;
}

} // namespace Vulkan::Nx2FgTiming
