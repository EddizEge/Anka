// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include "common/assert.h"

/// The EDEN_NX2_* switches read once, before main, into plain globals (the per-draw paths test
/// them without a function-local static's initialization check): EDEN_NX2_RANGE_SKIP, SSBO_MEMO,
/// SYNC_SKIP, SYNC_EXACT, SAME_SKIP, SMALL_WRITE, ALIAS_LAZY and TEX_TOUCH_SKIP. Each place that
/// reads one registers a function that reads it again (a global in an anonymous namespace has a
/// copy per translation unit, each registered). The menu program sets its Anka settings after
/// main (Nx2PerfSession::Prepare, before the first game boots) and then calls ReloadAll, so the
/// Safe preset's "0" reaches these too. Call it only while no emulation thread runs.
namespace Common::Nx2EarlySwitches {

using ReloadFunction = void (*)();

inline std::vector<ReloadFunction>& Registry() {
    static std::vector<ReloadFunction> functions;
    return functions;
}

/// From a dynamic initializer: `[[maybe_unused]] const bool x = Register(&ReloadMine);`
inline bool Register(ReloadFunction function) {
    Registry().push_back(function);
    return true;
}

/// Reads every registered switch again from the environment. Returns how many functions ran.
/// Once per process: a second call asserts (the globals are plain, not atomic; a game that ran
/// already may have threads reading them). The caller asserts that no emulation runs.
inline std::size_t ReloadAll() {
    static std::atomic<bool> reloaded{false};
    ASSERT_MSG(!reloaded.exchange(true), "Nx2EarlySwitches::ReloadAll called twice");
    for (const ReloadFunction function : Registry()) {
        function();
    }
    return Registry().size();
}

} // namespace Common::Nx2EarlySwitches
