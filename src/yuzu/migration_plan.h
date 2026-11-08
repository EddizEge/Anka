// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

// The data migration's decisions without Qt (yuzu/user_data_migration.cpp, migration_worker.cpp;
// src/tests/yuzu/migration_plan.cpp).
namespace MigrationPlan {

/// MigrationWorker::MigrationStrategy's values, which are the dialog's radio button ids.
enum class Strategy : int {
    Copy = 0,
    Move = 1,
    Link = 2,
};

/// What the dialog offers, and whether "Clear Shader Cache" starts checked.
struct Choices {
    bool move{};               ///< "Clear Old Data"
    bool link{};               ///< "Link Old Directory"
    bool clear_shader_cache{}; ///< checked by default

    constexpr bool operator==(const Choices&) const = default;
};

/// Portable (a `user` folder next to the program, as Anka ships): the data stays in the program's
/// folder, and the old emulator's folder is left as it was: only a copy, its shader cache kept
/// unless asked. Moving the old folder there or linking it emptied a user's yuzu folder (Anka #6).
constexpr Choices ChoicesFor(bool portable) {
    return portable ? Choices{false, false, false} : Choices{true, true, true};
}

/// The strategy the worker runs for the button the dialog returned (-1: none checked).
constexpr Strategy FromButton(int id, bool portable) {
    if (portable || id < static_cast<int>(Strategy::Copy) || id > static_cast<int>(Strategy::Link)) {
        return Strategy::Copy;
    }
    return static_cast<Strategy>(id);
}

/// The data folder before the migration replaces it.
enum class Target {
    Missing,      ///< not there
    Empty,        ///< only empty folders
    FirstRunOnly, ///< only the files the program writes before the prompt
    HasData,      ///< anything else: never removed
    Unreadable,   ///< could not be walked: never removed
};

/// The files the program writes before the prompt can run (ProfileManager's profiles.dat), relative
/// to the data folder. Compared as UTF-8: generic_string() throws for a file named in a script the
/// ANSI code page has not.
inline bool IsFirstRunFile(const std::filesystem::path& relative) {
    return relative.generic_u8string() ==
           u8"nand/system/save/8000000000000010/su/avators/profiles.dat";
}

inline Target Classify(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto status = fs::symlink_status(dir, ec);
    if (ec || status.type() == fs::file_type::not_found) {
        return status.type() == fs::file_type::not_found ? Target::Missing : Target::Unreadable;
    }
    if (status.type() != fs::file_type::directory) {
        return Target::HasData; // a file, a link or a junction to another folder
    }
    bool first_run_files = false;
    fs::recursive_directory_iterator it{dir, ec};
    if (ec) {
        return Target::Unreadable;
    }
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) {
            return Target::Unreadable;
        }
        const auto type = it->symlink_status(ec).type();
        if (ec) {
            return Target::Unreadable;
        }
        if (type == fs::file_type::directory) {
            continue;
        }
        if (type != fs::file_type::regular || !IsFirstRunFile(fs::relative(it->path(), dir, ec)) ||
            ec) {
            return Target::HasData;
        }
        first_run_files = true;
    }
    if (ec) {
        return Target::Unreadable;
    }
    return first_run_files ? Target::FirstRunOnly : Target::Empty;
}

/// Whether the migration may remove the data folder before it fills it.
constexpr bool MayReplace(Target target) {
    return target == Target::Missing || target == Target::Empty || target == Target::FirstRunOnly;
}

/// Whether `inner` is `outer` or inside it, by their names (trailing separators and "." / ".."
/// resolved, nothing on the disk). The config folder is inside the data folder on Windows; on
/// Linux and macOS out of portable mode it is beside it (~/.config/eden, ~/.local/share/eden), and
/// emptying the data folder leaves it: the prompt then needs both done.
inline bool IsInside(const std::filesystem::path& inner, const std::filesystem::path& outer) {
    const auto parts = [](const std::filesystem::path& path) {
        std::vector<std::filesystem::path> out;
        for (const auto& part : path.lexically_normal()) {
            if (!part.empty()) {
                out.push_back(part);
            }
        }
        return out;
    };
    const auto in = parts(inner);
    const auto out = parts(outer);
    return !out.empty() && out.size() <= in.size() &&
           std::equal(out.begin(), out.end(), in.begin());
}

} // namespace MigrationPlan
