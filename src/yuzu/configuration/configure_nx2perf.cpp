// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/configuration/configure_nx2perf.h"

#include <cstdlib>
#include <string>
#include <utility>

#include <QCheckBox>
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

    connect(ui->recommended, &QPushButton::clicked, this, [this] {
        Show(Perf::Recommended());
        if (reactive_flushing_off && reactive_flushing_off()) {
            ui->status->setText(tr("All on, and Reactive Flushing off in Graphics, Advanced. "
                                   "Press OK to keep it."));
        } else {
            ui->status->setText(tr("All on. Reactive Flushing (Graphics, Advanced) can only be "
                                   "turned off with no game running. Press OK to keep it."));
        }
    });
    connect(ui->all_off, &QPushButton::clicked, this, [this] {
        Show(Perf::AllOff());
        ui->status->setText(tr("All off. Press OK to keep it."));
    });
}

ConfigureNx2Perf::~ConfigureNx2Perf() = default;

void ConfigureNx2Perf::SetReactiveFlushingOff(std::function<bool()> func) {
    reactive_flushing_off = std::move(func);
}

void ConfigureNx2Perf::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureNx2Perf::RetranslateUI() {
    ui->retranslateUi(this);
    MarkSetOutside();
}

void ConfigureNx2Perf::SetConfiguration() {
    Show(UISettings::values.nx2perf);
}

void ConfigureNx2Perf::Show(const Perf::Switches& switches) {
    for (const Row& row : rows) {
        row.box->setChecked(switches.*row.field);
    }
}

void ConfigureNx2Perf::ApplyConfiguration() {
    Perf::Switches switches = UISettings::values.nx2perf;
    for (const Row& row : rows) {
        switches.*row.field = row.box->isChecked();
    }
    UISettings::values.nx2perf = switches;
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
