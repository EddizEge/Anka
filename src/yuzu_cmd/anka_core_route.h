// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Anka 1.1 ships two cores: this program (Donkey Kong Bananza and every other game) and a second
// core next to it for The Legend of Zelda: Ocarina of Time. The menu (yuzu/main_window.cpp) and
// anka-cli (yuzu_cmd/yuzu.cpp) start the second core for that game; the decisions without Qt or
// the emulator (src/tests/yuzu/anka_core_route.cpp). The contract is the second core's
// (anka-birlesim 455f2a90cd): ANKA_USER_DIR, -f / -c / -g, exit codes 0 / 1 / 75 / 76.
//
// Every name the second core goes by (its folder, programs, settings file, cache and log folders,
// lock) and every text the user reads about it are here and only here: a rename is one commit.
namespace AnkaCoreRoute {

/// The second core's file names (the contract with it).
namespace Names {
constexpr std::string_view Folder = "zelda";              ///< <package>/<Folder>
constexpr std::string_view CliProgram = "anka-zelda-cli.exe";
constexpr std::string_view QtProgram = "anka-zelda.exe";  ///< with a window of its own; none in 1.1
constexpr std::string_view Ini = "sdl2-config-zelda.ini"; ///< <user>/config/<Ini>
constexpr std::string_view CacheFolder = "zelda";         ///< <user>/cache/<CacheFolder>: its caches
constexpr std::string_view LogFolder = "zelda";           ///< <user>/log/<LogFolder>/eden_log.txt
constexpr std::string_view LogFile = "eden_log.txt";
constexpr std::string_view Lock = "Local\\AnkaZeldaCore"; ///< its single instance (exit 76)
} // namespace Names

/// What the user reads about the second core (English, as the rest of Anka). No game's name.
namespace Text {
constexpr const char* Starting = "Starting this game on Anka's second core";
constexpr const char* Closed = "Anka's second core closed";
constexpr const char* StillRunning = "Another game is still running on Anka's second core. Close "
                                     "it before starting another game.";
constexpr const char* StopRunningFirst =
    "Stop the running game first: this game runs on Anka's second core.";
constexpr const char* PropertiesNote = "These settings have no effect on this game's core.";
constexpr const char* Missing = "Anka's second core is missing: %1 was not found. Extract the "
                                "whole Anka package.";
constexpr const char* IniNotWritten =
    "The settings of Anka's second core could not be written to %1.";
constexpr const char* NotStarted = "Anka's second core could not be started: %1";
constexpr const char* StartFailed = "The game could not start on Anka's second core. The details "
                                    "are in its log:\n%1";
constexpr const char* AlreadyRunning = "Anka's second core is already running.";
constexpr const char* Crashed = "Anka's second core closed unexpectedly (exit code 0x%1). Its "
                                "log:\n%2";
/// The Anka tab, under a setting the second core does not take (ReachesSecondCore)
constexpr const char* NoEffectOnSecondCore = "Anka's second core: no effect";
/// The Anka tab's preset: what it means on the second core (SecondCorePresetVars)
constexpr const char* PresetOnSecondCore =
    "On Anka's second core Reactive Flushing stays off, so both Recommended presets are the "
    "same there; Safe also turns off its GPU shortcuts.";
} // namespace Text

/// The game the second core plays (its updates share the base id's upper bits).
constexpr std::uint64_t SecondCoreProgramId = 0x0400486024DEC000ULL;
/// SecondCoreProgramId as [DisabledAddOns] stores it.
constexpr std::uint64_t SecondCoreTitleId = SecondCoreProgramId;

constexpr bool RunsOnSecondCore(std::uint64_t program_id) {
    return (program_id & ~std::uint64_t{0xFFF}) == SecondCoreProgramId;
}

/// The second core in the package: Names::QtProgram when the menu starts it and it is there, else
/// Names::CliProgram. Empty: neither is there.
inline std::optional<std::filesystem::path> FindSecondCore(const std::filesystem::path& package_dir,
                                                           bool prefer_qt) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = package_dir / Names::Folder;
    if (prefer_qt && fs::is_regular_file(dir / Names::QtProgram, ec)) {
        return dir / Names::QtProgram;
    }
    if (fs::is_regular_file(dir / Names::CliProgram, ec)) {
        return dir / Names::CliProgram;
    }
    return std::nullopt;
}

