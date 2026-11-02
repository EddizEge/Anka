// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/cpu_features.h"
#include "video_core/nx2_busy_clock.h"

namespace VideoCommon::Nx2ModeGov {

// EDEN_NX2_MODE_GOV=1 (multicore, off by default; needs EDEN_NX2_PRESENT_INTERVAL_WORD for the
// game's mode): pick Donkey Kong Bananza's 60 / 30 / 20 fps mode from the frame time Eden reaches
// and steer the game there through the GPU time it reads, without holding that time where its
// dynamic resolution drops (its tensor-core upscaler is rejected and the frame turns purple).
// A run with the busy clock (the frame's GPU time between the timestamps at 0x...c3000 and
// 0x...c3010, and in a ring of three at 0x...caa000 + 0x30 k, both around the whole frame):
//  - the resolution drops at ~22-25 ms in every mode within ~1 s, to its lowest (50%) at ~28 ms,
//    and comes back only below ~12 ms;
//  - 60 -> 30 after tens to hundreds of frames at ~28-36 ms, 60/30 -> 20 after one frame of
//    ~55 ms or more; 30 -> 60 and 20 -> 30 after ~8-17 frames at ~30-38 ms; 30 and 20 stay
//    at 5-25 ms.
// So the game reads hold_ms in the wanted mode, up_ms to go faster, one frame of spike_ms then
// down_ms to go slower from 60 (from 30 the spike, then hold_ms: down_ms would send 30 back to
// 60); a push lasts at most up_frames (from 30 down too) / down_frames, then cooldown_frames of
// hold_ms, and starts over when the game changes its mode. The wanted mode comes from the frame time without Eden's present pacing, averaged over
// avg_s: 60 at or below to60_ms, left above from60_ms; 20 above from30_ms, left below to30_ms.
// The timer is the busy clock (nx2_busy_clock.h) scaled at each present so that a frame's busy
// time reads as the shown time; it never goes back.

struct Params {
    double hold_ms = 10.0;
    double up_ms = 36.0;
    double down_ms = 34.0;
    double spike_ms = 70.0;
    double to60_ms = 17.0;
    double from60_ms = 19.5;
    double to30_ms = 31.0;
    double from30_ms = 36.0;
    double avg_s = 0.5;
    double down_dwell_s = 0.5;
    double up_dwell_s = 2.0;
    u32 up_frames = 30;
    u32 down_frames = 150;
    u32 cooldown_frames = 90;
};

/// One present: the game's mode (vblanks a frame, 1-3), its frame time without the pacing and the
/// wall time since the previous present.
struct Input {
    u32 mode{};
    double work_ms{};
    double dt_ms{};
};

struct Output {
    double shown_ms{}; ///< GPU time the game should read for this frame
    u32 want{};        ///< wanted mode (vblanks)
    bool want_changed{};
};

/// The decision part, without clocks or settings (src/tests/video_core/nx2_mode_gov.cpp).
class Governor {
public:
    explicit Governor(const Params& params_ = {}) : params{params_} {}

    Output Step(const Input& in) {
        const u32 mode = std::clamp<u32>(in.mode, 1, 3);
        const double dt_s = (std::max)(in.dt_ms, 0.0) / 1000.0;
        if (!have_avg) {
            avg_ms = in.work_ms;
            have_avg = true;
        } else {
            const double alpha = 1.0 - std::exp(-dt_s / (std::max)(params.avg_s, 0.01));
            avg_ms += alpha * (in.work_ms - avg_ms);
        }
        Output out{};
        if (want == 0) {
            want = mode;
            out.want_changed = true;
        }
        const u32 candidate = Candidate();
        if (candidate != want) {
            if (candidate != pending) {
                pending = candidate;
                pending_s = 0.0;
            }
            pending_s += dt_s;
            const double dwell = candidate < want ? params.up_dwell_s : params.down_dwell_s;
            if (pending_s >= dwell) {
                want = candidate;
                out.want_changed = true;
                pending = 0;
                pending_s = 0.0;
                pushing = 0;
                spiked = false;
                cooldown = 0;
            }
        } else {
            pending = 0;
            pending_s = 0.0;
        }
        out.want = want;
        if (mode != push_mode) {
            // The game changed its mode: a push goes on from the new one (60 -> 30 on the way to
            // 20 spikes again at 30).
            push_mode = mode;
            pushing = 0;
            spiked = false;
        }
        if (mode == want) {
            pushing = 0;
            spiked = false;
            out.shown_ms = params.hold_ms;
        } else if (cooldown > 0) {
            --cooldown;
            out.shown_ms = params.hold_ms;
        } else if (want < mode) {
            out.shown_ms = params.up_ms;
            if (++pushing >= params.up_frames) {
                pushing = 0;
                cooldown = params.cooldown_frames;
            }
        } else {
            // From 60 the spike, then down_ms (60 drops at ~28-36 ms). From 30 the spike alone,
            // then hold_ms: ~30-38 ms there sends 30 back to 60, not on to 20.
            const bool from60 = mode == 1;
            out.shown_ms = !spiked ? params.spike_ms : from60 ? params.down_ms : params.hold_ms;
            spiked = true;
            if (++pushing >= (from60 ? params.down_frames : params.up_frames)) {
                pushing = 0;
                spiked = false;
                cooldown = params.cooldown_frames;
            }
        }
        return out;
    }

