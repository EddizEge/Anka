// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QTranslator>
#include "common/fs/fs.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "qt_common/qt_string_lookup.h"
#include "user_data_migration.h"
#include "yuzu/migration_dialog.h"
#include "yuzu/migration_plan.h"

// Needs to be included at the end due to https://bugreports.qt.io/browse/QTBUG-73263
#include <filesystem>
#include <QButtonGroup>
#include <QCheckBox>
#include <QGuiApplication>
#include <QProgressDialog>
#include <QRadioButton>
#include <QThread>

UserDataMigrator::UserDataMigrator(QMainWindow* main_window) {
    // NOTE: Logging is not initialized yet, do not produce logs here.

    // Check migration if config directory does not exist
    // TODO: ProfileManager messes with us a bit here, and force-creates the
    // /nand/system/save/8000000000000010/su/avators/profiles.dat file. Find a way to reorder
    // operations and have it create after this guy runs.
    if (!std::filesystem::is_directory(Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir))) {
        ShowMigrationPrompt(main_window);
    }
}

namespace {

// The data folder is the `user` folder next to the program (Anka's package ships one).
bool IsPortable() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    const fs::path portable = Common::FS::GetExeDirectory() / PORTABLE_DIR;
#else
    const fs::path portable = Common::FS::GetCurrentDir() / PORTABLE_DIR;
#endif
    std::error_code ec;
    return fs::equivalent(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir), portable, ec);
}

} // Anonymous namespace

void UserDataMigrator::ShowMigrationPrompt(QMainWindow* main_window) {
    namespace fs = std::filesystem;
    using namespace QtCommon::StringLookup;

    // Portable: only a copy, the shader cache kept by default (yuzu/migration_plan.h)
    const bool portable = IsPortable();
    const MigrationPlan::Choices choices = MigrationPlan::ChoicesFor(portable);

    MigrationDialog migration_prompt;
    migration_prompt.setWindowTitle(QObject::tr("Migration"));

    // mutually exclusive
    QButtonGroup* group = new QButtonGroup(&migration_prompt);

    // MACRO MADNESS, indeed

#define BUTTON(clazz, name, text, tooltip, checkState)                                             \
    clazz* name = new clazz(&migration_prompt);                                                    \
    name->setText(text);                                                                           \
    name->setToolTip(Lookup(tooltip));                                                             \
    name->setChecked(checkState);                                                                  \
    migration_prompt.addBox(name);

    BUTTON(QCheckBox, clear_shaders, QObject::tr("Clear Shader Cache"), MigrationTooltipClearShader,
           choices.clear_shader_cache)

#define RADIO(name, text, tooltip, checkState, strategy)                                           \
    BUTTON(QRadioButton, name, text, tooltip, checkState)                                          \
    group->addButton(name, static_cast<int>(MigrationWorker::MigrationStrategy::strategy));

    RADIO(keep_old, QObject::tr("Keep Old Data"), MigrationTooltipKeepOld, true, Copy)
    RADIO(clear_old, QObject::tr("Clear Old Data"), MigrationTooltipClearOld, false, Move)
    RADIO(link_old, QObject::tr("Link Old Directory"), MigrationTooltipLinkOld, false, Link)

#undef RADIO
#undef BUTTON

    if (!choices.move) {
        group->removeButton(clear_old);
        clear_old->hide();
    }
    if (!choices.link) {
        group->removeButton(link_old);
        link_old->hide();
    }

    std::vector<Emulator> found{};

    for (const Emulator& emu : legacy_emus)
        if (fs::is_directory(emu.get_user_dir()))
            found.emplace_back(emu);

    if (found.empty()) {
        return;
    }

    // makes my life easier
    qRegisterMetaType<Emulator>();

    QString prompt_text = Lookup(MigrationPromptPrefix);

    // natural language processing is a nightmare
    for (const Emulator& emu : found) {
        prompt_text = prompt_text % QStringLiteral("\n ") % emu.name();

        QAbstractButton* button = migration_prompt.addButton(emu.name());

        // This is cursed, but it's actually the most efficient way by a mile
        button->setProperty("emulator", QVariant::fromValue(emu));
    }

    prompt_text.append(QObject::tr("\n\n"));
    prompt_text = prompt_text % QStringLiteral("\n\n") % Lookup(MigrationPrompt);

    migration_prompt.setText(prompt_text);
    migration_prompt.addButton(QObject::tr("No"), true);

    migration_prompt.exec();

    QAbstractButton* button = migration_prompt.clickedButton();

    if (button->text() == QObject::tr("No")) {
        return ShowMigrationCancelledMessage(main_window);
    }

    const auto strategy = static_cast<MigrationWorker::MigrationStrategy>(
        MigrationPlan::FromButton(group->checkedId(), portable));

    selected_emu = button->property("emulator").value<Emulator>();

    MigrateUserData(main_window, clear_shaders->isChecked(), strategy);
}

