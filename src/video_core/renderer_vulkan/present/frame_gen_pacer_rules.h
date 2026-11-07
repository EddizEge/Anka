// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>

#include "common/common_types.h"

namespace Vulkan::FrameGenPacerRules {

// The display-bound decisions of FrameGenPacer in target-rate mode ("Dynamic",
// frame_gen_target_rate != 0), without clocks or settings (src/tests/video_core/frame_gen_pacer.cpp).
// Fixed multiplier mode (target 0) does not use them.
//
// With frame gen the swapchain is FIFO: every present (real or generated) waits for its own
// refresh. When the game's frame rate times the outputs per real frame exceeds the refresh rate,
// vsync throttles the game itself down to the next divisor of the refresh: at 60 Hz a game at
// 33 FPS on 2x locks to 30 FPS and shows 60, more frames on screen but fewer real ones.

/// A probe step (one more generated frame per real frame) is kept only when the game's own frame
/// rate after it is at least this fraction of the rate before it. The observed failure (60 Hz,
/// 33 -> 30 FPS on 1x -> 2x) is a 9% drop, and vsync locking to a lower divisor always costs more
/// than that at these rates; FSR3's own GPU cost is a few percent, so a tighter 5% limit would also
/// reject legitimate steps on fast screens.
constexpr f32 PROBE_BASE_RETENTION = 0.92f;

/// The refresh rate in Hz from FrameGen::RefreshPeriod (seconds, 0 = unknown); 0 when unknown.
[[nodiscard]] constexpr f32 RefreshHz(f32 refresh_period) {
    return refresh_period > 0.0f ? 1.0f / refresh_period : 0.0f;
}

/// The output rate to aim for: the target, but never above a known refresh rate (frames beyond
/// the refresh are never shown, they only hold the game back).
[[nodiscard]] constexpr f32 EffectiveTarget(f32 target_rate, f32 refresh_hz) {
    return refresh_hz > 0.0f ? std::min(target_rate, refresh_hz) : target_rate;
}

/// Whether the pacer may step from `limit` generated frames per real frame to `limit + 1`: the
/// game's frame rate times the outputs per real frame after the step (limit + 2) must still fit
/// the refresh, otherwise vsync slows the game. Unknown refresh (0): no cap.
[[nodiscard]] constexpr bool CanStepUp(f32 base_rate, size_t limit, f32 refresh_hz) {
    return refresh_hz <= 0.0f || base_rate * static_cast<f32>(limit + 2) <= refresh_hz;
}

/// A level is kept after the refresh rate changed only while its output stays this far below the
/// new refresh: vsync holds a game whose output no longer fits to exactly refresh / (level + 1),
/// so a level that only just fits cannot be told from one vsync is already throttling.
constexpr f32 REFRESH_FIT_MARGIN = 0.97f;

/// Refresh rates closer than this (relative) are the same display mode.
constexpr f32 REFRESH_CHANGE_RATIO = 0.01f;

/// Whether the refresh rate changed from `before_hz` to `now_hz` (Hz, 0 = unknown).
[[nodiscard]] constexpr bool RefreshChanged(f32 before_hz, f32 now_hz) {
    const f32 difference = before_hz > now_hz ? before_hz - now_hz : now_hz - before_hz;
    return difference > (before_hz > now_hz ? before_hz : now_hz) * REFRESH_CHANGE_RATIO;
}

/// The level after the refresh rate changed (the window moved to another display, the display
/// mode changed): `limit` while the game's frame rate times its outputs clearly fits the new
/// refresh, else 0, and the pacer climbs again from the game's own frame rate. Not the highest
/// level that fits: once the output exceeds the refresh, vsync has already lowered the measured
/// frame rate (at 60 Hz a 46 FPS game on 3x reads 20), so a lower level would look like it fits
/// and stay throttled. Unknown refresh (0): no cap, `limit`.
[[nodiscard]] constexpr size_t LimitAfterRefreshChange(f32 base_rate, size_t limit,
                                                       f32 refresh_hz) {
    if (refresh_hz <= 0.0f ||
        base_rate * static_cast<f32>(limit + 1) <= refresh_hz * REFRESH_FIT_MARGIN) {
        return limit;
    }
    return 0;
}

/// Whether a probe step left the game's own frame rate where it was (see PROBE_BASE_RETENTION);
/// false means the step is reverted.
[[nodiscard]] constexpr bool ProbeKeepsGameRate(f32 base_before, f32 base_after) {
    return base_after >= base_before * PROBE_BASE_RETENTION;
}

} // namespace Vulkan::FrameGenPacerRules
