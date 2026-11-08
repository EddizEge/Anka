// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <QWidget>

#include "core/nx2_perf_settings.h"
#include "yuzu_cmd/anka_core_route.h"

class QCheckBox;
class QLabel;

namespace Ui {
class ConfigureNx2Perf;
}

// The Anka tab: a preset (the launchers' three setups, or Custom) and Anka's settings
// (core/nx2_perf_settings.h), one box each; a box changed by hand makes the preset Custom. A box
// whose variables were all set outside the program (yuzu/nx2perf_session.h) is greyed out: theirs
// is used. A setting Anka's second core does not take says "No effect in <its game>" under its
// text. Under the main core's game group, the second core's game group: its own options, one row
// per AnkaCoreRoute::SecondCoreOptions entry, which no preset changes.
class ConfigureNx2Perf : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureNx2Perf(QWidget* parent = nullptr);
    ~ConfigureNx2Perf() override;

    void ApplyConfiguration();

    /// A preset also sets Reactive Flushing in Graphics, Advanced, through this (false: it could
    /// not, a game is running).
    void SetReactiveFlushing(std::function<bool(bool, bool)> func);

    /// A game runs: Reactive Flushing and frame generation cannot change now; the tab keeps them
    /// for the restart or the game's end (Nx2PerfSession::AddPendingGraphics).
    void SetGameRunning(bool running);

private:
    struct Row {
        QCheckBox* box;
        QLabel* text; ///< the description under the box
        bool Core::Nx2PerfSettings::Switches::*field;
        std::vector<std::string_view> names;
    };

    /// A row of the second core's section, in AnkaCoreRoute::SecondCoreOptions' order.
    struct OptionRow {
        QWidget* input; ///< QCheckBox, QSpinBox or QComboBox, by the option's kind
        QLabel* label;  ///< beside a number or a choice; none for a box
        QLabel* text;   ///< the description under it
    };

    void changeEvent(QEvent* event) override;
    void RetranslateUI();
    void SetConfiguration();
    Core::Nx2PerfSettings::Switches Current() const;
    void Show(const Core::Nx2PerfSettings::Switches& switches);
    void ShowPreset();
    void ChoosePreset(Core::Nx2PerfSettings::Preset chosen);
    void BoxChanged();
    /// The tooltips: set outside the program.
    void MarkSetOutside();
    /// Under each setting Anka's second core does not take, a quiet "no effect" line. Once per
    /// text: after setupUi / retranslateUi and ShowDkDynConflict.
    void ShowSecondCoreNotes();
    /// A box's [Anka] key (Core::Nx2PerfSettings::Keys).
    static std::string_view KeyName(bool Core::Nx2PerfSettings::Switches::*field);
    void ShowFrameGen();
    void ChooseFrameGen(Core::Nx2PerfSettings::FrameGen chosen);
    void ShowPending();
    /// The "waiting" note while a game runs and a choice waits for it, else nothing.
    void ShowStatus();
    [[nodiscard]] bool Waiting() const;
    void ShowDkDynConflict();
    /// The second core's section: its rows from the table, once.
    void BuildSecondCoreOptions();
    void RetranslateSecondCoreOptions();
    void ShowSecondCoreOptions(const AnkaCoreRoute::SecondCoreValues& values);
    AnkaCoreRoute::SecondCoreValues CurrentSecondCoreOptions() const;

    std::unique_ptr<Ui::ConfigureNx2Perf> ui;
    std::vector<Row> rows;
    std::vector<OptionRow> option_rows;
    Core::Nx2PerfSettings::Preset preset = Core::Nx2PerfSettings::Preset::Recommended;
    std::function<bool(bool, bool)> reactive_flushing;
    bool game_running = false;
    Core::Nx2PerfSettings::FrameGen frame_gen_shown = Core::Nx2PerfSettings::FrameGen::Off;
    // Chosen in this dialog (written on every Apply: the Graphics tab, applied first, writes its
    // own view of the same settings each time)
    std::optional<Core::Nx2PerfSettings::FrameGen> frame_gen_choice;
    std::optional<bool> pending_reactive;
    std::optional<bool> pending_barrier;
};
