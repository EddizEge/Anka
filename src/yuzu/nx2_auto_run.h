// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <vector>

#include "yuzu_cmd/nx2_auto_script.h"

namespace Core {
class System;
}
namespace InputCommon {
class InputSubsystem;
}

// EDEN_NX2_AUTO_INPUT_FILE and EDEN_NX2_AUTO_EXIT (yuzu_cmd/nx2_auto_script.h) in the menu
// program, as in yuzu_cmd: timed presses on the virtual gamepad and a close after a time, so the
// menu program can be measured unattended against the command-line one. The script's port of a
// player is always one of that player's inputs, next to the configured ones; player 1 is only
// connected for this run when it is not.
class Nx2AutoRun {
public:
    /// Null when neither variable is set (or the input file cannot be read and there is no exit).
    static std::unique_ptr<Nx2AutoRun> StartFromEnvironment();

    /// From a timer on the GUI thread while the game runs. Returns true once the automatic exit
    /// is due.
    bool Tick(Core::System& system, InputCommon::InputSubsystem& input);

private:
    using Anchor = Nx2AutoScript::Anchor;
    static constexpr std::array<Anchor, 3> Clocks{Anchor::Boot, Anchor::Present, Anchor::Album};

    void Bind(Core::System& system, InputCommon::InputSubsystem& input);
    void ClearInput();
    std::optional<double> Seconds(Anchor anchor, std::chrono::steady_clock::time_point now) const;

    Nx2AutoScript::Script script;
    Nx2AutoScript::ExitSpec exit;
    bool exiting = false;
    // One schedule and cursor per clock, by Anchor value.
    std::array<std::vector<Nx2AutoScript::Action>, Clocks.size()> actions;
    std::array<size_t, Clocks.size()> next{};
    bool bound = false;
    size_t port = 0;
    std::chrono::steady_clock::time_point boot;
    std::optional<std::chrono::steady_clock::time_point> first_present;
    std::optional<std::chrono::steady_clock::time_point> first_album;
};