/// The second core's settings file, written by the starting program at every start.
inline std::filesystem::path SecondCoreIniPath(const std::filesystem::path& user_dir) {
    return user_dir / "config" / Names::Ini;
}

/// The second core's log (for the messages after a failed start).
inline std::filesystem::path SecondCoreLogPath(const std::filesystem::path& user_dir) {
    return user_dir / "log" / Names::LogFolder / Names::LogFile;
}

/// One Anka tab setting on the second core: the variable it sets there while its box is on.
struct SecondCoreSetting {
    std::string_view key; ///< the [Anka] key (Core::Nx2PerfSettings::Keys)
    std::string_view env;
    std::string_view value;
};

/// The Anka tab's settings the second core reads (anka-birlesim's table, 08.11.2026: every
/// variable's reader looked up in its tree). The others have no counterpart there and set
/// nothing: split_gpu_thread (the second core splits its GPU thread itself, EDEN_NX2_SPLIT, on by
/// default), acquire_peek, jit_same, front_macro_lean, bananza_dynamic_fps, bananza_frame_pacing.
/// Frame generation goes over in its settings file (MakeSecondCoreIni), not here.
inline constexpr std::array<SecondCoreSetting, 2> SecondCoreSettings{{
    {"fg_timed", "EDEN_NX2_FG_TIMED", "1"},
    {"osd", "EDEN_NX2_OSD", "1"},
}};

/// A preset's variable on the second core (Core::Nx2PerfSettings::Preset as an int).
struct SecondCorePresetVar {
    int preset;
    std::string_view env;
    std::string_view value;
};

/// The presets on the second core. Recommended: the boxes alone. Reactive: the same
/// (EDEN_NX2_RX_RT_FIT has no reader there, and its settings file keeps Reactive Flushing off).
/// Safe: the boxes and the part of Core::Nx2PerfSettings::SafeOff it reads, at 0. Custom: the
/// boxes. Never EDEN_NX2_PBFLUSH_WINDOW_KIB=256, the main core's window: it halved the second
/// core's frame rate and made it stutter.
inline constexpr int SafePreset = 2; ///< Core::Nx2PerfSettings::Preset::Safe
inline constexpr std::array<SecondCorePresetVar, 8> SecondCorePresetVars{{
    {SafePreset, "EDEN_NX2_UBO_BIND_SKIP", "0"},
    {SafePreset, "EDEN_NX2_MARK_WORDS", "0"},
    {SafePreset, "EDEN_NX2_SLOT_SKIP", "0"},
    {SafePreset, "EDEN_NX2_TICK_SKIP", "0"},
    {SafePreset, "EDEN_NX2_HLE_INDIRECT", "0"},
    {SafePreset, "EDEN_NX2_LAYOUT_MEMO", "0"},
    {SafePreset, "EDEN_NX2_FAST_PRIORITY_SCAN", "0"},
    {SafePreset, "EDEN_NX2_PBFLUSH_WINDOW_KIB", "0"},
}};

/// Whether an Anka tab setting does anything on the second core (else the tab marks it
/// Text::NoEffectOnSecondCore).
inline bool ReachesSecondCore(std::string_view key,
                              std::span<const SecondCoreSetting> table = SecondCoreSettings) {
    return std::any_of(table.begin(), table.end(),
                       [key](const SecondCoreSetting& setting) { return setting.key == key; });
}

/// The second core's variables for the Anka tab's state: `box_on(key)` for every setting of the
/// table, then the preset's.
template <typename BoxOn>
std::vector<std::pair<std::string, std::string>> SecondCoreEnv(
    BoxOn&& box_on, int preset, std::span<const SecondCoreSetting> settings = SecondCoreSettings,
    std::span<const SecondCorePresetVar> presets = SecondCorePresetVars) {
    std::vector<std::pair<std::string, std::string>> env;
    for (const auto& setting : settings) {
        if (box_on(setting.key)) {
            env.emplace_back(setting.env, setting.value);
        }
    }
    for (const auto& var : presets) {
        if (var.preset == preset) {
            env.emplace_back(var.env, var.value);
        }
    }
    return env;
}

/// Variables of the starting program's environment the second core does not get: Anka's switches
/// and the fork's diagnostics (EDEN_*; a launcher of the main core sets dozens) and the NVIDIA
/// driver's shader cache settings (__GL_SHADER_DISK_CACHE*), as the second core's own test
/// launchers clear them. Its own (PlanSecondCore's) are set afterwards.
constexpr bool ClearedForSecondCore(std::string_view name) {
    return name.starts_with("EDEN_") || name.starts_with("__GL_SHADER_DISK_CACHE");
}

