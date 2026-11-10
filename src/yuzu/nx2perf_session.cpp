// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/nx2perf_session.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QStringList>

#include "common/logging.h"
#include "common/nx2_early_switches.h"
#include "common/assert.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/nx2_perf_settings.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/qt_common.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Nx2PerfSession {

namespace {

namespace Perf = Core::Nx2PerfSettings;

// A restarted process waits for this process id (the old one) before it opens the log.
constexpr char RESTART_PID_VARIABLE[] = "ANKA_RESTART_WAIT_PID";
constexpr int RESTART_WAIT_MS = 30'000;

QProcessEnvironment g_start_environment;
std::vector<std::string_view> g_set_outside;
PendingGraphics g_pending;
// What "Later" was answered for: asked again only when the settings differ from it
PendingGraphics g_pending_asked;
std::optional<std::vector<Perf::Var>> g_plan_asked;

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

bool SamePending(const PendingGraphics& a, const PendingGraphics& b) {
    return a.reactive_flushing == b.reactive_flushing &&
           a.barrier_feedback_loops == b.barrier_feedback_loops && a.frame_gen == b.frame_gen;
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
    // The log is not open yet: nothing is logged here.
    if (const char* pid_text = std::getenv(RESTART_PID_VARIABLE)) {
#ifdef _WIN32
        const unsigned long pid = std::strtoul(pid_text, nullptr, 10);
        if (pid != 0) {
            if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid))) {
                WaitForSingleObject(process, RESTART_WAIT_MS);
                CloseHandle(process);
            }
        }
        _putenv_s(RESTART_PID_VARIABLE, "");
#else
        (void)pid_text;
        unsetenv(RESTART_PID_VARIABLE);
#endif
    }
    g_start_environment = QProcessEnvironment::systemEnvironment();
    g_start_environment.remove(QString::fromLatin1(RESTART_PID_VARIABLE));
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
        // A few switches were read before main (Safe's GPU shortcuts among them): read them again
        // now, before the game's threads start; nothing has used them yet.
        ASSERT_MSG(!QtCommon::system || !QtCommon::system->IsPoweredOn(),
                   "Anka: the early switches are read again while emulation runs");
        const std::size_t reloaded = Common::Nx2EarlySwitches::ReloadAll();
        LOG_INFO(Frontend, "Anka settings for {:016X}{}: {}", program_id,
                 bananza ? " (Donkey Kong Bananza)" : "", Describe(plan).toStdString());
        LOG_INFO(Frontend, "Anka: switches read before main read again ({} places)", reloaded);
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
    const bool pending = HasPendingGraphics();
    // After "Later" only a change since then asks again (back to the running values: nothing)
    const bool plan_new = Perf::NeedsRestart(applied, plan) && plan != g_plan_asked;
    const bool pending_new = applied && pending && !SamePending(g_pending, g_pending_asked);
    if (!plan_new && !pending_new) {
        return false;
    }
    QString text = QObject::tr("This session's game already started with the earlier Anka "
                               "settings; the new ones take effect after a restart.");
    if (pending) {
        text += QStringLiteral("\n\n") +
                QObject::tr("Reactive Flushing and frame generation cannot change in a running "
                            "game: they are applied before the restart, or when the game stops "
                            "if you choose Later.");
    }
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
        g_plan_asked = plan;
        g_pending_asked = g_pending;
        return false;
    }
    LOG_INFO(Frontend, "Anka: restarting to apply: {}", Describe(plan).toStdString());
    g_restart = Restart{running_game, fullscreen};
    return true;
}

void AddPendingGraphics(const PendingGraphics& pending) {
    if (pending.reactive_flushing) {
        g_pending.reactive_flushing = pending.reactive_flushing;
    }
    if (pending.barrier_feedback_loops) {
        g_pending.barrier_feedback_loops = pending.barrier_feedback_loops;
    }
    if (pending.frame_gen) {
        g_pending.frame_gen = pending.frame_gen;
    }
    // A choice back to what the running game has (the global settings) is nothing to wait for
    const auto& v = Settings::values;
    if (g_pending.reactive_flushing == v.use_reactive_flushing.GetValue(true)) {
        g_pending.reactive_flushing.reset();
    }
    if (g_pending.barrier_feedback_loops == v.barrier_feedback_loops.GetValue(true)) {
        g_pending.barrier_feedback_loops.reset();
    }
    if (g_pending.frame_gen == CurrentFrameGen()) {
        g_pending.frame_gen.reset();
    }
}

