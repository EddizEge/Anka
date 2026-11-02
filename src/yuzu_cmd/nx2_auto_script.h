// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace Nx2AutoScript {

// EDEN_NX2_AUTO_INPUT_FILE=<file>: timed presses on the virtual gamepad, for unattended runs.
// One item per line, '#' starts a comment:
//   <seconds> <key> <held seconds>                 key: A B X Y L R ZL ZR Plus Minus Up Down Left Right
//   <seconds> stick <L|R> <x> <y> <held seconds>   x and y in [-1, 1]
//   anchor boot | present | album                  seconds count from start-up, from the first
//                                                  present (default), or from the game's first
//                                                  SetAlbumImageOrientation call (Donkey Kong
//                                                  Bananza: its title menu); one anchor per file
// Names are case-insensitive. Bad lines are reported and skipped.

enum class Anchor { Boot, Present, Album };

struct Event {
    double start;    // seconds after the anchor
    double duration; // seconds held
    bool stick;
    int id;          // VirtualGamepad::VirtualButton, or VirtualStick for a stick
    float x;
    float y;
    std::string name; // "A", "stick L"
};

struct Script {
    Anchor anchor = Anchor::Present;
    std::vector<Event> events;
    std::vector<std::string> errors; // "line 3: ...", one per skipped line
};

Script ParseScript(std::string_view text);

// Presses and releases in time order; at the same time releases come first, so back to back
// presses of one key release it before pressing it again.
struct Action {
    double time;
    bool down;
    size_t event;
};

std::vector<Action> Schedule(const Script& script);

// EDEN_NX2_AUTO_EXIT=<seconds>[@boot|@present|@album]: close the window that many seconds after
// start-up (or the first present, or the first album orientation call), the same way as closing it
// by hand. "0" or empty is off.
struct ExitSpec {
    bool enabled = false;
    double seconds = 0.0;
    Anchor anchor = Anchor::Boot;
    std::string error; // set when the text is not valid; enabled is false then
};

ExitSpec ParseExit(std::string_view text);

const char* AnchorName(Anchor anchor);

} // namespace Nx2AutoScript