struct Launch {
    std::filesystem::path program;
    std::filesystem::path working_dir;
    std::vector<std::string> args; ///< UTF-8, without the program
    std::vector<std::pair<std::string, std::string>> env;
};

/// The second core's command line: `-f` when full screen, `-c <user>/config/<Names::Ini>`,
/// `-g <game>`; ANKA_USER_DIR always, then `settings_env` (SecondCoreEnv), then
/// EDEN_CLI_RESTART_HANDOFF=1 only to restart after exit 75.
inline Launch PlanSecondCore(const std::filesystem::path& core,
                             const std::filesystem::path& user_dir, const std::string& game_utf8,
                             bool fullscreen, bool restart_handoff,
                             const std::vector<std::pair<std::string, std::string>>& settings_env =
                                 {}) {
    Launch launch;
    launch.program = std::filesystem::path{core}.make_preferred();
    launch.working_dir = launch.program.parent_path();
    if (fullscreen) {
        launch.args.emplace_back("-f");
    }
    launch.args.emplace_back("-c");
    const auto ini = SecondCoreIniPath(user_dir).make_preferred().u8string();
    launch.args.emplace_back(ini.begin(), ini.end());
    launch.args.emplace_back("-g");
    launch.args.push_back(game_utf8);
    const auto user = std::filesystem::path{user_dir}.make_preferred().u8string();
    launch.env.emplace_back("ANKA_USER_DIR", std::string(user.begin(), user.end()));
    launch.env.insert(launch.env.end(), settings_env.begin(), settings_env.end());
    if (restart_handoff) {
        launch.env.emplace_back("EDEN_CLI_RESTART_HANDOFF", "1");
    }
    return launch;
}

/// The second core's whole environment: the starting program's (`inherited`) without what
/// ClearedForSecondCore drops and without the names the launch sets (Windows: any case), then the
/// launch's. A main core game's switches left in the program's environment (Nx2PerfSession set
/// them) never reach it.
inline std::vector<std::pair<std::string, std::string>> SecondCoreEnvironment(
    const std::vector<std::pair<std::string, std::string>>& inherited, const Launch& launch) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    const auto same_name = [lower](std::string_view a, std::string_view b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [lower](char x, char y) { return lower(x) == lower(y); });
    };
    std::vector<std::pair<std::string, std::string>> env;
    for (const auto& var : inherited) {
        const bool set_by_launch =
            std::any_of(launch.env.begin(), launch.env.end(),
                        [&](const auto& own) { return same_name(own.first, var.first); });
        if (!set_by_launch && !ClearedForSecondCore(var.first)) {
            env.push_back(var);
        }
    }
    env.insert(env.end(), launch.env.begin(), launch.env.end());
    return env;
}

/// One argument as CommandLineToArgvW reads it back: quoted when it has a space, a tab or a quote
/// (or is empty); backslashes doubled only before a quote.
inline std::string QuoteWindowsArg(std::string_view arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string{arg};
    }
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

/// The program and its arguments as one Windows command line (CreateProcessW).
inline std::string WindowsCommandLine(const std::filesystem::path& program,
                                      const std::vector<std::string>& args) {
    const auto program_utf8 = program.u8string();
    std::string line = QuoteWindowsArg(std::string(program_utf8.begin(), program_utf8.end()));
    for (const auto& arg : args) {
        line += ' ';
        line += QuoteWindowsArg(arg);
    }
    return line;
}

/// What the starting program does when the second core exits.
enum class ExitReaction {
    None,           ///< 0: closed normally
    StartFailed,    ///< 1: the game did not start; the details are in its log
    Restart,        ///< 75: the game asked for a restart: the same arguments + the handoff
    AlreadyRunning, ///< 76: the second core is already open; it touched nothing
    Crashed,        ///< anything else (0xC0000005...)
};

constexpr ExitReaction ForExitCode(std::uint32_t code) {
    switch (code) {
    case 0:
        return ExitReaction::None;
    case 1:
        return ExitReaction::StartFailed;
    case 75:
        return ExitReaction::Restart;
    case 76:
        return ExitReaction::AlreadyRunning;
    default:
        return ExitReaction::Crashed;
    }
}

