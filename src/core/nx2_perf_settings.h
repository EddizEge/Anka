// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "core/loader/nx2_dk_dynamic_fps.h"

// NX2Perf's settings in the Qt program (eglantine-nx2perf.exe): the EDEN_NX2_* switches the
// recommended launcher sets (dist/nx2perf/start-recommended.bat), as on / off settings kept in the
// [NX2Perf] group of qt-config.ini, all on until changed. The program sets the variables in its
// own environment before a game boots (yuzu/nx2perf_session.h); eden-cli and the launchers don't
// read these settings.
//
// Every switch is read once per process, most of them into a function's static the first time
// a game reaches it (EDEN_NX2_BACK_STAGES, ACQUIRE_PEEK, PRESENT_INTERVAL_WORD, MODE_PACE,
// PACE_SPIN_US, DK_DYN, OSD); JIT_SAME, FRONT_MACRO_LEAN and FG_TIMED are read again at each boot.
// So the variables are set once, at the first boot, and a later boot that would need others (a
// changed setting, or Bananza after another game and back) restarts the program for that game.
namespace Core::Nx2PerfSettings {

struct Switches {
    bool split_gpu_thread = true;  ///< EDEN_NX2_BACK_STAGES=1 + EDEN_NX2_BACK_CUTS=a
    bool acquire_peek = true;      ///< EDEN_NX2_ACQUIRE_PEEK=1
    bool jit_same = true;          ///< EDEN_NX2_JIT_SAME=1
    bool front_macro_lean = true;  ///< EDEN_NX2_FRONT_MACRO_LEAN=1
    bool fg_timed = true;          ///< EDEN_NX2_FG_TIMED=1
    bool osd = true;               ///< EDEN_NX2_OSD=1
    bool dk_dyn = true;            ///< EDEN_NX2_DK_DYN=1, Bananza only
    bool dk_pacing = true;         ///< PRESENT_INTERVAL_WORD + MODE_PACE + PACE_SPIN_US, Bananza only

    bool operator==(const Switches&) const = default;
};

/// What the recommended launcher sets (and the first start's settings).
constexpr Switches Recommended() {
    return Switches{};
}

/// Like 2.0.0 (start-safe.bat): none of the switches.
constexpr Switches AllOff() {
    return Switches{false, false, false, false, false, false, false, false};
}

/// The keys of the [NX2Perf] group, one per setting.
struct Key {
    std::string_view name;
    bool Switches::*field;
};
inline constexpr std::array<Key, 8> Keys{{
    {"split_gpu_thread", &Switches::split_gpu_thread},
    {"acquire_peek", &Switches::acquire_peek},
    {"jit_same", &Switches::jit_same},
    {"front_macro_lean", &Switches::front_macro_lean},
    {"fg_timed", &Switches::fg_timed},
    {"osd", &Switches::osd},
    {"bananza_dynamic_fps", &Switches::dk_dyn},
    {"bananza_frame_pacing", &Switches::dk_pacing},
}};

/// Reads every setting through `read(key, default)`; a missing key gives its default (on).
template <typename Read>
Switches Load(Read&& read) {
    Switches switches{};
    for (const Key& key : Keys) {
        switches.*key.field = read(key.name, Recommended().*key.field);
    }
    return switches;
}

/// Writes every setting through `write(key, value, default)`.
template <typename Write>
void Save(const Switches& switches, Write&& write) {
    for (const Key& key : Keys) {
        write(key.name, switches.*key.field, Recommended().*key.field);
    }
}

/// The settings for Donkey Kong Bananza only take its title: the program id EDEN_NX2_DK_DYN's
/// patch checks (Loader::Nx2DkDynamicFps::ApplyIfEnabled).
constexpr bool IsBananza(u64 program_id) {
    return program_id == Loader::Nx2DkDynamicFps::ProgramId;
}

struct Var {
    std::string_view name;
    std::string_view value;

    bool operator==(const Var&) const = default;
};

/// Every variable a setting can set, in the order they are set.
inline constexpr std::array<std::string_view, 11> Names{
    "EDEN_NX2_BACK_STAGES",   "EDEN_NX2_BACK_CUTS",
    "EDEN_NX2_ACQUIRE_PEEK",  "EDEN_NX2_JIT_SAME",
    "EDEN_NX2_FRONT_MACRO_LEAN", "EDEN_NX2_FG_TIMED",
    "EDEN_NX2_OSD",           "EDEN_NX2_DK_DYN",
    "EDEN_NX2_PRESENT_INTERVAL_WORD", "EDEN_NX2_MODE_PACE",
    "EDEN_NX2_PACE_SPIN_US",
};

/// The variables the settings ask for when the game is (or is not) Bananza, the same values as
/// start-recommended.bat. The Bananza ones never for another game.
inline std::vector<Var> Wanted(const Switches& switches, bool bananza) {
    std::vector<Var> vars;
    if (switches.split_gpu_thread) {
        vars.push_back({"EDEN_NX2_BACK_STAGES", "1"});
        vars.push_back({"EDEN_NX2_BACK_CUTS", "a"});
    }
    if (switches.acquire_peek) {
        vars.push_back({"EDEN_NX2_ACQUIRE_PEEK", "1"});
    }
    if (switches.jit_same) {
        vars.push_back({"EDEN_NX2_JIT_SAME", "1"});
    }
    if (switches.front_macro_lean) {
        vars.push_back({"EDEN_NX2_FRONT_MACRO_LEAN", "1"});
    }
    if (switches.fg_timed) {
        vars.push_back({"EDEN_NX2_FG_TIMED", "1"});
    }
    if (switches.osd) {
        vars.push_back({"EDEN_NX2_OSD", "1"});
    }
    if (bananza && switches.dk_dyn) {
        vars.push_back({"EDEN_NX2_DK_DYN", "1"});
    }
    if (bananza && switches.dk_pacing) {
        vars.push_back({"EDEN_NX2_PRESENT_INTERVAL_WORD", "12:16:4"});
        vars.push_back({"EDEN_NX2_MODE_PACE", "2"});
        vars.push_back({"EDEN_NX2_PACE_SPIN_US", "1500"});
    }
    return vars;
}

/// What the program sets: the wanted variables but those already in its environment when it
/// started (a launcher, the user, or nx2_switches.txt set them: theirs wins, set or not by us).
/// `set_outside(name)` tells whether a name was there.
template <typename SetOutside>
std::vector<Var> Plan(const Switches& switches, bool bananza, SetOutside&& set_outside) {
    std::vector<Var> plan;
    for (const Var& var : Wanted(switches, bananza)) {
        if (!set_outside(var.name)) {
            plan.push_back(var);
        }
    }
    return plan;
}

} // namespace Core::Nx2PerfSettings
