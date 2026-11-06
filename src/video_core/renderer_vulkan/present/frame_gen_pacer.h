// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <optional>

#include "common/common_types.h"

namespace Vulkan {

struct FrameGenPlan {
    size_t generations{};
    bool warm{};
};

class FrameGenPacer {
public:
    /// refresh_period: the display's refresh period in seconds (FrameGen::RefreshPeriod), 0 when
    /// unknown; in target-rate mode it caps the target and the generation level
    /// (frame_gen_pacer_rules.h).
    [[nodiscard]] FrameGenPlan Plan(size_t capacity, f32 refresh_period);

    void Reset();

    /// The interval between this real frame and the one before it in seconds, as the last Plan
    /// measured it (0 when that call did not get to measure it: first frame, stall, burst);
    /// for EDEN_NX2_FG_TIMED, read on the thread that calls Plan.
    [[nodiscard]] f32 LastInterval() const {
        return last_interval;
    }

private:
    using Clock = std::chrono::steady_clock;

    void Stabilize(Clock::time_point now);
    void DeferEvaluations(Clock::duration amount);
    void UpdateLimit(Clock::time_point now, f32 base_rate, f32 target_rate, f32 refresh_hz,
                     size_t ceiling);

    std::optional<Clock::time_point> last_frame;
    std::optional<Clock::time_point> stable_until;
    std::optional<Clock::time_point> probe_until;
    std::optional<Clock::time_point> next_probe;
    std::optional<Clock::time_point> deficit_since;
    f32 smoothed_interval{};
    f32 last_interval{};
    f32 output_credit{};
    f32 probe_base_rate{};
    size_t probe_previous_limit{};
    size_t limit{};
    u32 probe_failures{};
};

} // namespace Vulkan
