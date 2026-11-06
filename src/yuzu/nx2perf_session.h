// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

#include <QString>

#include "common/common_types.h"

class QWidget;

// NX2Perf's settings (core/nx2_perf_settings.h, the NX2Perf tab) as EDEN_NX2_* variables in this
// process's environment. The switches are read once per process, so the variables are set once,
// before the first game boots; a later game that needs others (another title class for the
// Bananza settings, or changed settings) is started in a new process, asked first.
namespace Nx2PerfSession {

/// main(), first thing: the environment a restart gives the new process (before
/// nx2_switches.txt is loaded: the new process loads it itself).
void CaptureStartEnvironment();

/// main(), after nx2_switches.txt is loaded: the variables set outside the program (a launcher,
/// the user, the file). The settings never change those.
void CaptureOutsideSwitches();

/// Whether `name` was in the environment before the program set any (CaptureOutsideSwitches).
bool SetOutside(std::string_view name);

enum class Boot {
    Go,      ///< the variables are right for this game: boot it
    Restart, ///< RequestRestart was called: close the window, main() starts the game again
    Cancel,  ///< don't boot
};

/// Before a game boots: sets the variables the first time, else compares them with the ones
/// this process has and asks whether to restart. `game` is the file a restart starts with
/// "-g"; `applet` (a firmware applet, not an application): never asked, it runs with what the
/// session has (a restart would boot it as an application).
Boot Prepare(QWidget* parent, u64 program_id, const QString& game, bool fullscreen, bool applet);

/// The window refused to close after Prepare asked for a restart: no restart at exit.
void CancelRestart();

/// main(), after the event loop: starts the program again with the game, if Prepare asked.
void RestartIfRequested();

} // namespace Nx2PerfSession