/// The menu's state the second core's settings take over (besides the config file itself).
struct IniState {
    bool docked{};
    /// [Data%20Storage] keys and the paths in use (nand_directory, sdmc_directory, ...).
    std::vector<std::pair<std::string, std::string>> data_storage;
};

/// Settings only the second core has; the menu has no value for them and leaves its own.
constexpr std::string_view SecondCoreOnlyKeys[] = {"nvidia_dlss", "game_dlss_network", "mkw_30fps",
                                              "mkw_dynamic_fps", "mkw_smooth", "dk_30fps"};

/// The add-on the second core needs on (its dynamic frame rate patch).
constexpr std::string_view SecondCoreKeptAddOn = "Dynamic FPS by Dark";

namespace Detail {

struct Section {
    std::string header; ///< "[Name]", empty for the lines before the first section
    std::vector<std::string> lines;
};

inline std::string_view KeyOf(std::string_view line) {
    const auto eq = line.find('=');
    return eq == std::string_view::npos ? std::string_view{} : line.substr(0, eq);
}

inline std::string_view ValueOf(std::string_view line) {
    const auto eq = line.find('=');
    return eq == std::string_view::npos ? std::string_view{} : line.substr(eq + 1);
}

/// A QSettings value as written: "..." quoted strings unquoted (\" and \\ unescaped).
inline std::string Unquote(std::string_view value) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return std::string{value};
    }
    std::string out;
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '\\' && i + 2 < value.size()) {
            ++i;
        }
        out.push_back(value[i]);
    }
    return out;
}

/// A value as QSettings reads it back: quoted when it has a character QSettings would quote.
inline std::string Quote(std::string_view value) {
    const bool plain =
        !value.empty() && value.front() != ' ' && value.back() != ' ' &&
        value.find_first_of(",;=\"\\#") == std::string_view::npos;
    if (value.empty() || plain) {
        return std::string{value};
    }
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

inline std::vector<Section> Parse(std::string_view text) {
    std::vector<Section> sections(1);
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        pos = end + 1;
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            sections.push_back({std::string{line}, {}});
        } else {
            sections.back().lines.emplace_back(line);
        }
    }
    return sections;
}

inline Section& SectionNamed(std::vector<Section>& sections, std::string_view name) {
    const std::string header = "[" + std::string{name} + "]";
    for (auto& section : sections) {
        if (section.header == header) {
            return section;
        }
    }
    return sections.emplace_back(Section{header, {}});
}

inline void DropKey(Section& section, std::string_view key) {
    const std::string with_default = std::string{key} + "\\default";
    std::erase_if(section.lines, [&](const std::string& line) {
        const auto k = KeyOf(line);
        return k == key || k == with_default;
    });
}

/// Trailing blank lines stay after the section's keys.
inline void Append(Section& section, std::string line) {
    auto it = section.lines.end();
    while (it != section.lines.begin() && std::prev(it)->empty()) {
        --it;
    }
    section.lines.insert(it, std::move(line));
}

/// `key\default=false` and `key=value`: without the first line the second core takes its own
/// default and ignores the value.
inline void SetKey(Section& section, std::string_view key, std::string_view value) {
    DropKey(section, key);
    Append(section, std::string{key} + "\\default=false");
    Append(section, std::string{key} + "=" + Quote(value));
}