const PendingGraphics& Pending() {
    return g_pending;
}

bool HasPendingGraphics() {
    return g_pending.reactive_flushing || g_pending.barrier_feedback_loops || g_pending.frame_gen;
}

bool ApplyPendingGraphics() {
    if (!HasPendingGraphics()) {
        return false;
    }
    if (g_pending.reactive_flushing) {
        Settings::values.use_reactive_flushing.SetValue(*g_pending.reactive_flushing);
    }
    if (g_pending.barrier_feedback_loops) {
        Settings::values.barrier_feedback_loops.SetValue(*g_pending.barrier_feedback_loops);
    }
    if (g_pending.frame_gen) {
        ApplyFrameGen(*g_pending.frame_gen);
    }
    LOG_INFO(Frontend, "Anka: graphics settings chosen during the game applied: reactive flushing "
                       "{}, barrier feedback loops {}, frame generation {}",
             g_pending.reactive_flushing ? (*g_pending.reactive_flushing ? "on" : "off") : "-",
             g_pending.barrier_feedback_loops ? (*g_pending.barrier_feedback_loops ? "on" : "off")
                                              : "-",
             g_pending.frame_gen ? static_cast<int>(*g_pending.frame_gen) : -1);
    g_pending = {};
    g_pending_asked = {};
    return true;
}

Perf::FrameGen CurrentFrameGen() {
    const auto& v = Settings::values;
    return Perf::FrameGenFrom({.on = v.frame_gen.GetValue(true),
                               .fsr3 = v.frame_gen_type.GetValue(true) ==
                                       Settings::FrameGenType::Fsr3,
                               .multiplier = v.frame_gen_multiplier.GetValue(true),
                               .target_rate = v.frame_gen_target_rate.GetValue(true)});
}

void ApplyFrameGen(Perf::FrameGen choice) {
    auto& v = Settings::values;
    const Perf::FrameGenSettings now =
        Perf::FrameGenApply(choice, {.on = v.frame_gen.GetValue(true),
                                     .fsr3 = v.frame_gen_type.GetValue(true) ==
                                             Settings::FrameGenType::Fsr3,
                                     .multiplier = v.frame_gen_multiplier.GetValue(true),
                                     .target_rate = v.frame_gen_target_rate.GetValue(true)});
    v.frame_gen.SetValue(now.on);
    if (now.on) {
        if (now.fsr3) {
            v.frame_gen_type.SetValue(Settings::FrameGenType::Fsr3);
        }
        v.frame_gen_multiplier.SetValue(now.multiplier);
        v.frame_gen_target_rate.SetValue(now.target_rate);
    }
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
    QProcessEnvironment environment = g_start_environment;
    // The new process waits for this one to exit before it opens the log (this one still holds
    // it, and saves the settings on its way out).
    environment.insert(QString::fromLatin1(RESTART_PID_VARIABLE),
                       QString::number(QCoreApplication::applicationPid()));
    process.setProcessEnvironment(environment);
#ifdef _WIN32
    // Qt passes bInheritHandles=TRUE: the new process would keep this one's inheritable handles
    // open (the log file, saves, game files) after this one exits.
    // Nor may it name this one's standard handles (STARTF_USESTDHANDLES, which Qt sets): they are
    // not inherited, so in the new process they would be invalid or someone else's handles.
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->inheritHandles = false;
        args->startupInfo->dwFlags &= ~STARTF_USESTDHANDLES;
        args->startupInfo->hStdInput = nullptr;
        args->startupInfo->hStdOutput = nullptr;
        args->startupInfo->hStdError = nullptr;
    });
#endif
    if (!process.startDetached()) {
        LOG_ERROR(Frontend, "Anka: the restart failed: {}",
                  process.errorString().toStdString());
    }
}

} // namespace Nx2PerfSession