    double Average() const {
        return avg_ms;
    }

private:
    /// The mode the average frame time asks for, with the current one's hysteresis.
    u32 Candidate() const {
        switch (want) {
        case 1:
            if (avg_ms > params.from30_ms) {
                return 3;
            }
            return avg_ms > params.from60_ms ? 2 : 1;
        case 2:
            if (avg_ms <= params.to60_ms) {
                return 1;
            }
            return avg_ms > params.from30_ms ? 3 : 2;
        default:
            if (avg_ms <= params.to60_ms) {
                return 1;
            }
            return avg_ms < params.to30_ms ? 2 : 3;
        }
    }

    Params params;
    double avg_ms{};
    bool have_avg{};
    u32 want{}; ///< 0: no present yet
    u32 pending{};
    double pending_s{};
    u32 pushing{};
    u32 push_mode{}; ///< the game's mode the push counts in
    u32 cooldown{};
    bool spiked{};
};

/// Busy ticks scaled per present: Read(busy) = base + (busy - busy_base) * scale, never below a
/// value read before.
struct ScaledClock {
    u64 base{};
    u64 busy_base{};
    double scale{1.0};
    u64 highest{};

    u64 Read(u64 busy) {
        const u64 since = busy > busy_base ? busy - busy_base : 0;
        const u64 value = base + static_cast<u64>(static_cast<double>(since) * scale);
        highest = (std::max)(highest, value);
        return highest;
    }

    /// From `busy` on, count with `new_scale`; returns the busy ticks since the previous rebase.
    u64 Rebase(u64 busy, double new_scale) {
        const u64 since = busy > busy_base ? busy - busy_base : 0;
        base = Read(busy);
        busy_base = (std::max)(busy, busy_base);
        scale = new_scale;
        return since;
    }
};

inline bool Enabled() {
    static const bool enabled = [] {
        const char* text = std::getenv("EDEN_NX2_MODE_GOV");
        if (text == nullptr || text[0] == '0') {
            return false;
        }
        // Both steer the same mode, one through the GPU time, one through late presents.
        if (const char* pace = std::getenv("EDEN_NX2_MODE_PACE"); pace != nullptr && pace[0] != '0') {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_GOV with EDEN_NX2_MODE_PACE: off (the pace "
                                "controller steers the mode)");
            return false;
        }
        if (!Settings::values.use_multi_core.GetValue()) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_GOV needs multicore: off");
            return false;
        }
        if (std::getenv("EDEN_NX2_PRESENT_INTERVAL_WORD") == nullptr) {
            LOG_WARNING(HW_GPU, "EDEN_NX2_MODE_GOV needs EDEN_NX2_PRESENT_INTERVAL_WORD: off");
            return false;
        }
        return true;
    }();
    return enabled;
}

