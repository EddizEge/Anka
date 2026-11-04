// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>

#include "common/common_types.h"

namespace Nx2Osd {

class Writer;

// EDEN_NX2_OSD=1 (off by default, Windows only): a thread puts "Oyun <fps> FPS | Ekran <fps> FPS
// | Hiz %<speed>" in an OSD slot of RivaTuner Statistics Server (common/nx2_rtss_osd.h) every
// ~0.5 s, averaged over the last ~1 s. RTSS shows it once its own "Show On-Screen Display" is
// on; RTSS is not started, configured or required: without it the thread retries every 2 s.
// `system_time_us`: the emulated clock, the speed when the presents give none.
// Unset: nullptr, nothing started or opened. Destroying the writer stops the thread and empties
// the slot.
struct WriterDeleter {
    void operator()(Writer* writer) const;
};
using WriterPtr = std::unique_ptr<Writer, WriterDeleter>;

WriterPtr StartFromEnvironment(std::function<s64()> system_time_us);

} // namespace Nx2Osd
