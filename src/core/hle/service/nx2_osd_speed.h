// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <mutex>

#include "common/common_types.h"
#include "common/nx2_rtss_osd.h"
#include "core/hle/service/nx2_dk_dyn_pace.h"

// EDEN_NX2_OSD's speed (common/nx2_rtss_osd.h, yuzu_cmd/nx2_osd.cpp): the vi2 present thread adds
// each present's interval and the game time it stepped; the OSD thread reads the totals twice a
// second. Off (EDEN_NX2_OSD unset), the present path only reads Enabled()'s static.
namespace Service::Nx2OsdSpeed {

inline bool Enabled() {
    static const bool enabled = Common::Nx2RtssOsd::Enabled(std::getenv("EDEN_NX2_OSD"));
    return enabled;
}

/// The game time a present's interval `frame_us` stepped. DK-DYN applied (`dk_dyn`): the patched
/// clock's step, the frame up to 2.5 vblanks (Nx2DkDynPace::StepEstimate, the 600 present
/// line's "speed estimate"). Else `interval` vblanks at 60 Hz when the swap interval word is
/// known (EDEN_NX2_PRESENT_INTERVAL_WORD: Bananza's 60/30/20 modes step a whole interval a
/// frame); 0: no estimate (`interval` 0), the OSD falls back to the emulated clock.
constexpr long long GameUs(bool dk_dyn, u32 interval, long long frame_us) {
    if (frame_us <= 0) {
        return 0;
    }
    if (dk_dyn) {
        return (std::min)(frame_us, Nx2DkDynPace::StepEstimate::CapUs);
    }
    return static_cast<long long>(interval) * 1'000'000 / 60;
}

struct Totals {
    u64 frame_us{};
    u64 game_us{};
};

/// The totals, under a mutex so the pair is read whole (a present at most every vblank; the
/// reader twice a second).
class Counter {
public:
    void Add(long long frame_us, long long game_us) {
        if (frame_us <= 0 || game_us <= 0) {
            return;
        }
        std::scoped_lock lock{mutex};
        totals.frame_us += static_cast<u64>(frame_us);
        totals.game_us += static_cast<u64>(game_us);
    }

    Totals Get() {
        std::scoped_lock lock{mutex};
        return totals;
    }

private:
    std::mutex mutex;
    Totals totals{};
};

inline Counter g_counter;

} // namespace Service::Nx2OsdSpeed
