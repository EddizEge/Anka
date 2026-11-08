// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Anka 1.1 ships two cores: this program (Donkey Kong Bananza and every other game) and a second
// core next to it for one game (SecondCoreProgramId). The menu (yuzu/main_window.cpp) and
// anka-cli (yuzu_cmd/yuzu.cpp) start the second core for that game; the decisions without Qt or
// the emulator (src/tests/yuzu/anka_core_route.cpp). The contract is the second core's
// (anka-birlesim 455f2a90cd, its names from 7b85eb7ce4's common/anka_core.h): ANKA_USER_DIR,
// -f / -c / -g, exit codes 0 / 1 / 75 / 76.
//
// Every name the second core goes by (its folder, programs, settings file, cache and log folders,
// lock) and every text the user reads about it are here and only here: a rename is one commit.
namespace AnkaCoreRoute {

/// The second core's file names (the contract with it).
namespace Names {
constexpr std::string_view Folder = "core2";              ///< <package>/<Folder>
constexpr std::string_view CliProgram = "anka-core2-cli.exe";
constexpr std::string_view QtProgram = "anka-core2.exe";  ///< with a window of its own; none in 1.1
constexpr std::string_view Ini = "sdl2-config-core2.ini"; ///< <user>/config/<Ini>
constexpr std::string_view CacheFolder = "core2";         ///< <user>/cache/<CacheFolder>: its caches
constexpr std::string_view LogFolder = "core2";           ///< <user>/log/<LogFolder>/eden_log.txt
constexpr std::string_view LogFile = "eden_log.txt";
constexpr std::string_view Lock = "Local\\AnkaCore2";     ///< its single instance (exit 76)
} // namespace Names

/// What the user reads about the second core (English, as the rest of Anka). The user knows the
/// game, not "the second core": the texts name it, GameName put in as %1 (the user's decision,
/// 08.11.2026). The product's own names (Names::, the package, its programs and window titles)
/// never carry it.
namespace Text {
constexpr const char* GameName = "The Legend of Zelda: Ocarina of Time";
constexpr const char* Starting = "Starting %1";
constexpr const char* Closed = "%1 closed";
/// Another game asked to start while it runs
constexpr const char* StillRunning = "%1 is still running. Close it before starting another game.";
/// It asked to start while a game runs on the main core
constexpr const char* StopRunningFirst = "Stop the running game before starting %1.";
/// Over the Properties window of the game on the second core, which has only the Add-Ons tab: the
/// choices reach the second core at its next start (MakeSecondCoreIni), not while it runs
constexpr const char* PropertiesNote = "For this game only the add-on choices are used. Changes "
                                       "apply the next time the game starts.";
/// %2: the second core's program, so the user sees which file of the package is missing
constexpr const char* Missing =
    "%1 cannot start: %2 was not found. Extract the whole Anka package.";
constexpr const char* IniNotWritten = "%1 was not started: its settings could not be written to %2.";
constexpr const char* SettingsNotRead =
    "%1 was not started: the settings could not be read from %2.";
constexpr const char* NotStarted = "%1 could not be started: %2";
constexpr const char* StartFailed = "%1 could not start. The details are in its log:\n%2";
/// Exit 76: it was already open (started from somewhere else)
constexpr const char* AlreadyRunning = "%1 is already running.";
constexpr const char* Crashed = "%1 closed unexpectedly (exit code 0x%2). Its log:\n%3";
/// The Anka tab, under a setting the second core does not take (ReachesSecondCore)
constexpr const char* NoEffectOnSecondCore = "No effect in %1";
/// The Anka tab's preset: what it means on the second core (SecondCorePresetVars)
constexpr const char* PresetOnSecondCore =
    "In %1 Reactive Flushing stays off, so both Recommended presets are the same there; Safe also "
    "turns off its GPU shortcuts and its GPU thread split. No preset changes that game's own "
    "settings.";
/// The Anka tab's group of the second core's own settings (SecondCoreOptions), as the main core's
/// game group is titled
constexpr const char* OptionsTitle = "%1 only (off in other games)";
constexpr const char* MaxResolution = "Full-resolution 3D";
constexpr const char* MaxResolutionText =
    "On: the game draws its 3D at full resolution. Off: it lowers it under load, down to 480p: "
    "blurrier, a few percent more FPS.";
constexpr const char* VisualTweak = "Haze and bloom tweak";
constexpr const char* VisualTweakText = "Thinner haze and lower exposure. No effect on speed.";
constexpr const char* OcclusionOff = "Occlusion off (experimental)";
constexpr const char* OcclusionOffText =
    "Turns off the game's occlusion queries and depth pre-pass. Experimental; no measured speed "
    "change.";
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

/// A UTF-8 path (Config::GetConfigFilePath, Common::FS::PathToUTF8String) as a path. A path built
/// from a std::string reads it in the ANSI code page on Windows: a folder named with an "ö" or in
/// Japanese becomes another path, or an exception.
inline std::filesystem::path PathFromUtf8(std::string_view utf8) {
    return std::filesystem::path{std::u8string(utf8.begin(), utf8.end())};
}

/// The starting program's settings file, for MakeSecondCoreIni. Empty when it could not be read or
/// holds nothing: the second core's settings file then stays as it was and the second core does not
/// start (written from nothing, it would have no controls and default settings).
inline std::optional<std::string> ReadStartingSettings(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    if (!in.is_open()) {
        return std::nullopt;
    }
    std::stringstream text;
    text << in.rdbuf();
    if (in.bad()) {
        return std::nullopt;
    }
    std::string settings = std::move(text).str();
    if (settings.find_first_not_of(" \t\r\n") == std::string::npos) {
        return std::nullopt;
    }
    return settings;
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
/// Safe: the boxes, the part of Core::Nx2PerfSettings::SafeOff it reads, at 0, and its own GPU
/// thread split off (EDEN_NX2_SPLIT=0; its rare start-up stall waited in that split). Custom: the
/// boxes. Never EDEN_NX2_PBFLUSH_WINDOW_KIB=256, the main core's window: it halved the second
/// core's frame rate and made it stutter.
inline constexpr int SafePreset = 2; ///< Core::Nx2PerfSettings::Preset::Safe
inline constexpr std::array<SecondCorePresetVar, 9> SecondCorePresetVars{{
    {SafePreset, "EDEN_NX2_UBO_BIND_SKIP", "0"},
    {SafePreset, "EDEN_NX2_MARK_WORDS", "0"},
    {SafePreset, "EDEN_NX2_SLOT_SKIP", "0"},
    {SafePreset, "EDEN_NX2_TICK_SKIP", "0"},
    {SafePreset, "EDEN_NX2_HLE_INDIRECT", "0"},
    {SafePreset, "EDEN_NX2_LAYOUT_MEMO", "0"},
    {SafePreset, "EDEN_NX2_FAST_PRIORITY_SCAN", "0"},
    {SafePreset, "EDEN_NX2_PBFLUSH_WINDOW_KIB", "0"},
    {SafePreset, "EDEN_NX2_SPLIT", "0"},
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

/// The kind of control a SecondCoreOption has; its value is an int either way.
enum class OptionKind : std::uint8_t {
    Toggle, ///< a box: 0 off, 1 on
    Number, ///< a whole number from `min` to `max`
    Choice, ///< one of `choices`, by its value
};

/// Where a SecondCoreOption's value goes on the second core.
enum class OptionTarget : std::uint8_t {
    Env, ///< the variable `name`: a Toggle as "1" / "0", the others as the number
    Ini, ///< `name` in [`section`] of its settings file: a Toggle as "true" / "false"
    /// A Choice's item writes its own keys of the settings file (`writes`), no variable: one choice
    /// that is several settings there (frame generation: a multiplier and a target rate)
    IniWrites,
};

/// A key an item of an OptionTarget::IniWrites choice writes in the second core's settings file.
struct IniWrite {
    std::string_view section;
    std::string_view key;
    std::string_view value;
};

/// An item of a Choice: the value kept and handed over, and what the user reads.
struct OptionChoice {
    int value;
    const char* label;
    std::span<const IniWrite> writes{}; ///< OptionTarget::IniWrites
};

/// A setting of the Anka tab's section of the second core's game: one the second core has and the
/// main core does not. Kept in the [Anka] group under `key` (beside Core::Nx2PerfSettings::Keys, never
/// one of them: no preset and no main core plan sees it), handed to the second core at each start.
struct SecondCoreOption {
    std::string_view key;
    OptionKind kind;
    int default_value; ///< the second core's own default
    OptionTarget target;
    std::string_view name;      ///< the variable, or the key in its settings file
    std::string_view section{}; ///< OptionTarget::Ini
    const char* label{};        ///< Text::
    const char* text{};         ///< Text::, under the control
    int min{};                  ///< OptionKind::Number
    int max{1};                 ///< OptionKind::Number
    std::span<const OptionChoice> choices{}; ///< OptionKind::Choice
};

/// The second core's own settings. One row each: the tab (configure_nx2perf.cpp), the [Anka] keys
/// (LoadSecondCoreOptions / SaveSecondCoreOptions), anka-cli (SecondCoreOptionsFromMenu), the
/// variables and the settings file (SecondCoreOptionOutputs) all follow it.
/// The variables are written "1" / "0" exactly (the second core reads their first character, or
/// "0" alone). Every name is an EDEN_* one: the starting program's own never reach the second core
/// (ClearedForSecondCore), so only these rows set them there.
/// - Full-resolution 3D / Occlusion off: built-in patches of the second core (its
///   file_sys/patch_manager.cpp, BUILTIN_PATCHES, BuiltinPatchOffered): '1' offers the patch, '0'
///   hides it. Max Resolution: its 3D at full scale instead of the GPU-time controller's, down to
///   480p (FZ-MAXRES0: off was a few percent faster). Occlusion Off: no speed change measured.
/// - Haze and bloom tweak: its renderer_vulkan/vk_pipeline_cache.cpp OotVisualWorkaround, on
///   unless EDEN_NX2_OOT_VISUAL_WORKAROUND is "0": thinner haze, lower exposure, shaders only.
inline constexpr std::array SecondCoreOptions{
    SecondCoreOption{.key = "second_core_max_resolution",
                     .kind = OptionKind::Toggle,
                     .default_value = 1,
                     .target = OptionTarget::Env,
                     .name = "EDEN_OOT_MAX_RESOLUTION",
                     .label = Text::MaxResolution,
                     .text = Text::MaxResolutionText},
    SecondCoreOption{.key = "second_core_visual_tweak",
                     .kind = OptionKind::Toggle,
                     .default_value = 1,
                     .target = OptionTarget::Env,
                     .name = "EDEN_NX2_OOT_VISUAL_WORKAROUND",
                     .label = Text::VisualTweak,
                     .text = Text::VisualTweakText},
    SecondCoreOption{.key = "second_core_occlusion_off",
                     .kind = OptionKind::Toggle,
                     .default_value = 0,
                     .target = OptionTarget::Env,
                     .name = "EDEN_OOT_OCCLUSION_OFF",
                     .label = Text::OcclusionOff,
                     .text = Text::OcclusionOffText},
};

/// The values of SecondCoreOptions, in its order.
using SecondCoreValues = std::array<int, SecondCoreOptions.size()>;

/// A value the option can hold: a Toggle 0 or 1, a Number within its range, a Choice one of its
/// values (else the default).
constexpr int ValidOption(const SecondCoreOption& option, int value) {
    switch (option.kind) {
    case OptionKind::Toggle:
        return value != 0 ? 1 : 0;
    case OptionKind::Number:
        return std::clamp(value, option.min, option.max);
    case OptionKind::Choice:
        return std::any_of(option.choices.begin(), option.choices.end(),
                           [value](const OptionChoice& choice) { return choice.value == value; })
                   ? value
                   : option.default_value;
    }
    return option.default_value;
}

/// Every option at the second core's own default.
template <std::size_t N>
constexpr std::array<int, N> OptionDefaults(const std::array<SecondCoreOption, N>& table) {
    std::array<int, N> values{};
    for (std::size_t i = 0; i < N; ++i) {
        values[i] = table[i].default_value;
    }
    return values;
}

constexpr SecondCoreValues SecondCoreDefaults() {
    return OptionDefaults(SecondCoreOptions);
}

/// An option's value as the second core reads it.
inline std::string OptionValueText(const SecondCoreOption& option, int value) {
    value = ValidOption(option, value);
    if (option.kind == OptionKind::Toggle) {
        if (option.target == OptionTarget::Env) {
            return value != 0 ? "1" : "0";
        }
        return value != 0 ? "true" : "false";
    }
    return std::to_string(value);
}

/// One value handed to the second core.
struct OptionOutput {
    OptionTarget target; ///< Env or Ini (an IniWrites item gives one Ini output per key)
    std::string section; ///< OptionTarget::Ini
    std::string name;
    std::string value;

    bool operator==(const OptionOutput&) const = default;
};

/// What the options hand over, every one of them and always explicitly, the default too: the
/// value is the one the tab shows whatever the second core's default is (or becomes), and its log
/// names it. A value missing from `values` is the option's default.
inline std::vector<OptionOutput> SecondCoreOptionOutputs(
    std::span<const int> values, std::span<const SecondCoreOption> table = SecondCoreOptions) {
    std::vector<OptionOutput> out;
    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto& option = table[i];
        const int value = i < values.size() ? values[i] : option.default_value;
        if (option.target == OptionTarget::IniWrites) {
            const int chosen = ValidOption(option, value);
            for (const auto& choice : option.choices) {
                if (choice.value != chosen) {
                    continue;
                }
                for (const auto& write : choice.writes) {
                    out.push_back({OptionTarget::Ini, std::string{write.section},
                                   std::string{write.key}, std::string{write.value}});
                }
            }
            continue;
        }
        out.push_back({option.target,
                       option.target == OptionTarget::Ini ? std::string{option.section}
                                                          : std::string{},
                       std::string{option.name}, OptionValueText(option, value)});
    }
    return out;
}

/// The options' variables (PlanSecondCore's settings_env). The starting program's own EDEN_* never
/// reach the second core (ClearedForSecondCore): a variable of these is always this value.
inline std::vector<std::pair<std::string, std::string>> SecondCoreOptionEnv(
    std::span<const int> values, std::span<const SecondCoreOption> table = SecondCoreOptions) {
    std::vector<std::pair<std::string, std::string>> env;
    for (const auto& out : SecondCoreOptionOutputs(values, table)) {
        if (out.target == OptionTarget::Env) {
            env.emplace_back(out.name, out.value);
        }
    }
    return env;
}

/// Reads the options of the [Anka] group: a Toggle through `read_bool(key, default)`, the others
/// through `read_int(key, default)` (as QtConfig reads the tab's other settings).
template <std::size_t N, typename ReadBool, typename ReadInt>
std::array<int, N> LoadSecondCoreOptions(const std::array<SecondCoreOption, N>& table,
                                         ReadBool&& read_bool, ReadInt&& read_int) {
    std::array<int, N> values{};
    for (std::size_t i = 0; i < N; ++i) {
        const auto& option = table[i];
        const int value =
            option.kind == OptionKind::Toggle
                ? (read_bool(option.key, option.default_value != 0) ? 1 : 0)
                : static_cast<int>(read_int(option.key, option.default_value));
        values[i] = ValidOption(option, value);
    }
    return values;
}

/// Writes the options to the [Anka] group: a Toggle through `write_bool(key, value, default)`, the
/// others through `write_int(key, value, default)`.
template <typename WriteBool, typename WriteInt>
void SaveSecondCoreOptions(std::span<const SecondCoreOption> table, std::span<const int> values,
                           WriteBool&& write_bool, WriteInt&& write_int) {
    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto& option = table[i];
        const int value =
            ValidOption(option, i < values.size() ? values[i] : option.default_value);
        if (option.kind == OptionKind::Toggle) {
            write_bool(option.key, value != 0, option.default_value != 0);
        } else {
            write_int(option.key, value, option.default_value);
        }
    }
}

/// Variables of the starting program's environment the second core does not get: Anka's switches
/// and the fork's diagnostics (EDEN_*; a launcher of the main core sets dozens) and the NVIDIA
/// driver's shader cache settings (__GL_SHADER_DISK_CACHE*), as the second core's own test
/// launchers clear them. Its own (PlanSecondCore's) are set afterwards. Any case: Windows reads
/// "eden_nx2_x" as EDEN_NX2_X.
constexpr bool ClearedForSecondCore(std::string_view name) {
    const auto starts_with = [name](std::string_view prefix) {
        return name.size() >= prefix.size() &&
               std::equal(prefix.begin(), prefix.end(), name.begin(), [](char p, char c) {
                   return p == (c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c);
               });
    };
    return starts_with("EDEN_") || starts_with("__GL_SHADER_DISK_CACHE");
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
    /// The values of the Anka tab's second core options (UISettings, or SecondCoreOptionsFromMenu),
    /// in the table's order; missing ones at their defaults
    std::vector<int> options;
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

/// The most entries KeepSecondCoreAddOn takes from an array.
constexpr std::size_t MaxArray = 4096;

/// A QSettings array index or size: digits only (else 0), at most MaxArray + 1.
inline std::size_t ArrayNumber(std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return 0;
        }
        n = std::min(n * 10 + std::size_t(c - '0'), MaxArray + 1);
    }
    return n;
}

/// [DisabledAddOns] without SecondCoreKeptAddOn for its game, renumbered. Read as
/// Config::ReadDisabledAddOnValues reads it: entries 1..`size`, of each `N\disabled\1..size`; a
/// missing size is 0. Config::SaveDisabledAddOnValues writes only the sizes when a list gets
/// shorter: the entries past them are old ones and must not come back.
inline void KeepSecondCoreAddOn(Section& section) {
    struct Entry {
        std::string title_id;
        std::size_t disabled_size{};
        std::vector<std::string> disabled; ///< raw values
    };
    std::size_t size = 0;
    for (const auto& line : section.lines) {
        if (KeyOf(line) == "size") {
            size = std::min(ArrayNumber(Unquote(ValueOf(line))), MaxArray);
        }
    }
    std::vector<Entry> entries(size);
    bool changed = false;
    for (const auto& line : section.lines) {
        const auto key = KeyOf(line);
        const auto slash = key.find('\\');
        if (key.empty() || slash == std::string_view::npos) {
            continue;
        }
        const std::size_t index = ArrayNumber(key.substr(0, slash));
        if (index == 0 || index > size) {
            continue;
        }
        auto& entry = entries[index - 1];
        const auto rest = key.substr(slash + 1);
        if (rest == "title_id") {
            entry.title_id = Unquote(ValueOf(line));
        } else if (rest == "disabled\\size") {
            entry.disabled_size = std::min(ArrayNumber(Unquote(ValueOf(line))), MaxArray);
        } else if (rest.size() > 11 && rest.starts_with("disabled\\") && rest.ends_with("\\d")) {
            const std::size_t j = ArrayNumber(rest.substr(9, rest.size() - 9 - 2));
            if (j == 0 || j > MaxArray) {
                continue;
            }
            if (entry.disabled.size() < j) {
                entry.disabled.resize(j);
            }
            entry.disabled[j - 1] = std::string{ValueOf(line)};
        }
    }
    for (auto& e : entries) {
        e.disabled.resize(e.disabled_size);
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
        // An entry within the size without a title_id line: the reader's default
        section.lines.push_back(n + "\\title_id=" +
                                (entries[i].title_id.empty() ? "0" : entries[i].title_id));
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

inline char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}

inline bool SameText(std::string_view a, std::string_view b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](char x, char y) { return Lower(x) == Lower(y); });
}

inline std::string_view Trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

/// A value as SimpleIni's GetBoolValue reads it: t / y / 1 / on true, f / n / 0 / off false.
inline std::optional<bool> IniBool(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    switch (value[0]) {
    case 't': case 'T': case 'y': case 'Y': case '1':
        return true;
    case 'f': case 'F': case 'n': case 'N': case '0':
        return false;
    case 'o': case 'O':
        if (value.size() > 1 && Lower(value[1]) == 'n') {
            return true;
        }
        if (value.size() > 1 && Lower(value[1]) == 'f') {
            return false;
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

} // namespace Detail

/// The second core's options from the menu's settings file (qt-config.ini), for anka-cli, which
/// has no Anka tab: its [Anka] group read as QtConfig reads it (Config::ReadBooleanSetting /
/// ReadIntegerSetting: a Toggle at its default unless `key\default` is false, a number unless
/// `key\default` is there and false; the last line of a key counts, names in any case). No file or
/// no group: the defaults.
template <std::size_t N>
std::array<int, N> SecondCoreOptionsFromMenu(std::string_view menu_ini,
                                             const std::array<SecondCoreOption, N>& table) {
    using namespace Detail;
    std::vector<std::pair<std::string, std::string>> keys;
    for (const auto& section : Parse(menu_ini)) {
        if (!SameText(section.header, "[Anka]")) {
            continue;
        }
        for (const auto& line : section.lines) {
            const auto eq = line.find('=');
            if (eq != std::string::npos) {
                keys.emplace_back(Trim(std::string_view{line}.substr(0, eq)),
                                  Trim(std::string_view{line}.substr(eq + 1)));
            }
        }
    }
    const auto value_of = [&keys](std::string_view key) -> std::optional<std::string_view> {
        for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
            if (SameText(it->first, key)) {
                return std::string_view{it->second};
            }
        }
        return std::nullopt;
    };
    const auto read_bool = [&](std::string_view key, bool def) {
        const auto is_default = value_of(std::string{key} + "\\default");
        if (is_default && IniBool(*is_default).value_or(false)) {
            return def;
        }
        const auto value = value_of(key);
        return value ? IniBool(*value).value_or(def) : def;
    };
    const auto read_int = [&](std::string_view key, int def) {
        const auto is_default = value_of(std::string{key} + "\\default");
        if (!is_default || IniBool(*is_default).value_or(true)) {
            return def;
        }
        const auto value = value_of(key);
        if (!value) {
            return def;
        }
        int number = 0;
        const auto [end, error] =
            std::from_chars(value->data(), value->data() + value->size(), number);
        return error == std::errc{} && end == value->data() + value->size() ? number : def;
    };
    return LoadSecondCoreOptions(table, read_bool, read_int);
}

inline SecondCoreValues SecondCoreOptionsFromMenu(std::string_view menu_ini) {
    return SecondCoreOptionsFromMenu(menu_ini, SecondCoreOptions);
}

/// The second core's settings from the menu's (qt-config.ini, or anka-cli's sdl2-config.ini: the
/// same sections and keys): the file as it is, then
/// - [System] use_docked_mode always the starting program's (the second core's default differs);
/// - [Data%20Storage] the paths in use, the second core writing them back as absolute paths;
/// - [DisabledAddOns] without the frame rate patch its game needs on;
/// - [Renderer] use_reactive_flushing always off and barrier_feedback_loops always on (its default),
///   whatever the preset wrote: the second core was only ever tested so;
/// - no setting only the second core has;
/// - the keys SecondCoreOptions write there (state.options, OptionTarget::Ini / IniWrites) with
///   `\default=false`, in place of the menu's (its frame_gen_* included).
/// anti_aliasing / scaling_filter / astc_recompression go over as they are: left at the
/// default (`\default=true`) the second core keeps its own (FXAA, ScaleForce, BC3).
/// TODO(contract): the full key list and any renamed keys come from anka-birlesim's contract.
inline std::string MakeSecondCoreIni(std::string_view config_text, const IniState& state,
                                     std::span<const SecondCoreOption> options =
                                         SecondCoreOptions) {
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
    SetKey(SectionNamed(sections, "Renderer"), "barrier_feedback_loops", "true");
    for (const auto& option : SecondCoreOptionOutputs(state.options, options)) {
        if (option.target != OptionTarget::Ini) {
            continue;
        }
        for (auto& section : sections) {
            DropKey(section, option.name);
        }
        SetKey(SectionNamed(sections, option.section), option.name, option.value);
    }
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
