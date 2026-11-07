// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

#include "common/logging.h"
#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"
#include "hid_core/hid_util.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "yuzu/nx2_auto_run.h"

std::unique_ptr<Nx2AutoRun> Nx2AutoRun::StartFromEnvironment() {
    auto run = std::make_unique<Nx2AutoRun>();
    run->boot = std::chrono::steady_clock::now();

    if (const char* text = std::getenv("EDEN_NX2_AUTO_EXIT")) {
        run->exit = Nx2AutoScript::ParseExit(text);
        if (!run->exit.error.empty()) {
            LOG_ERROR(Frontend, "nx2 auto exit: EDEN_NX2_AUTO_EXIT '{}': {}, no automatic exit",
                      text, run->exit.error);
        } else if (run->exit.enabled) {
            LOG_INFO(Frontend, "nx2 auto exit: closing the window {:.2f} s after {}",
                     run->exit.seconds, Nx2AutoScript::AnchorName(run->exit.anchor));
        }
    }

    const char* path = std::getenv("EDEN_NX2_AUTO_INPUT_FILE");
    if (path != nullptr && *path != '\0') {
        std::ifstream file{path};
        if (!file) {
            LOG_ERROR(Frontend, "nx2 auto input: cannot read EDEN_NX2_AUTO_INPUT_FILE '{}'", path);
        } else {
            const std::string text{std::istreambuf_iterator<char>{file}, {}};
            run->script = Nx2AutoScript::ParseScript(text);
            for (const std::string& error : run->script.errors) {
                LOG_ERROR(Frontend, "nx2 auto input: {}: {} (skipped)", path, error);
            }
            for (const Anchor clock : Clocks) {
                run->actions[size_t(clock)] = Nx2AutoScript::Schedule(run->script, clock);
            }
            LOG_INFO(Frontend,
                     "nx2 auto input: {} presses from {} ({} after boot, {} after present, {} "
                     "after album; file anchor {}{}), {} bad lines",
                     run->script.events.size(), path,
                     run->actions[size_t(Anchor::Boot)].size() / 2,
                     run->actions[size_t(Anchor::Present)].size() / 2,
                     run->actions[size_t(Anchor::Album)].size() / 2,
                     Nx2AutoScript::AnchorName(run->script.anchor),
                     run->script.until_album ? ", until album" : "", run->script.errors.size());
        }
    }

    const bool any_input = std::any_of(run->actions.begin(), run->actions.end(),
                                       [](const auto& list) { return !list.empty(); });
    if (!run->exit.enabled && !any_input) {
        return nullptr;
    }
    return run;
}

void Nx2AutoRun::ClearInput() {
    for (auto& list : actions) {
        list.clear();
    }
}

void Nx2AutoRun::Bind(Core::System& system, InputCommon::InputSubsystem& input) {
    bound = true;
    if (input.GetVirtualGamepad() == nullptr) {
        LOG_ERROR(Frontend, "nx2 auto input: no virtual gamepad engine, the script is not run");
        ClearInput();
        return;
    }
    constexpr size_t HandheldPort =
        Service::HID::NpadIdTypeToIndex(Core::HID::NpadIdType::Handheld);
    auto& hid = system.HIDCore();
    auto* const player1 = hid.GetEmulatedController(Core::HID::NpadIdType::Player1);
    auto* const handheld = hid.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    if (player1 != nullptr && player1->IsConnected()) {
        LOG_INFO(Frontend, "nx2 auto input: player 1 is connected (style {}), virtual gamepad port 0",
                 static_cast<int>(player1->GetNpadStyleIndex()));
        port = 0;
        return;
    }
    if (handheld != nullptr && handheld->IsConnected()) {
        LOG_INFO(Frontend,
                 "nx2 auto input: player 1 is not connected but handheld is, virtual gamepad port {}",
                 HandheldPort);
        port = HandheldPort;
        return;
    }
    if (player1 != nullptr) {
        // Runtime state only: nothing writes it back to the settings or the config file.
        if (player1->GetNpadStyleIndex() == Core::HID::NpadStyleIndex::None) {
            player1->SetNpadStyleIndex(Core::HID::NpadStyleIndex::Fullkey);
        }
        player1->Connect();
    }
    if (player1 == nullptr || !player1->IsConnected()) {
        LOG_ERROR(Frontend,
                  "nx2 auto input: could not connect player 1, the script's presses reach no controller");
        ClearInput();
        return;
    }
    LOG_INFO(Frontend,
             "nx2 auto input: connected player 1 for this run (style {}), virtual gamepad port 0",
             static_cast<int>(player1->GetNpadStyleIndex()));
    port = 0;
}

