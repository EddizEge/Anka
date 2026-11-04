// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/present/nx2_fg_timing.h"

namespace {

using namespace Vulkan::Nx2FgTiming;

constexpr f32 HZ170 = 1.0f / 170.0f;

bool Near(f32 a, f32 b, f32 tolerance = 0.005f) {
    return std::fabs(a - b) <= tolerance;
}

} // Anonymous namespace

TEST_CASE("Nx2FgTiming: the even weights are the old expression", "[video_core]") {
    for (size_t count = 1; count <= 7; ++count) {
        for (size_t generation = 0; generation < count; ++generation) {
            const f32 old = static_cast<f32>(generation + 1) / static_cast<f32>(count + 1);
            REQUIRE(EvenBlend(generation, count) == old);
        }
    }
}

TEST_CASE("Nx2FgTiming: the weights of the burst at its display times", "[video_core]") {
    // 30 fps, three generated frames at 170 Hz: 0.47 / 0.65 / 0.82
    const f32 b30 = 1.0f / 30.0f;
    REQUIRE(TimedApplies(3, b30, HZ170));
    REQUIRE(Near(TimedBlend(0, 3, b30, HZ170), 0.4706f));
    REQUIRE(Near(TimedBlend(1, 3, b30, HZ170), 0.6471f));
    REQUIRE(Near(TimedBlend(2, 3, b30, HZ170), 0.8235f));
    // 46 fps, two: 0.46 / 0.73
    const f32 b46 = 1.0f / 46.0f;
    REQUIRE(Near(TimedBlend(0, 2, b46, HZ170), 0.4588f));
    REQUIRE(Near(TimedBlend(1, 2, b46, HZ170), 0.7294f));
    // 60 fps, one: 1 - 5.88 / 16.67
    REQUIRE(Near(TimedBlend(0, 1, 1.0f / 60.0f, HZ170), 0.6471f));
    // Each generated frame is one refresh after the one before it: the steps are V / B
    for (size_t generation = 1; generation < 3; ++generation) {
        REQUIRE(Near(TimedBlend(generation, 3, b30, HZ170) -
                         TimedBlend(generation - 1, 3, b30, HZ170),
                     HZ170 / b30, 1.0e-5f));
    }
    // And the last one a refresh before the real frame
    REQUIRE(Near(1.0f - TimedBlend(2, 3, b30, HZ170), HZ170 / b30, 1.0e-5f));
}

TEST_CASE("Nx2FgTiming: the screen time between real frames when n changes", "[video_core]") {
    // Same n: the frame interval itself
    REQUIRE(DisplayGap(1.0f / 30.0f, 3, 3, HZ170) == 1.0f / 30.0f);
    // 50 fps at 170 Hz, the credit alternating n: N - 1 went out n_prev refreshes after its
    // Composite, N n after this one
    const f32 c = 0.020f;
    const f32 more = DisplayGap(c, 2, 1, HZ170); // 25.9 ms
    REQUIRE(Near(more, c + HZ170, 1.0e-6f));
    REQUIRE(Near(TimedBlend(0, 2, more, HZ170), 0.5455f));
    REQUIRE(Near(TimedBlend(1, 2, more, HZ170), 0.7727f));
    const f32 fewer = DisplayGap(c, 1, 2, HZ170); // 14.1 ms
    REQUIRE(Near(fewer, c - HZ170, 1.0e-6f));
    REQUIRE(TimedApplies(1, fewer, HZ170));
    REQUIRE(Near(TimedBlend(0, 1, fewer, HZ170), 0.5833f));
    // The previous burst of four presents (23.5 ms) has not drained by 20 ms: even weights
    const f32 backed_up = DisplayGap(c, 2, 3, HZ170);
    REQUIRE_FALSE(TimedApplies(2, backed_up, HZ170));
    REQUIRE(TimedBlend(0, 2, backed_up, HZ170) == EvenBlend(0, 2));
    // In general the drain test on the gap is C > (n_prev + 1) * V
    for (size_t previous = 0; previous <= 3; ++previous) {
        for (size_t count = 1; count <= 3; ++count) {
            for (f32 interval = 0.002f; interval < 0.06f; interval += 0.0001f) {
                const bool drained = interval > static_cast<f32>(previous + 1) * HZ170 + 1.0e-6f;
                const bool backed = interval < static_cast<f32>(previous + 1) * HZ170 - 1.0e-6f;
                const bool applies =
                    TimedApplies(count, DisplayGap(interval, count, previous, HZ170), HZ170);
                if (drained) {
                    REQUIRE(applies);
                }
                if (backed) {
                    REQUIRE_FALSE(applies);
                }
            }
        }
    }
}