/// EDEN_NX2_MODE_GOV_<NAME>=<value> overrides a parameter (ms, or frames for the counts).
inline Params ReadParams() {
    Params p{};
    const auto number = [](const char* name, double& value) {
        if (const char* text = std::getenv(name)) {
            const double parsed = std::strtod(text, nullptr);
            if (parsed > 0.0 && parsed < 1000.0) {
                value = parsed;
            }
        }
    };
    const auto frames = [](const char* name, u32& value) {
        if (const char* text = std::getenv(name)) {
            const long parsed = std::strtol(text, nullptr, 10);
            if (parsed >= 0 && parsed <= 100000) {
                value = static_cast<u32>(parsed);
            }
        }
    };
    number("EDEN_NX2_MODE_GOV_HOLD", p.hold_ms);
    number("EDEN_NX2_MODE_GOV_UP", p.up_ms);
    number("EDEN_NX2_MODE_GOV_DOWN", p.down_ms);
    number("EDEN_NX2_MODE_GOV_SPIKE", p.spike_ms);
    number("EDEN_NX2_MODE_GOV_TO60", p.to60_ms);
    number("EDEN_NX2_MODE_GOV_FROM60", p.from60_ms);
    number("EDEN_NX2_MODE_GOV_TO30", p.to30_ms);
    number("EDEN_NX2_MODE_GOV_FROM30", p.from30_ms);
    frames("EDEN_NX2_MODE_GOV_UP_FRAMES", p.up_frames);
    frames("EDEN_NX2_MODE_GOV_DOWN_FRAMES", p.down_frames);
    frames("EDEN_NX2_MODE_GOV_COOLDOWN", p.cooldown_frames);
    LOG_INFO(HW_GPU,
             "GPU timer steered (EDEN_NX2_MODE_GOV): {} ms a frame in the wanted mode, {} to go "
             "faster, {} then {} to go slower (at most {} / {} frames, then {} frames of rest); "
             "60 at or below {} ms of work, 30 above {}, 20 above {} (30 again below {})",
             p.hold_ms, p.up_ms, p.spike_ms, p.down_ms, p.up_frames, p.down_frames,
             p.cooldown_frames, p.to60_ms, p.from60_ms, p.from30_ms, p.to30_ms);
    return p;
}

constexpr double TicksPerMs = static_cast<double>(Common::WallClock::GPUTickFreq) / 1000.0;

struct State {
    explicit State(const Params& params) : governor{params} {}

    std::mutex mutex;
    Governor governor;
    ScaledClock clock;
    double busy_avg_ms{};
    std::chrono::steady_clock::time_point last_paced{};
    u32 last_mode{};
};

inline State& GetState() {
    static State state{ReadParams()};
    return state;
}

/// GPU::GetTicks while enabled.
inline u64 Ticks() {
    const u64 busy = Nx2BusyClock::Ticks();
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    return s.clock.Read(busy);
}

/// vi2 229 after its pacing: `mode` is PresentInterval's vblanks, `composited` and `paced` the
/// times before and after the pacing sleep.
inline void Present(u32 mode, std::chrono::steady_clock::time_point composited,
                    std::chrono::steady_clock::time_point paced) {
    const u64 busy = Nx2BusyClock::Ticks();
    auto& s = GetState();
    std::scoped_lock lock{s.mutex};
    const auto ms = [](auto duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    const bool first = s.last_paced == std::chrono::steady_clock::time_point{};
    const double dt_ms = first ? 0.0 : ms(paced - s.last_paced);
    const double work_ms = (std::max)(dt_ms - ms(paced - composited), 0.0);
    s.last_paced = paced;
    const u64 busy_ticks = s.clock.Rebase(busy, s.clock.scale);
    if (first) {
        s.last_mode = mode;
        return;
    }
    const double busy_ms = static_cast<double>(busy_ticks) / TicksPerMs;
    s.busy_avg_ms =
        s.busy_avg_ms == 0.0 ? busy_ms : s.busy_avg_ms + 0.3 * (busy_ms - s.busy_avg_ms);
    const Output out = s.governor.Step({.mode = mode, .work_ms = work_ms, .dt_ms = dt_ms});
    // The next frame's busy time, as long as the last ones, reads as out.shown_ms.
    s.clock.scale = std::clamp(out.shown_ms / (std::max)(s.busy_avg_ms, 0.5), 0.01, 100.0);
    if (out.want_changed || mode != s.last_mode) {
        LOG_INFO(HW_GPU,
                 "nx2 mode gov: game mode {} (was {}), wanted {} (work {:.1f} ms on average, GPU "
                 "busy {:.1f} ms a frame), showing {:.1f} ms",
                 mode, s.last_mode, out.want, s.governor.Average(), s.busy_avg_ms, out.shown_ms);
    }
    s.last_mode = mode;
}

} // namespace VideoCommon::Nx2ModeGov