/// [DisabledAddOns] without SecondCoreKeptAddOn for its game, renumbered.
inline void KeepSecondCoreAddOn(Section& section) {
    struct Entry {
        std::string title_id;
        std::vector<std::string> disabled; ///< raw values
    };
    std::vector<Entry> entries;
    const auto entry = [&entries](std::size_t index) -> Entry& {
        if (entries.size() < index) {
            entries.resize(index);
        }
        return entries[index - 1];
    };
    bool changed = false;
    for (const auto& line : section.lines) {
        const auto key = KeyOf(line);
        const auto slash = key.find('\\');
        if (key.empty() || slash == std::string_view::npos) {
            continue;
        }
        std::size_t index = 0;
        for (const char c : key.substr(0, slash)) {
            if (c < '0' || c > '9') {
                index = 0;
                break;
            }
            index = index * 10 + std::size_t(c - '0');
        }
        if (index == 0 || index > 4096) {
            continue;
        }
        const auto rest = key.substr(slash + 1);
        if (rest == "title_id") {
            entry(index).title_id = Unquote(ValueOf(line));
        } else if (rest.starts_with("disabled\\") && rest.ends_with("\\d") &&
                   rest != "disabled\\size") {
            const auto number = rest.substr(9, rest.size() - 9 - 2);
            std::size_t j = 0;
            for (const char c : number) {
                j = (c >= '0' && c <= '9') ? j * 10 + std::size_t(c - '0') : 0;
            }
            if (j == 0 || j > 4096) {
                continue;
            }
            auto& disabled = entry(index).disabled;
            if (disabled.size() < j) {
                disabled.resize(j);
            }
            disabled[j - 1] = std::string{ValueOf(line)};
        }
    }
    for (auto& e : entries) {
        if (e.title_id != std::to_string(SecondCoreTitleId)) {
            continue;
        }
        const auto before = e.disabled.size();
        std::erase_if(e.disabled, [](const std::string& raw) {
            return Unquote(raw) == SecondCoreKeptAddOn;
        });
        changed |= e.disabled.size() != before;
    }
    if (!changed) {
        return;
    }
    // Written again the way Config::SaveDisabledAddOnValues writes it
    std::vector<std::string> blank_tail;
    while (!section.lines.empty() && section.lines.back().empty()) {
        blank_tail.push_back(section.lines.back());
        section.lines.pop_back();
    }
    section.lines.clear();
    section.lines.push_back("size=" + std::to_string(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string n = std::to_string(i + 1);
        section.lines.push_back(n + "\\title_id\\default=false");
        section.lines.push_back(n + "\\title_id=" + entries[i].title_id);
        for (std::size_t j = 0; j < entries[i].disabled.size(); ++j) {
            const std::string d = n + "\\disabled\\" + std::to_string(j + 1) + "\\d";
            section.lines.push_back(d + "\\default=false");
            section.lines.push_back(d + "=" + entries[i].disabled[j]);
        }
        section.lines.push_back(n + "\\disabled\\size=" +
                                std::to_string(entries[i].disabled.size()));
    }
    section.lines.insert(section.lines.end(), blank_tail.begin(), blank_tail.end());
}

} // namespace Detail

/// The second core's settings from the menu's (qt-config.ini, or anka-cli's sdl2-config.ini: the
/// same sections and keys): the file as it is, then
/// - [System] use_docked_mode always the starting program's (the second core's default differs);
/// - [Data%20Storage] the paths in use, the second core writing them back as absolute paths;
/// - [DisabledAddOns] without the frame rate patch its game needs on;
/// - [Renderer] use_reactive_flushing always off, whatever the preset wrote: the second core was
///   only ever tested so;
/// - no setting only the second core has.
/// anti_aliasing / scaling_filter / astc_recompression go over as they are: left at the
/// default (`\default=true`) the second core keeps its own (FXAA, ScaleForce, BC3).
/// TODO(contract): the full key list and any renamed keys come from anka-birlesim's contract.
inline std::string MakeSecondCoreIni(std::string_view config_text, const IniState& state) {
    using namespace Detail;
    auto sections = Parse(config_text);
    const std::string newline = config_text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";

    for (auto& section : sections) {
        for (const auto key : SecondCoreOnlyKeys) {
            DropKey(section, key);
        }
    }
    SetKey(SectionNamed(sections, "System"), "use_docked_mode", state.docked ? "1" : "0");
    SetKey(SectionNamed(sections, "Renderer"), "use_reactive_flushing", "false");
    if (!state.data_storage.empty()) {
        auto& storage = SectionNamed(sections, "Data%20Storage");
        for (const auto& [key, value] : state.data_storage) {
            SetKey(storage, key, value);
        }
    }
    for (auto& section : sections) {
        if (section.header == "[DisabledAddOns]") {
            KeepSecondCoreAddOn(section);
        }
    }

    std::string out;
    for (const auto& section : sections) {
        if (!section.header.empty()) {
            out += section.header + newline;
        }
        for (const auto& line : section.lines) {
            out += line + newline;
        }
    }
    // Parse() reads the final newline as an empty last line
    if (!config_text.empty() && config_text.back() == '\n' &&
        out.size() >= 2 * newline.size() &&
        out.compare(out.size() - 2 * newline.size(), 2 * newline.size(), newline + newline) == 0) {
        out.resize(out.size() - newline.size());
    }
    return out;
}

} // namespace AnkaCoreRoute
