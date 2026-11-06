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
    QString game; ///< empty: the program's own command line again
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
    Perf::ForEachName([](std::string_view name) {
        if (std::getenv(std::string{name}.c_str()) != nullptr) {
            g_set_outside.push_back(name);
        }
    });
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
        LOG_INFO(Frontend, "Anka settings for {:016X}{}: {}", program_id,
                 bananza ? " (Donkey Kong Bananza)" : "", Describe(plan).toStdString());
        for (const std::string_view name : g_set_outside) {
            LOG_INFO(Frontend, "Anka: {} was set outside the program, left as it is", name);
        }
        g_applied = Applied{std::move(plan), bananza};
        return Boot::Go;
    }
    if (plan == g_applied->plan) {
        return Boot::Go;
    }
    if (applet) {
        LOG_INFO(Frontend, "Anka: applet {:016X} runs with this session's settings: {}",
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
        why = QObject::tr("The Anka settings changed since this session's first game.");
    }
    QMessageBox box{parent};
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(QObject::tr("Anka"));
    box.setText(QObject::tr("Restart to start this game with the right Anka settings?"));
    box.setInformativeText(why + QStringLiteral("\n\n") +
                           QObject::tr("Anka's settings are read once per session, so the "
                                       "program restarts and starts the game."));
    QPushButton* const restart = box.addButton(QObject::tr("Restart"), QMessageBox::AcceptRole);
    QPushButton* const anyway = box.addButton(QObject::tr("Start Anyway"), QMessageBox::NoRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(restart);
    box.exec();
    if (box.clickedButton() == restart) {
        LOG_INFO(Frontend, "Anka: restarting for {:016X}: {}", program_id,
                 Describe(plan).toStdString());
        g_restart = Restart{game, fullscreen};
        return Boot::Restart;
    }
    if (box.clickedButton() == anyway) {
        LOG_INFO(Frontend, "Anka: {:016X} started with this session's settings: {}",
                 program_id, Describe(g_applied->plan).toStdString());
        return Boot::Go;
    }
    return Boot::Cancel;
}

bool OfferRestart(QWidget* parent, const QString& running_game, bool fullscreen) {
    std::optional<std::vector<Perf::Var>> applied;
    bool bananza = false;
    if (g_applied) {
        applied = g_applied->plan;
        bananza = g_applied->bananza;
    }
    const auto plan = Perf::Plan(UISettings::values.nx2perf, bananza, SetOutside);
    if (!Perf::NeedsRestart(applied, plan)) {
        return false;
    }
    QString text = QObject::tr("This session's game already started with the earlier Anka "
                               "settings; the new ones take effect after a restart.");
    if (!running_game.isEmpty()) {
        text += QStringLiteral("\n\n") +
                QObject::tr("Anka opens again with the running game: unsaved game progress is "
                            "lost.");
    }
    QMessageBox box{parent};
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QObject::tr("Anka"));
    box.setText(QObject::tr("Restart Anka now to apply?"));
    box.setInformativeText(text);
    QPushButton* const restart = box.addButton(QObject::tr("Restart Now"), QMessageBox::AcceptRole);
    QPushButton* const later = box.addButton(QObject::tr("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(later);
    box.exec();
    if (box.clickedButton() != restart) {
        LOG_INFO(Frontend, "Anka: settings changed, restart later: {}", Describe(plan).toStdString());
        return false;
    }
    LOG_INFO(Frontend, "Anka: restarting to apply: {}", Describe(plan).toStdString());
    g_restart = Restart{running_game, fullscreen};
    return true;
}

void CancelRestart() {
    if (g_restart) {
        LOG_INFO(Frontend, "Anka: the window stayed open, no restart");
    }
    g_restart.reset();
}

void RestartIfRequested() {
    if (!g_restart) {
        return;
    }
    QStringList arguments;
    if (g_restart->game.isEmpty()) {
        arguments = QCoreApplication::arguments().mid(1); // the same command line
    } else {
        if (g_restart->fullscreen) {
            arguments << QStringLiteral("-f");
        }
        arguments << QStringLiteral("-g") << g_restart->game;
    }
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(arguments);
    process.setProcessEnvironment(g_start_environment);
    if (!process.startDetached()) {
        LOG_ERROR(Frontend, "Anka: the restart failed: {}",
                  process.errorString().toStdString());
    }
}

} // namespace Nx2PerfSession
