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
//   @boot | @present | @album <press line>         this press counts from that anchor instead of
//                                                  the file's: "@album 60 stick R 1 0 2"
//   until album                                    at the first album call, the presses on the
//                                                  boot and present clocks that have not started
//                                                  are dropped (keys already down are still
//                                                  released): "A every 3 s until the menu" then
//                                                  works however slowly the build starts
// Names are case-insensitive. Bad lines are reported and skipped.

enum class Anchor { Boot, Present, Album };

struct Event {
    Anchor anchor;   // the clock 'start' is on
    double start;    // seconds after the anchor
    double duration; // seconds held
    bool stick;
    int id;          // VirtualGamepad::VirtualButton, or VirtualStick for a stick
    float x;
    float y;
    std::string name; // "A", "stick L"
};

struct Script {
    Anchor anchor = Anchor::Present; // the file's, for presses without an '@' anchor
    bool until_album = false;
    std::vector<Event> events;
    std::vector<std::string> errors; // "line 3: ...", one per skipped line
};

Script ParseScript(std::string_view text);

// The presses and releases on one anchor's clock, in time order; at the same time releases come
// first, so back to back presses of one key release it before pressing it again.
struct Action {
    double time;
    bool down;
    size_t event;
};

std::vector<Action> Schedule(const Script& script, Anchor anchor);

// 'until album': removes the presses from actions[next] on and their releases, and returns how
// many presses it removed. The releases of presses before 'next' stay.
size_t DropUnstarted(std::vector<Action>& actions, size_t next);

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