// Seconds since the anchor; nothing before the first present (or album call) for those anchors.
std::optional<double> Nx2AutoRun::Seconds(Anchor anchor,
                                          std::chrono::steady_clock::time_point now) const {
    if (anchor == Anchor::Boot) {
        return std::chrono::duration<double>(now - boot).count();
    }
    const auto& start = anchor == Anchor::Album ? first_album : first_present;
    if (!start) {
        return std::nullopt;
    }
    return std::chrono::duration<double>(now - *start).count();
}

bool Nx2AutoRun::Tick(Core::System& system, InputCommon::InputSubsystem& input) {
    const auto now = std::chrono::steady_clock::now();
    const u64 presents = Core::g_nx2_present_count.load(std::memory_order_relaxed);
    if (!first_present && presents > 0) {
        first_present = now;
        LOG_INFO(Frontend, "nx2 auto: first present {:.2f} s after start-up",
                 std::chrono::duration<double>(now - boot).count());
    }
    if (!first_album && Core::g_nx2_album_count.load(std::memory_order_relaxed) > 0) {
        first_album = now;
        LOG_INFO(Frontend,
                 "nx2 auto: first album orientation call {:.2f} s after start-up (present #{})",
                 std::chrono::duration<double>(now - boot).count(), presents);
        if (script.until_album) {
            size_t dropped = 0;
            for (const Anchor clock : {Anchor::Boot, Anchor::Present}) {
                dropped += Nx2AutoScript::DropUnstarted(actions[size_t(clock)], next[size_t(clock)]);
            }
            LOG_INFO(Frontend, "nx2 auto input: until album: dropped {} presses that had not started",
                     dropped);
        }
    }
    if (exit.enabled && !exiting) {
        const auto elapsed = Seconds(exit.anchor, now);
        if (elapsed && *elapsed >= exit.seconds) {
            LOG_INFO(Frontend,
                     "nx2 auto exit after {:.2f} s (anchor {}, present #{}): closing the window",
                     *elapsed, Nx2AutoScript::AnchorName(exit.anchor), presents);
            exiting = true;
        }
    }
    if (exiting) {
        return true;
    }
    const auto pending = [this](Anchor clock) {
        return next[size_t(clock)] < actions[size_t(clock)].size();
    };
    if (std::none_of(Clocks.begin(), Clocks.end(), pending)) {
        return false;
    }
    if (!bound) {
        Bind(system, input);
    }
    auto* const pad = input.GetVirtualGamepad();
    for (const Anchor clock : Clocks) {
        const auto& list = actions[size_t(clock)];
        size_t& cursor = next[size_t(clock)];
        const auto elapsed = Seconds(clock, now);
        while (elapsed && cursor < list.size() && list[cursor].time <= *elapsed) {
            const Nx2AutoScript::Action& action = list[cursor++];
            const Nx2AutoScript::Event& event = script.events[action.event];
            const char* const anchor = Nx2AutoScript::AnchorName(clock);
            if (event.stick) {
                const float x = action.down ? event.x : 0.0f;
                const float y = action.down ? event.y : 0.0f;
                pad->SetStickPosition(port, event.id, x, y);
                LOG_INFO(Frontend,
                         "nx2 auto input: t={:.2f} s after {} (present #{}) {} {} ({:.2f}, {:.2f})",
                         *elapsed, anchor, presents, event.name, action.down ? "down" : "up", x, y);
            } else {
                pad->SetButtonState(port, event.id, action.down);
                LOG_INFO(Frontend, "nx2 auto input: t={:.2f} s after {} (present #{}) {} {}",
                         *elapsed, anchor, presents, event.name, action.down ? "down" : "up");
            }
        }
    }
    return false;
}
