// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_nx2perf.h"

#include <cstdlib>
#include <string>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QSignalBlocker>
#include <QStringList>

#include "qt_common/config/uisettings.h"
#include "ui_configure_nx2perf.h"
#include "yuzu/nx2perf_session.h"

namespace Perf = Core::Nx2PerfSettings;

ConfigureNx2Perf::ConfigureNx2Perf(QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigureNx2Perf>()} {
    ui->setupUi(this);

    using S = Perf::Switches;
    rows = {
        {ui->split_gpu_thread, &S::split_gpu_thread, {"EDEN_NX2_BACK_STAGES", "EDEN_NX2_BACK_CUTS"}},
        {ui->acquire_peek, &S::acquire_peek, {"EDEN_NX2_ACQUIRE_PEEK"}},
        {ui->jit_same, &S::jit_same, {"EDEN_NX2_JIT_SAME"}},
        {ui->front_macro_lean, &S::front_macro_lean, {"EDEN_NX2_FRONT_MACRO_LEAN"}},
        {ui->fg_timed, &S::fg_timed, {"EDEN_NX2_FG_TIMED"}},
        {ui->osd, &S::osd, {"EDEN_NX2_OSD"}},
        {ui->dk_dyn, &S::dk_dyn, {"EDEN_NX2_DK_DYN"}},
        {ui->dk_pacing,
         &S::dk_pacing,
         {"EDEN_NX2_PRESENT_INTERVAL_WORD", "EDEN_NX2_MODE_PACE", "EDEN_NX2_PACE_SPIN_US"}},
    };

    SetConfiguration();
    MarkSetOutside();

    // The preset's item order is Perf::Preset's (configure_nx2perf.ui).
    connect(ui->preset, &QComboBox::activated, this,
            [this](int index) { ChoosePreset(static_cast<Perf::Preset>(index)); });
    for (const Row& row : rows) {
        connect(row.box, &QCheckBox::toggled, this, [this] { BoxChanged(); });
    }
}

ConfigureNx2Perf::~ConfigureNx2Perf() = default;

void ConfigureNx2Perf::SetReactiveFlushing(std::function<bool(bool, bool)> func) {
    reactive_flushing = std::move(func);
}

void ConfigureNx2Perf::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureNx2Perf::RetranslateUI() {
    ui->retranslateUi(this);
    ShowPreset();
    MarkSetOutside();
}

void ConfigureNx2Perf::SetConfiguration() {
    Show(UISettings::values.nx2perf);
}

Perf::Switches ConfigureNx2Perf::Current() const {
    Perf::Switches switches{};
    for (const Row& row : rows) {
        switches.*row.field = row.box->isChecked();
    }
    switches.preset = preset;
    return switches;
}

void ConfigureNx2Perf::Show(const Perf::Switches& switches) {
    for (const Row& row : rows) {
        const QSignalBlocker blocker{row.box};
        row.box->setChecked(switches.*row.field);
    }
    preset = switches.preset;
    ShowPreset();
}

void ConfigureNx2Perf::ShowPreset() {
    {
        const QSignalBlocker blocker{ui->preset};
        ui->preset->setCurrentIndex(static_cast<int>(preset));
    }
    switch (preset) {
    case Perf::Preset::Recommended:
        ui->preset_text->setText(tr("All settings on, Reactive Flushing off (Graphics, Advanced): "
                                    "the setup Anka was measured with. Like start-recommended.bat."));
        break;
    case Perf::Preset::Reactive:
        ui->preset_text->setText(
            tr("All settings on, Reactive Flushing and barrier feedback loops on, RX_RT_FIT=3. "
               "Experimental: on our PC faster in open areas, slower at the opening. Like "
               "start-reactive-experimental.bat."));
        break;
    case Perf::Preset::Safe:
        ui->preset_text->setText(tr("All settings below off, and the GPU shortcuts Anka keeps on by "
                                    "default off too (the variables start-safe.bat sets to 0). "
                                    "Graphics settings stay as they are."));
        break;
    case Perf::Preset::Custom:
        ui->preset_text->setText(tr("Your own choice of the settings below."));
        break;
    }
}

void ConfigureNx2Perf::ChoosePreset(Perf::Preset chosen) {
    ui->status->clear();
    Show(Perf::FromPreset(chosen, Current()));
    const auto reactive = Perf::ReactiveFlushing(chosen);
    if (!reactive) {
        return;
    }
    const bool barrier_on = Perf::BarrierFeedbackLoops(chosen).value_or(false);
    if (!reactive_flushing || !reactive_flushing(*reactive, barrier_on)) {
        ui->status->setText(tr("Reactive Flushing (Graphics, Advanced) can only be changed with no "
                               "game running; set it there afterwards."));
    }
}

void ConfigureNx2Perf::BoxChanged() {
    const Perf::Switches switches = Perf::AfterBoxChange(Current());
    if (switches.preset != preset) {
        preset = switches.preset;
        ShowPreset();
    }
}

void ConfigureNx2Perf::ApplyConfiguration() {
    UISettings::values.nx2perf = Current();
}

void ConfigureNx2Perf::MarkSetOutside() {
    for (const Row& row : rows) {
        QStringList outside;
        for (const std::string_view name : row.names) {
            if (Nx2PerfSession::SetOutside(name)) {
                // The value now: the program never sets one that was there at its start.
                const std::string key{name};
                const char* value = std::getenv(key.c_str());
                outside << QStringLiteral("%1=%2").arg(QString::fromStdString(key),
                                                       QString::fromUtf8(value ? value : ""));
            }
        }
        // All set outside: the box changes nothing, its value is theirs.
        const bool all_outside = outside.size() == static_cast<qsizetype>(row.names.size());
        row.box->setEnabled(!all_outside);
        if (outside.isEmpty()) {
            row.box->setToolTip(QString{});
        } else if (all_outside) {
            row.box->setToolTip(tr("Set outside the program (your environment, a .bat of your own "
                                   "or nx2_switches.txt), so this is used whatever the box says: "
                                   "%1")
                                    .arg(outside.join(QStringLiteral(", "))));
        } else {
            row.box->setToolTip(tr("Partly set outside the program, and that part is used: %1. "
                                   "The box sets the rest.")
                                    .arg(outside.join(QStringLiteral(", "))));
        }
    }
}
