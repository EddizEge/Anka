// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include <QWidget>

#include "core/nx2_perf_settings.h"

class QCheckBox;

namespace Ui {
class ConfigureNx2Perf;
}

// The NX2Perf tab: NX2Perf's settings (core/nx2_perf_settings.h), one box each. A box whose
// variables were all set outside the program (yuzu/nx2perf_session.h) is greyed out: theirs is
// used.
class ConfigureNx2Perf : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureNx2Perf(QWidget* parent = nullptr);
    ~ConfigureNx2Perf() override;

    void ApplyConfiguration();

    /// "Recommended" also turns Reactive Flushing off in Graphics, Advanced, through this (false:
    /// it could not, a game is running).
    void SetReactiveFlushingOff(std::function<bool()> func);

private:
    struct Row {
        QCheckBox* box;
        bool Core::Nx2PerfSettings::Switches::*field;
        std::vector<std::string_view> names;
    };

    void changeEvent(QEvent* event) override;
    void RetranslateUI();
    void SetConfiguration();
    void Show(const Core::Nx2PerfSettings::Switches& switches);
    void MarkSetOutside();

    std::unique_ptr<Ui::ConfigureNx2Perf> ui;
    std::vector<Row> rows;
    std::function<bool()> reactive_flushing_off;
};
