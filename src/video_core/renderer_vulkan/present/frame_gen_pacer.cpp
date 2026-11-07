// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/present/frame_gen_pacer.h"
#include "video_core/renderer_vulkan/present/frame_gen_pacer_rules.h"

namespace Vulkan {

namespace {

using Clock = std::chrono::steady_clock;

constexpr f32 INTERVAL_SMOOTHING = 0.25f;
constexpr f32 MINIMUM_BASE_RATE = 10.0f;
constexpr f32 BURST_CADENCE_RATIO = 3.0f;
constexpr f32 BURST_TARGET_RATIO = 2.0f;
constexpr f32 PROBE_THROUGHPUT_TOLERANCE = 0.95f;
constexpr f32 PROBE_BASE_COLLAPSE_RATIO = 0.70f;
constexpr f32 PROBE_MARGINAL_GAIN = 1.15f;
constexpr f32 TARGET_SATISFIED_RATIO = 0.95f;
constexpr f32 CREDIT_EPSILON = 1.0e-4f;
constexpr u32 MAX_PROBE_FAILURES = 4;

constexpr auto STABILIZATION_DURATION = std::chrono::seconds(1);
constexpr auto PROBE_DURATION = std::chrono::seconds(1);
constexpr auto DEFICIT_DURATION = std::chrono::seconds(1);
constexpr auto PROBE_STEP_DELAY = std::chrono::milliseconds(250);

[[nodiscard]] Clock::duration ProbeBackoff(u32 failures) {
    switch (failures) {
    case 1:
        return std::chrono::seconds(5);
    case 2:
        return std::chrono::seconds(15);
    case 3:
        return std::chrono::seconds(30);
    default:
        return std::chrono::seconds(60);
    }
}

} // Anonymous namespace

FrameGenPlan FrameGenPacer::Plan(size_t capacity, f32 refresh_period) {
    return Plan(capacity, refresh_period, Clock::now());
}

FrameGenPlan FrameGenPacer::Plan(size_t capacity, f32 refresh_period, Clock::time_point now) {
    const size_t ceiling = std::min(capacity, Settings::FrameGenMaxGenerations());
    if (ceiling == 0) {
        Reset();
        return {};
    }

    last_interval = 0.0f;
    if (!last_frame) {
        last_frame = now;
        return {};
    }

    const Clock::duration interval = now - *last_frame;
    const f32 interval_seconds = std::chrono::duration<f32>(interval).count();
    last_frame = now;

    if (interval_seconds <= 0.0f) {
        Stabilize(now);
        return {};
    }

    const f32 refresh_hz = FrameGenPacerRules::RefreshHz(refresh_period);
    const f32 target_rate = FrameGenPacerRules::EffectiveTarget(
        static_cast<f32>(Settings::values.frame_gen_target_rate.GetValue()), refresh_hz);

    if (smoothed_interval > 0.0f) {
        f32 burst_threshold = BURST_CADENCE_RATIO / smoothed_interval;
        if (target_rate > 0.0f) {
            burst_threshold = std::max(burst_threshold, target_rate * BURST_TARGET_RATIO);
        }
        if (1.0f / interval_seconds > burst_threshold) {
            DeferEvaluations(interval);
            output_credit = 0.0f;
            return {};
        }
    }

    if (interval_seconds > 1.0f / MINIMUM_BASE_RATE) {
        Stabilize(now);
        return {};
    }

    last_interval = interval_seconds;
    smoothed_interval = smoothed_interval > 0.0f
                            ? smoothed_interval +
                                  (interval_seconds - smoothed_interval) * INTERVAL_SMOOTHING
                            : interval_seconds;

    if (stable_until) {
        if (now < *stable_until) {
            return {};
        }
        stable_until.reset();
    }

    if (target_rate == 0.0f) {
        limit = std::min(Settings::FrameGenGenerations(), ceiling);
        output_credit = 0.0f;
        return {.generations = limit, .warm = limit > 0};
    }

    UpdateLimit(now, 1.0f / smoothed_interval, target_rate, refresh_hz, ceiling);

    const size_t allowed = std::min(limit, ceiling);
    const f32 desired_outputs = smoothed_interval * target_rate;
    if (allowed == 0 || desired_outputs <= 1.0f) {
        output_credit = 0.0f;
        return {};
    }

    output_credit += desired_outputs;
    const size_t outputs =
        std::max<size_t>(1, static_cast<size_t>(std::floor(output_credit + CREDIT_EPSILON)));
    const size_t generations = std::min(outputs - 1, allowed);

    output_credit -= static_cast<f32>(generations + 1);
    if (output_credit < 0.0f) {
        output_credit = 0.0f;
    } else if (generations == allowed && output_credit >= 1.0f) {
        output_credit = std::fmod(output_credit, 1.0f);
    }

    return {.generations = generations, .warm = true};
}

void FrameGenPacer::UpdateLimit(Clock::time_point now, f32 base_rate, f32 target_rate,
                                f32 refresh_hz, size_t ceiling) {
    limit = std::min(limit, ceiling);

    if (FrameGenPacerRules::RefreshChanged(refresh_hz_seen, refresh_hz)) {
        // The window moved to another display or the mode changed: a step in progress measured
        // the old refresh, and a level the new one cannot show would let vsync slow the game
        if (probe_until) {
            probe_until.reset();
            limit = probe_previous_limit;
        }
        const size_t before = limit;
        limit = FrameGenPacerRules::LimitAfterRefreshChange(base_rate, limit, refresh_hz);
        if (refresh_hz_seen > 0.0f || limit != before) {
            LOG_INFO(Render_Vulkan, "Frame gen pacer: display refresh {:.1f} -> {:.1f} Hz, {} -> {} "
                                    "generated frame(s) per real frame",
                     refresh_hz_seen, refresh_hz, before, limit);
        }
        refresh_hz_seen = refresh_hz;
        probe_failures = 0;
        next_probe.reset();
        deficit_since.reset();
        output_credit = 0.0f;
        return;
    }

    if (probe_until) {
        if (now < *probe_until) {
            return;
        }
        probe_until.reset();
        output_credit = 0.0f;

        const f32 previous_output =
            std::min(target_rate, probe_base_rate * static_cast<f32>(probe_previous_limit + 1));
        const f32 current_output =
            std::min(target_rate, base_rate * static_cast<f32>(limit + 1));

        const bool throughput_regressed =
            current_output < previous_output * PROBE_THROUGHPUT_TOLERANCE;
        const bool collapsed_for_marginal_gain =
            base_rate < probe_base_rate * PROBE_BASE_COLLAPSE_RATIO &&
            current_output < previous_output * PROBE_MARGINAL_GAIN;
        // The game's own frame rate must not drop with the step: more frames on screen with
        // fewer real ones (vsync holding the game to a lower divisor) is not a gain
        const bool emulation_slowed =
            !FrameGenPacerRules::ProbeKeepsGameRate(probe_base_rate, base_rate);

        if (throughput_regressed || collapsed_for_marginal_gain || emulation_slowed) {
            limit = probe_previous_limit;
            probe_failures = std::min(probe_failures + 1, MAX_PROBE_FAILURES);
            next_probe = now + ProbeBackoff(probe_failures);
            deficit_since.reset();
            return;
        }

        probe_failures = 0;
        next_probe = now + PROBE_STEP_DELAY;
    }

    if (base_rate * static_cast<f32>(limit + 1) >= target_rate * TARGET_SATISFIED_RATIO ||
        limit >= ceiling || !FrameGenPacerRules::CanStepUp(base_rate, limit, refresh_hz)) {
        deficit_since.reset();
        return;
    }

    if (!deficit_since) {
        deficit_since = now;
        return;
    }
    if (now - *deficit_since < DEFICIT_DURATION) {
        return;
    }
    if (next_probe && now < *next_probe) {
        return;
    }

    probe_previous_limit = limit;
    probe_base_rate = base_rate;
    ++limit;
    probe_until = now + PROBE_DURATION;
    deficit_since.reset();
    output_credit = 0.0f;
}

void FrameGenPacer::DeferEvaluations(Clock::duration amount) {
    const auto defer = [amount](std::optional<Clock::time_point>& deadline) {
        if (deadline) {
            *deadline += amount;
        }
    };
    defer(stable_until);
    defer(probe_until);
    defer(next_probe);
    deficit_since.reset();
}

void FrameGenPacer::Stabilize(Clock::time_point now) {
    stable_until = now + STABILIZATION_DURATION;
    probe_until.reset();
    deficit_since.reset();
    smoothed_interval = 0.0f;
    output_credit = 0.0f;
}

void FrameGenPacer::Reset() {
    last_frame.reset();
    stable_until.reset();
    probe_until.reset();
    next_probe.reset();
    deficit_since.reset();
    smoothed_interval = 0.0f;
    last_interval = 0.0f;
    output_credit = 0.0f;
    probe_base_rate = 0.0f;
    refresh_hz_seen = 0.0f;
    probe_previous_limit = 0;
    limit = 0;
    probe_failures = 0;
}

} // namespace Vulkan
