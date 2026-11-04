// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Vulkan::Nx2FgTiming {

/// The refresh rate (Hz) of the display the window is on, 0 when unknown (no window, not
/// Windows, the hardware default reported). For EDEN_NX2_FG_TIMED; kept apart from the frame gen
/// so <windows.h> stays out of the Vulkan sources.
[[nodiscard]] f32 QueryDisplayRefreshHz(void* native_window);

} // namespace Vulkan::Nx2FgTiming
