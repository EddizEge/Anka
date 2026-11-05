// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/nx2_reactive.h"
#include "common/settings.h"

namespace VideoCommon::Nx2Reactive {

/// EDEN_NX2_RX_PITCH_FORCED=1: with reactive flushing on, pitch linear images are still
/// forced_flushed (downloaded at every render target bind; a DMA read of one sees guest memory as
/// downloaded), as they are with it off; guest CPU reads still trap and latch the other images.
/// Without the switch, reactive flushing leaves pitch linear images to the reads it traps: data
/// the guest gets another way (not a trapped read of the image's own pages) stays stale.
[[nodiscard]] constexpr bool PitchLinearForced(bool reactive_flushing, bool pitch_forced) noexcept {
    return !reactive_flushing || pitch_forced;
}

[[nodiscard]] inline bool PitchForcedSwitch() noexcept {
    static const bool enabled =
        Common::Nx2Reactive::ParseFlag(Common::Nx2Reactive::GetEnv("EDEN_NX2_RX_PITCH_FORCED"));
    return enabled;
}

/// Whether a pitch linear image is made forced_flushed (ImageInfo's constructors).
[[nodiscard]] inline bool ForcePitchLinear() noexcept {
    return PitchLinearForced(Settings::values.use_reactive_flushing.GetValue(),
                             PitchForcedSwitch());
}

} // namespace VideoCommon::Nx2Reactive
