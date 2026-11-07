// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/fs/symlink.h"
#include "migration_worker.h"

#include <filesystem>
#include <QMap>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>

#include "common/fs/path_util.h"
#include "yuzu/migration_plan.h"

static_assert(static_cast<int>(MigrationWorker::MigrationStrategy::Copy) ==
              static_cast<int>(MigrationPlan::Strategy::Copy));
static_assert(static_cast<int>(MigrationWorker::MigrationStrategy::Move) ==
              static_cast<int>(MigrationPlan::Strategy::Move));
static_assert(static_cast<int>(MigrationWorker::MigrationStrategy::Link) ==
              static_cast<int>(MigrationPlan::Strategy::Link));

MigrationWorker::MigrationWorker(const Emulator selected_emu_, const bool clear_shader_cache_,
                                 const MigrationStrategy strategy_)
    : QObject(), selected_emu(selected_emu_), clear_shader_cache(clear_shader_cache_),
      strategy(strategy_) {}

void MigrationWorker::process() {
    namespace fs = std::filesystem;
    constexpr auto copy_options = fs::copy_options::update_existing | fs::copy_options::recursive;

    const fs::path legacy_user_dir = selected_emu.get_user_dir();
    const fs::path legacy_config_dir = selected_emu.get_config_dir();
    const fs::path legacy_cache_dir = selected_emu.get_cache_dir();

    // TODO(crueter): Make these constexpr since they're defaulted
    fs::path eden_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir);
    fs::path config_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
    fs::path cache_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir);
    fs::path shader_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);

    eden_dir.make_preferred();
    config_dir.make_preferred();
    cache_dir.make_preferred();
    shader_dir.make_preferred();

    // The data folder is replaced only while it holds nothing of the user's: a portable `user`
    // folder with keys, saves or games in it is never removed (yuzu/migration_plan.h).
    const MigrationPlan::Target target = MigrationPlan::Classify(eden_dir);
    if (!MigrationPlan::MayReplace(target)) {
        emit error(tr("Nothing was migrated: %1 already holds data (or could not be read), and "
                      "the migration would replace it.\nMove its contents away first, or copy the "
                      "old data into it by hand.")
                       .arg(QString::fromStdU16String(eden_dir.u16string())));
        return;
    }
    try {
        fs::remove_all(eden_dir);
    } catch (fs::filesystem_error& _) {
        // directory may not exist at this point, that's fine
    }

    // A failed copy or move (fs::rename cannot move a folder to another drive) stops here; the
    // old data is where it was.
    try {
        switch (strategy) {
        case MigrationStrategy::Link:
            // Create symlinks/directory junctions if requested

            // Windows 11 has random permission nonsense to deal with.
            try {
                Common::FS::CreateSymlink(legacy_user_dir, eden_dir);
            } catch (const fs::filesystem_error& e) {
                emit error(tr("Linking the old directory failed. You may need to re-run with "
                              "administrative privileges on Windows.\nOS gave error: %1")
                               .arg(tr(e.what())));
                return; // the error is shown (user_data_migration.cpp), no longer std::exit(-1)
            }

            // Windows doesn't need any more links, because cache and config
            // are already children of the root directory
#ifndef WIN32
            if (fs::is_directory(legacy_config_dir)) {
                Common::FS::CreateSymlink(legacy_config_dir, config_dir);
            }

            if (fs::is_directory(legacy_cache_dir)) {
                Common::FS::CreateSymlink(legacy_cache_dir, cache_dir);
            }
#endif

            success_text.append(tr("\n\nNote that your configuration and data will be shared with %1.\n"
                                   "If this is not desirable, delete the following files:\n%2\n%3\n%4")
                                    .arg(selected_emu.name(), QString::fromStdU16String(eden_dir.u16string()),
                                         QString::fromStdU16String(config_dir.u16string()),
                                         QString::fromStdU16String(cache_dir.u16string())));

            break;
        case MigrationStrategy::Move:
            // Rename directories if deletion is requested (achieves the same result)
            fs::rename(legacy_user_dir, eden_dir);

            // Windows doesn't need any more renames, because cache and config
            // are already children of the root directory
#ifndef WIN32
            if (fs::is_directory(legacy_config_dir)) {
                fs::rename(legacy_config_dir, config_dir);
            }

            if (fs::is_directory(legacy_cache_dir)) {
                fs::rename(legacy_cache_dir, cache_dir);
            }
#endif
            break;
        case MigrationStrategy::Copy:
        default:
            // Default behavior: copy
            fs::copy(legacy_user_dir, eden_dir, copy_options);

            // Windows doesn't need any more copies, because cache and config
            // are already children of the root directory
#ifndef WIN32
            if (fs::is_directory(legacy_config_dir)) {
                fs::copy(legacy_config_dir, config_dir, copy_options);
            }

            if (fs::is_directory(legacy_cache_dir)) {
                fs::copy(legacy_cache_dir, cache_dir, copy_options);
            }
#endif

            success_text.append(tr("\n\nIf you wish to clean up the files which were left in the old "
                                   "data location, you can do so by deleting the following directory:\n"
                                   "%1")
                                    .arg(QString::fromStdU16String(legacy_user_dir.u16string())));
            break;
        }
    } catch (const fs::filesystem_error& e) {
        emit error(tr("Migrating from %1 failed; the old data was not removed.\nOS gave error: %2")
                       .arg(selected_emu.name(), QString::fromLocal8Bit(e.what())));
        return;
    }

    // Delete and re-create shader dir
    if (clear_shader_cache) {
        fs::remove_all(shader_dir);
        fs::create_directory(shader_dir);
    }

    emit finished(success_text, Common::FS::PathToUTF8String(legacy_user_dir));
}