TEST_CASE("Nx2FgTiming: falls back to the even weights", "[video_core]") {
    const f32 b30 = 1.0f / 30.0f;
    // No generated frames, a generation past the count
    REQUIRE_FALSE(TimedApplies(0, b30, HZ170));
    REQUIRE(TimedBlend(3, 3, b30, HZ170) == EvenBlend(3, 3));
    REQUIRE(TimedBlend(5, 3, b30, HZ170) == EvenBlend(5, 3));
    // Interval or refresh unknown (pacer reset or stalled: 0), negative, not finite
    const f32 bad[] = {0.0f, -0.01f, NAN, INFINITY, -INFINITY};
    for (const f32 value : bad) {
        REQUIRE_FALSE(TimedApplies(3, value, HZ170));
        REQUIRE_FALSE(TimedApplies(3, b30, value));
        for (size_t generation = 0; generation < 3; ++generation) {
            REQUIRE(TimedBlend(generation, 3, value, HZ170) == EvenBlend(generation, 3));
            REQUIRE(TimedBlend(generation, 3, b30, value) == EvenBlend(generation, 3));
        }
    }
    // B <= (n + 1) * V: the presents never drain, they are one refresh apart all the time
    REQUIRE_FALSE(TimedApplies(3, 4.0f * HZ170, HZ170));
    REQUIRE_FALSE(TimedApplies(3, 3.5f * HZ170, HZ170));
    REQUIRE_FALSE(TimedApplies(1, 2.0f * HZ170, HZ170));
    REQUIRE(TimedApplies(3, 4.01f * HZ170, HZ170));
    for (size_t generation = 0; generation < 3; ++generation) {
        REQUIRE(TimedBlend(generation, 3, 4.0f * HZ170, HZ170) == EvenBlend(generation, 3));
        // Right above the edge the timed weights meet the even ones (no jump)
        REQUIRE(Near(TimedBlend(generation, 3, 4.0001f * HZ170, HZ170), EvenBlend(generation, 3),
                     1.0e-4f));
    }
    // 120 fps at 170 Hz with one generated frame: 8.3 ms < 2 * 5.88 ms
    REQUIRE_FALSE(TimedApplies(1, 1.0f / 120.0f, HZ170));
    // Far more than 1000 refreshes
    REQUIRE_FALSE(TimedApplies(1, 1001.0f * HZ170, HZ170));
    REQUIRE(TimedApplies(1, 1000.0f * HZ170, HZ170));
}

TEST_CASE("Nx2FgTiming: inside (0, 1), increasing, never below the even weights", "[video_core]") {
    const f32 refreshes[] = {1.0f / 60.0f, 1.0f / 120.0f, 1.0f / 144.0f, HZ170, 1.0f / 240.0f,
                             1.0f / 360.0f};
    size_t timed = 0;
    for (const f32 period : refreshes) {
        for (size_t count = 1; count <= 7; ++count) {
            for (f32 interval = 0.001f; interval <= 0.2f; interval += 0.00037f) {
                f32 previous = 0.0f;
                for (size_t generation = 0; generation < count; ++generation) {
                    const f32 t = TimedBlend(generation, count, interval, period);
                    REQUIRE(t > 0.0f);
                    REQUIRE(t < 1.0f);
                    REQUIRE(t > previous);
                    REQUIRE(t >= EvenBlend(generation, count));
                    previous = t;
                }
                timed += TimedApplies(count, interval, period) ? 1 : 0;
            }
        }
    }
    REQUIRE(timed > 1000);
}

TEST_CASE("Nx2FgTiming: switches and the refresh override", "[video_core]") {
    REQUIRE_FALSE(SwitchOn(nullptr));
    REQUIRE_FALSE(SwitchOn(""));
    REQUIRE_FALSE(SwitchOn("0"));
    REQUIRE_FALSE(SwitchOn("2"));
    REQUIRE_FALSE(SwitchOn("yes"));
    REQUIRE(SwitchOn("1"));
    REQUIRE(SwitchOn("10"));

    bool invalid = true;
    REQUIRE_FALSE(ParseRefreshHz(nullptr, &invalid).has_value());
    REQUIRE_FALSE(invalid);
    REQUIRE_FALSE(ParseRefreshHz("", &invalid).has_value());
    REQUIRE_FALSE(invalid);
    REQUIRE(ParseRefreshHz("170", &invalid).value_or(0.0f) == 170.0f);
    REQUIRE_FALSE(invalid);
    REQUIRE(Near(ParseRefreshHz("59.94", &invalid).value_or(0.0f), 59.94f, 1.0e-4f));
    REQUIRE(ParseRefreshHz("20").has_value());
    REQUIRE(ParseRefreshHz("1000").has_value());
    const char* wrong[] = {"0", "19.9", "1001", "abc", "170hz", " ", "nan", "inf", "-60"};
    for (const char* text : wrong) {
        invalid = false;
        REQUIRE_FALSE(ParseRefreshHz(text, &invalid).has_value());
        REQUIRE(invalid);
    }

    REQUIRE_FALSE(UsableDisplayHz(0.0f).has_value());
    REQUIRE_FALSE(UsableDisplayHz(1.0f).has_value());
    REQUIRE_FALSE(UsableDisplayHz(NAN).has_value());
    REQUIRE(UsableDisplayHz(170.0f).value_or(0.0f) == 170.0f);
}
