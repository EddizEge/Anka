// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/nx2perf_session.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStringList>

#include "common/logging.h"
#include "core/nx2_perf_settings.h"
#include "qt_common/config/uisettings.h"

namespace Nx2PerfSession {

namespace {

namespace Perf = Core::Nx2PerfSettings;

QProcessEnvironment g_start_environment;
std::vector<std::string_view> g_set_outside;

struct Applied {
    std::vector<Perf::Var> plan;
    bool bananza;
};
std::optional<Applied> g_applied; // set at the first boot

struct Restart {
    QString game;
    bool fullscreen;
};
std::optional<Restart> g_restart;

void SetVariable(const Perf::Var& var) {
    const std::string name{var.name};
    const std::string value{var.value};
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

QString Describe(const std::vector<Perf::Var>& plan) {
    QStringList lines;
    for (const auto& var : plan) {
        lines << QStringLiteral("%1=%2").arg(QString::fromUtf8(var.name.data(), var.name.size()),
                                             QString::fromUtf8(var.value.data(), var.value.size()));
    }
    return lines.isEmpty() ? QStringLiteral("none") : lines.join(QLatin1Char(' '));
}

} // Anonymous namespace

void CaptureStartEnvironment() {
    g_start_environment = QProcessEnvironment::systemEnvironment();
}

void CaptureOutsideSwitches() {
    g_set_outside.clear();
    for (const std::string_view name : Perf::Names) {
        if (std::getenv(std::string{name}.c_str()) != nullptr) {
            g_set_outside.push_back(name);
        }
    }
}

bool SetOutside(std::string_view name) {
    for (const std::string_view outside : g_set_outside) {
        if (outside == name) {
            return true;
        }
    }
    return false;
}

Boot Prepare(QWidget* parent, u64 program_id, const QString& game, bool fullscreen,
             bool applet) {
    const bool bananza = Perf::IsBananza(program_id);
    auto plan = Perf::Plan(UISettings::values.nx2perf, bananza, SetOutside);
    if (!g_applied) {
        for (const auto& var : plan) {
            SetVariable(var);
        }
        LOG_INFO(Frontend, "NX2Perf settings for {:016X}{}: {}", program_id,
                 bananza ? " (Donkey Kong Bananza)" : "", Describe(plan).toStdString());
        for (const std::string_view name : g_set_outside) {
            LOG_INFO(Frontend, "NX2Perf: {} was set outside the program, left as it is", name);
        }
        g_applied = Applied{std::move(plan), bananza};
        return Boot::Go;
    }
    if (plan == g_applied->plan) {
        return Boot::Go;
    }
    if (applet) {
        LOG_INFO(Frontend, "NX2Perf: applet {:016X} runs with this session's settings: {}",
                 program_id, Describe(g_applied->plan).toStdString());
        return Boot::Go;
    }

    QString why;
    if (bananza && !g_applied->bananza) {
        why = QObject::tr("Donkey Kong Bananza's own settings are off in this session, which "
                          "started with another game.");
    } else if (!bananza && g_applied->bananza) {
        why = QObject::tr("Donkey Kong Bananza's own settings are still on from Bananza; they "
                          "are only for Bananza.");
    } else {
        why = QObject::tr("The NX2Perf settings changed since this session's first game.");
    }
    QMessageBox box{parent};
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(QObject::tr("NX2Perf"));
    box.setText(QObject::tr("Restart to start this game with the right NX2Perf settings?"));
    box.setInformativeText(why + QStringLiteral("\n\n") +
                           QObject::tr("NX2Perf's settings are read once per session, so the "
                                       "program restarts and starts the game."));
    QPushButton* const restart = box.addButton(QObject::tr("Restart"), QMessageBox::AcceptRole);
    QPushButton* const anyway = box.addButton(QObject::tr("Start Anyway"), QMessageBox::NoRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(restart);
    box.exec();
    if (box.clickedButton() == restart) {
        LOG_INFO(Frontend, "NX2Perf: restarting for {:016X}: {}", program_id,
                 Describe(plan).toStdString());
        g_restart = Restart{game, fullscreen};
        return Boot::Restart;
    }
    if (box.clickedButton() == anyway) {
        LOG_INFO(Frontend, "NX2Perf: {:016X} started with this session's settings: {}",
                 program_id, Describe(g_applied->plan).toStdString());
        return Boot::Go;
    }
    return Boot::Cancel;
}

void CancelRestart() {
    if (g_restart) {
        LOG_INFO(Frontend, "NX2Perf: the window stayed open, no restart");
    }
    g_restart.reset();
}

void RestartIfRequested() {
    if (!g_restart) {
        return;
    }
    QStringList arguments;
    if (g_restart->fullscreen) {
        arguments << QStringLiteral("-f");
    }
    arguments << QStringLiteral("-g") << g_restart->game;
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(arguments);
    process.setProcessEnvironment(g_start_environment);
    if (!process.startDetached()) {
        LOG_ERROR(Frontend, "NX2Perf: the restart failed: {}",
                  process.errorString().toStdString());
    }
}

} // namespace Nx2PerfSession