void UserDataMigrator::ShowMigrationCancelledMessage(QMainWindow* main_window) {
    // Deleting only the config folder brings the prompt back, but this first start has already
    // written to the data folder (log/eden_log.txt): the migration then refuses to replace a data
    // folder that holds data (MigrationPlan::MayReplace), portable or not. Both after closing Anka:
    // its exit writes qt-config.ini again, and eden_log.txt can't move while it is open. With the
    // config folder outside the data folder (Linux / macOS, not portable) it has to go as well.
    using Common::FS::EdenPath;
    const auto data = Common::FS::GetEdenPath(EdenPath::EdenDir);
    const auto config = Common::FS::GetEdenPath(EdenPath::ConfigDir);
    const QString data_text =
        QString::fromStdString(Common::FS::GetEdenPathString(EdenPath::EdenDir));
    const QString text =
        MigrationPlan::IsInside(config, data)
            ? QObject::tr("After closing Anka, you can bring this prompt back by moving "
                          "everything in the data folder somewhere else:\n%1")
                  .arg(data_text)
            : QObject::tr("After closing Anka, you can bring this prompt back by deleting the "
                          "config folder:\n%1\nand moving everything in the data folder somewhere "
                          "else:\n%2")
                  .arg(QString::fromStdString(Common::FS::GetEdenPathString(EdenPath::ConfigDir)),
                       data_text);
    QMessageBox::information(main_window, QObject::tr("Migration"), text, QMessageBox::Ok);
}

void UserDataMigrator::MigrateUserData(QMainWindow* main_window, const bool clear_shader_cache,
                                       const MigrationWorker::MigrationStrategy strategy) {
    // Create a dialog to let the user know it's migrating
    QProgressDialog* progress = new QProgressDialog(main_window);
    progress->setWindowTitle(QObject::tr("Migrating"));
    progress->setLabelText(QObject::tr("Migrating, this may take a while..."));
    progress->setRange(0, 0);
    progress->setCancelButton(nullptr);
    progress->setWindowModality(Qt::WindowModality::ApplicationModal);

    QThread* thread = new QThread(main_window);
    MigrationWorker* worker = new MigrationWorker(selected_emu, clear_shader_cache, strategy);
    worker->moveToThread(thread);

    thread->connect(thread, &QThread::started, worker, &MigrationWorker::process);

    thread->connect(worker, &MigrationWorker::finished, progress,
                    [=, this](const QString& success_text, const std::string& path) {
                        progress->close();
                        QMessageBox::information(main_window, QObject::tr("Migration"),
                                                 success_text, QMessageBox::Ok);

                        migrated = true;
                        thread->quit();
                    });

    // Nothing was removed or the old data stayed where it was: no migration, the prompt comes back
    // at the next start while the config folder is missing.
    thread->connect(worker, &MigrationWorker::error, progress,
                    [=](const QString& error_message) {
                        progress->close();
                        QMessageBox::warning(main_window, QObject::tr("Migration"), error_message,
                                             QMessageBox::Ok);
                        thread->quit();
                    });

    thread->connect(worker, &MigrationWorker::finished, worker, &QObject::deleteLater);
    thread->connect(worker, &MigrationWorker::error, worker, &QObject::deleteLater);
    thread->connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    thread->start();
    progress->exec();
}
