// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/nx2_perf_settings.h"

// anka-cli --profile recommended|safe|reactive (Anka 1.1.2): what the launchers
// dist/anka/start-recommended.bat, start-safe.bat and start-reactive-experimental.bat did before
// the program starts, done by the program itself, so the launchers need no PowerShell (1.1.1 ran
// anka-config.ps1 through powershell.exe -ExecutionPolicy Bypass, which virus scanners flag):
//  - the EDEN_NX2_* variables of the launcher, set (or cleared) in this process's environment
//    before anything reads them (most are read once, the first time a game reaches them;
//    core/nx2_perf_settings.h);
//  - without -c, the user folder's config\sdl2-config.ini (user\ next to the program, else
//    %APPDATA%\eden, as the program finds it): made once as a copy of config\qt-config.ini (the
//    menu program's controller and settings), never changed when it exists. Recommended turns
//    reactive flushing off in a new copy and only notes it when an existing file has it on;
//    reactive uses config\sdl2-config-reactive.ini, made once from sdl2-config.ini with reactive
//    flushing and barrier feedback loops on.
// The .ini edits are anka-config.ps1's, byte for byte: only [Renderer] lines, the same encoding
// (a BOM only if the file had one) and line endings. Without --profile nothing here runs.
// The decisions without the file system or the environment are in this header.
namespace AnkaProfile {

enum class Profile { Recommended, Safe, Reactive };

inline constexpr std::array<std::string_view, 3> ProfileNames{"recommended", "safe", "reactive"};

constexpr std::string_view Name(Profile profile) {
    return ProfileNames[static_cast<std::size_t>(profile)];
}

constexpr char LowerAscii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (LowerAscii(a[i]) != LowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

constexpr std::optional<Profile> ProfileFromName(std::string_view name) {
    for (std::size_t i = 0; i < ProfileNames.size(); ++i) {
        if (EqualsIgnoreCase(name, ProfileNames[i])) {
            return static_cast<Profile>(i);
        }
    }
    return std::nullopt;
}

struct Options {
    std::optional<Profile> profile;
    /// --dk-dyn VALUE / --osd VALUE: EDEN_NX2_DK_DYN / EDEN_NX2_OSD instead of 1 (the launchers'
    /// DK_DYN and OSD lines); empty clears the variable, as "set EDEN_NX2_DK_DYN=" did.
    std::optional<std::string> dk_dyn;
    std::optional<std::string> osd;
    /// --rx-rt-fit VALUE: EDEN_NX2_RX_RT_FIT instead of 3 (reactive; the launcher's RX_RT_FIT line).
    std::optional<std::string> rx_rt_fit;
    /// -c / --config was given: the .ini is the user's, the profile leaves it alone (the launchers
    /// skipped their .ini step when their CONFIG line was set).
    bool config_given = false;
};

struct TakenArgs {
    Options options;
    std::vector<int> taken; ///< argv indexes this took out, ascending
    std::string error;      ///< not empty: a bad option, the text for the user
};

/// Core::ParseLaunchParams' getopt_long options, as far as this needs them: the long name, the
/// short one, whether getopt gives the short form a value ("c:" in its option string: -c x.ini,
/// -cx.ini), what the long form takes (--config x.ini, --config=x.ini; --program takes a value
/// only after "=") and whether ParseLaunchParams then takes the next argument itself
/// (argv[optind]): -p a b takes a and b, -pa b and --program b take b, -x f and --filter f take f.
struct LaunchOption {
    enum class Long { None, Required, Optional };
    std::string_view name;
    char short_name;
    bool short_value;
    Long long_value;
    bool takes_next;
};
inline constexpr std::array<LaunchOption, 13> LaunchOptions{{
    {"debug", 'd', false, LaunchOption::Long::None, false},
    {"config", 'c', true, LaunchOption::Long::Required, false},
    {"fullscreen", 'f', false, LaunchOption::Long::None, false},
    {"help", 'h', false, LaunchOption::Long::None, false},
    {"game", 'g', true, LaunchOption::Long::Required, false},
    {"multiplayer", 'm', true, LaunchOption::Long::Required, false},
    {"program", 'p', true, LaunchOption::Long::Optional, true},
    {"user", 'u', true, LaunchOption::Long::Required, false},
    {"version", 'v', false, LaunchOption::Long::None, false},
    {"input-profile", 'i', false, LaunchOption::Long::None, false},
    {"null-render", 'n', false, LaunchOption::Long::None, false},
    {"singlecore", 's', false, LaunchOption::Long::None, false},
    {"filter", 'x', false, LaunchOption::Long::None, true},
}};

/// The option "--name" means for getopt_long: the exact name, else the one it is an unambiguous
/// prefix of (--conf is --config); none when unknown or ambiguous.
constexpr const LaunchOption* FindLongOption(std::string_view name) {
    const LaunchOption* found = nullptr;
    int prefixes = 0;
    for (const LaunchOption& option : LaunchOptions) {
        if (option.name == name) {
            return &option;
        }
        if (!name.empty() && option.name.starts_with(name)) {
            found = &option;
            ++prefixes;
        }
    }
    return prefixes == 1 ? found : nullptr;
}

constexpr const LaunchOption* FindShortOption(char name) {
    for (const LaunchOption& option : LaunchOptions) {
        if (option.short_name == name) {
            return &option;
        }
    }
    return nullptr;
}

/// Takes --profile NAME, --dk-dyn VALUE, --osd VALUE and --rx-rt-fit VALUE (also --name=VALUE)
/// out of argv, so that Core::ParseLaunchParams never sees them (exact names only). The other
/// options are read as getopt_long reads them, to skip their values (a game file named like an
/// option stays a game file) and to notice -c: -c x.ini, -cx.ini, -fc x.ini, --config x.ini,
/// --config=x.ini, --conf x.ini. Nothing after "--" is an option.
inline TakenArgs TakeArgs(int argc, const char* const* argv) {
    TakenArgs out;
    struct Own {
        std::string_view name;
        int id;
    };
    constexpr std::array<Own, 4> own{
        {{"--profile", 0}, {"--dk-dyn", 1}, {"--osd", 2}, {"--rx-rt-fit", 3}}};
    std::optional<std::string> profile_name;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--") {
            break;
        }
        bool mine = false;
        for (const Own& option : own) {
            std::optional<std::string> value;
            if (arg == option.name) {
                if (i + 1 >= argc) {
                    out.error = "anka-cli: " + std::string{option.name} + " needs a value.";
                    return out;
                }
                out.taken.push_back(i);
                out.taken.push_back(i + 1);
                value = argv[++i];
            } else if (arg.size() > option.name.size() && arg.starts_with(option.name) &&
                       arg[option.name.size()] == '=') {
                out.taken.push_back(i);
                value = std::string{arg.substr(option.name.size() + 1)};
            } else {
                continue;
            }
            mine = true;
            switch (option.id) {
            case 0:
                profile_name = *value;
                break;
            case 1:
                out.options.dk_dyn = *value;
                break;
            case 2:
                out.options.osd = *value;
                break;
            default:
                out.options.rx_rt_fit = *value;
                break;
            }
            break;
        }
        if (mine) {
            continue;
        }
        if (arg.size() > 2 && arg.starts_with("--")) {
            const std::size_t equals = arg.find('=');
            const LaunchOption* option = FindLongOption(arg.substr(2, equals == arg.npos ? arg.npos
                                                                                       : equals - 2));
            if (option != nullptr) {
                out.options.config_given = out.options.config_given || option->short_name == 'c';
                if (equals == arg.npos) {
                    if (option->long_value == LaunchOption::Long::Required) {
                        ++i;
                    }
                    if (option->takes_next) {
                        ++i;
                    }
                } else if (option->long_value != LaunchOption::Long::None && option->takes_next) {
                    // --program=a b: ParseLaunchParams takes b too (a "=" value on an option
                    // without one is a getopt error: nothing more is taken)
                    ++i;
                }
            }
            continue;
        }
        if (arg.size() > 1 && arg[0] == '-') {
            // a cluster of short options: -fc x.ini; one that takes a value ends it (-cx.ini)
            for (std::size_t k = 1; k < arg.size(); ++k) {
                const LaunchOption* option = FindShortOption(arg[k]);
                if (option == nullptr) {
                    continue;
                }
                out.options.config_given = out.options.config_given || option->short_name == 'c';
                if (option->short_value) {
                    if (k + 1 == arg.size()) {
                        ++i;
                    }
                    if (option->takes_next) {
                        ++i;
                    }
                    break;
                }
                if (option->takes_next) {
                    if (k + 1 == arg.size()) {
                        ++i;
                    }
                    break;
                }
            }
        }
    }
    if (profile_name) {
        out.options.profile = ProfileFromName(*profile_name);
        if (!out.options.profile) {
            out.error = "anka-cli: unknown --profile \"" + *profile_name +
                        "\": use recommended, safe or reactive.";
            return out;
        }
    }
    if ((out.options.dk_dyn || out.options.osd) &&
        (!out.options.profile || *out.options.profile == Profile::Safe)) {
        out.error = "anka-cli: --dk-dyn and --osd go with --profile recommended or reactive.";
    } else if (out.options.rx_rt_fit &&
               (!out.options.profile || *out.options.profile != Profile::Reactive)) {
        out.error = "anka-cli: --rx-rt-fit goes with --profile reactive.";
    }
    return out;
}

/// One variable of a profile; an empty value clears it.
struct Var {
    std::string_view name;
    std::string value;

    bool operator==(const Var&) const = default;
};

/// The launchers' opt-in switches that none of them uses, cleared by recommended and reactive
/// (safe clears them too). EDEN_NX2_RX_RT_FIT is reactive's own and not here.
inline constexpr std::array<std::string_view, 31> UnusedOptIns{
    "EDEN_NX2_PRESENT_INTERVAL", "EDEN_NX2_MODE_GOV",         "EDEN_NX2_DK_DYN_MIN_FPS",
    "EDEN_NX2_BACK_CPU",         "EDEN_NX2_SEMEXEC_DEFER",    "EDEN_NX2_SKIP_EMPTY_DRAWS",
    "EDEN_NX2_ASYNC_SKIP",       "EDEN_NX2_VBLANK_GRID",      "EDEN_NX2_VRR_VSYNC",
    "EDEN_NX2_GPU_BUSY_CLOCK",   "EDEN_NX2_GPU_TICK_DIV",     "EDEN_NX2_PACE_GUEST_SLEEP",
    "EDEN_NX2_PRESENT_RATE",     "EDEN_NX2_PRESENT_DIAG",     "EDEN_NX2_MODE_DIAG",
    "EDEN_NX2_MODE_DIAG_MAX",    "EDEN_NX2_PACE_ONE",         "EDEN_NX2_MEMBAR",
    "EDEN_NX2_BACK_CORE",        "EDEN_NX2_FRONT_CORE",       "EDEN_NX2_FG_DIAG",
    "EDEN_NX2_FLUSH_FIRST_READ", "EDEN_NX2_READ_LATCH_AGE",   "EDEN_NX2_RX_ONE_SUBMIT",
    "EDEN_NX2_RX_COALESCE",      "EDEN_NX2_RX_PITCH_FORCED",  "EDEN_NX2_RX_MASKED_TARGETS",
    "EDEN_NX2_REACTIVE_STATS",   "EDEN_NX2_RX_NO_COPYBACK",   "EDEN_NX2_RX_NO_RT_LATCH",
    "EDEN_NX2_RX_COALESCE_LOOSE",
};

/// The switches recommended and reactive turn on (safe clears them).
inline constexpr std::array<std::string_view, 11> RecommendedNames{
    "EDEN_NX2_BACK_STAGES",           "EDEN_NX2_BACK_CUTS",    "EDEN_NX2_ACQUIRE_PEEK",
    "EDEN_NX2_JIT_SAME",              "EDEN_NX2_FRONT_MACRO_LEAN",
    "EDEN_NX2_PRESENT_INTERVAL_WORD", "EDEN_NX2_MODE_PACE",    "EDEN_NX2_PACE_SPIN_US",
    "EDEN_NX2_DK_DYN",                "EDEN_NX2_FG_TIMED",     "EDEN_NX2_OSD",
};

/// The variables of the options' profile, in the launcher's order (none without a profile).
inline std::vector<Var> Variables(const Options& options) {
    std::vector<Var> vars;
    if (!options.profile) {
        return vars;
    }
    const Profile profile = *options.profile;
    if (profile == Profile::Safe) {
        for (const std::string_view name : RecommendedNames) {
            vars.push_back({name, {}});
        }
        vars.push_back({"EDEN_NX2_RX_RT_FIT", {}});
        for (const std::string_view name : UnusedOptIns) {
            vars.push_back({name, {}});
        }
        for (const std::string_view name : Core::Nx2PerfSettings::SafeOff) {
            vars.push_back({name, "0"});
        }
        return vars;
    }
    for (const std::string_view name : UnusedOptIns) {
        vars.push_back({name, {}});
    }
    if (profile == Profile::Recommended) {
        vars.push_back({"EDEN_NX2_RX_RT_FIT", {}});
    }
    vars.push_back({"EDEN_NX2_BACK_STAGES", "1"});
    vars.push_back({"EDEN_NX2_BACK_CUTS", "a"});
    vars.push_back({"EDEN_NX2_ACQUIRE_PEEK", "1"});
    vars.push_back({"EDEN_NX2_JIT_SAME", "1"});
    vars.push_back({"EDEN_NX2_FRONT_MACRO_LEAN", "1"});
    vars.push_back({"EDEN_NX2_PRESENT_INTERVAL_WORD", "12:16:4"});
    vars.push_back({"EDEN_NX2_MODE_PACE", "2"});
    vars.push_back({"EDEN_NX2_PACE_SPIN_US", "1500"});
    vars.push_back({"EDEN_NX2_DK_DYN", options.dk_dyn.value_or("1")});
    vars.push_back({"EDEN_NX2_FG_TIMED", "1"});
    vars.push_back({"EDEN_NX2_OSD", options.osd.value_or("1")});
    if (profile == Profile::Reactive) {
        vars.push_back({"EDEN_NX2_RX_RT_FIT", options.rx_rt_fit.value_or("3")});
    }
    return vars;
}

/// For the log: the set variables as NAME=VALUE, then how many were cleared.
inline std::string Describe(const std::vector<Var>& vars) {
    std::string set;
    std::size_t cleared = 0;
    for (const Var& var : vars) {
        if (var.value.empty()) {
            ++cleared;
            continue;
        }
        if (!set.empty()) {
            set += ' ';
        }
        set += std::string{var.name} + '=' + var.value;
    }
    return (set.empty() ? std::string{"none"} : set) + " (" + std::to_string(cleared) +
           " others cleared)";
}

// ---- the .ini, as anka-config.ps1 read and wrote it --------------------------------------------

/// An .ini file as lines: the text after an optional UTF-8 BOM, split at "\n" with a "\r" before
/// it dropped; written back with "\r\n" between lines when the file had one anywhere, else "\n".
/// The bytes stay as they are (anka-config.ps1 decoded UTF-8 and encoded it again: the same bytes
/// for any valid UTF-8 file).
struct Ini {
    bool bom = false;
    bool crlf = false;
    bool ends_with_newline = false;
    std::vector<std::string> lines;
};

inline Ini ParseIni(std::string_view bytes) {
    Ini ini;
    if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF") {
        ini.bom = true;
        bytes.remove_prefix(3);
    }
    ini.crlf = bytes.find("\r\n") != std::string_view::npos;
    ini.ends_with_newline = !bytes.empty() && bytes.back() == '\n';
    std::size_t start = 0;
    while (true) {
        const std::size_t end = bytes.find('\n', start);
        std::string_view line = bytes.substr(start, end == std::string_view::npos ? bytes.npos
                                                                                   : end - start);
        if (end != std::string_view::npos && !line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        ini.lines.emplace_back(line);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (ini.ends_with_newline) {
        ini.lines.pop_back();
    }
    return ini;
}

inline std::string WriteIni(const Ini& ini) {
    const std::string_view newline = ini.crlf ? "\r\n" : "\n";
    std::string out = ini.bom ? "\xEF\xBB\xBF" : "";
    for (std::size_t i = 0; i < ini.lines.size(); ++i) {
        if (i != 0) {
            out += newline;
        }
        out += ini.lines[i];
    }
    if (ini.ends_with_newline) {
        out += newline;
    }
    return out;
}

/// .NET's String.Trim(): the white space of Char.IsWhiteSpace, here in UTF-8.
inline std::string_view TrimLikeDotNet(std::string_view s) {
    const auto space_at_start = [](std::string_view t) -> std::size_t {
        if (t.empty()) {
            return 0;
        }
        const unsigned char c = static_cast<unsigned char>(t[0]);
        if (c == ' ' || (c >= 0x09 && c <= 0x0D)) {
            return 1;
        }
        if (t.size() >= 2 && c == 0xC2 &&
            (static_cast<unsigned char>(t[1]) == 0x85 || static_cast<unsigned char>(t[1]) == 0xA0)) {
            return 2;
        }
        if (t.size() >= 3) {
            const unsigned char c1 = static_cast<unsigned char>(t[1]);
            const unsigned char c2 = static_cast<unsigned char>(t[2]);
            const bool space = (c == 0xE1 && c1 == 0x9A && c2 == 0x80) ||           // U+1680
                               (c == 0xE2 && c1 == 0x80 && (c2 <= 0x8A ||           // U+2000-200A
                                                            c2 == 0xA8 || c2 == 0xA9 || // 2028-9
                                                            c2 == 0xAF)) ||          // U+202F
                               (c == 0xE2 && c1 == 0x81 && c2 == 0x9F) ||           // U+205F
                               (c == 0xE3 && c1 == 0x80 && c2 == 0x80);             // U+3000
            if (space && c2 >= 0x80) {
                return 3;
            }
        }
        return 0;
    };
    const auto space_at_end = [&](std::string_view t) -> std::size_t {
        for (std::size_t n = 1; n <= 3 && n <= t.size(); ++n) {
            if (space_at_start(t.substr(t.size() - n)) == n) {
                return n;
            }
        }
        return 0;
    };
    while (const std::size_t n = space_at_start(s)) {
        s.remove_prefix(n);
    }
    while (const std::size_t n = space_at_end(s)) {
        s.remove_suffix(n);
    }
    return s;
}

/// [Renderer]'s lines: [begin, end) of ini.lines; begin is empty without the section. The first
/// [Renderer] (any case, surrounding spaces) up to the next line starting with "[".
struct Section {
    std::optional<std::size_t> begin;
    std::size_t end = 0;
};

inline Section FindRenderer(const Ini& ini) {
    Section section;
    section.end = ini.lines.size();
    for (std::size_t i = 0; i < ini.lines.size(); ++i) {
        const std::string_view line = TrimLikeDotNet(ini.lines[i]);
        if (!section.begin) {
            if (EqualsIgnoreCase(line, "[Renderer]")) {
                section.begin = i + 1;
            }
        } else if (line.starts_with('[')) {
            section.end = i;
            break;
        }
    }
    return section;
}

/// The first "key=value" line of [Renderer], its value trimmed.
inline std::optional<std::string> RendererValue(const Ini& ini, std::string_view key) {
    const Section section = FindRenderer(ini);
    if (!section.begin) {
        return std::nullopt;
    }
    const std::string prefix = std::string{key} + '=';
    for (std::size_t i = *section.begin; i < section.end; ++i) {
        if (ini.lines[i].starts_with(prefix)) {
            return std::string{TrimLikeDotNet(std::string_view{ini.lines[i]}.substr(prefix.size()))};
        }
    }
    return std::nullopt;
}

/// Whether reactive flushing is on: anything but use_reactive_flushing\default=false and
/// use_reactive_flushing=false (the default is on).
inline bool ReactiveFlushingOn(const Ini& ini) {
    const auto is_false = [](const std::optional<std::string>& value) {
        return value && EqualsIgnoreCase(*value, "false");
    };
    return !(is_false(RendererValue(ini, "use_reactive_flushing\\default")) &&
             is_false(RendererValue(ini, "use_reactive_flushing")));
}

/// Sets "key=value" lines in [Renderer]: every line of the key is replaced, a missing key is
/// added after the section's last non-empty line; a missing section is added at the end (after
/// an empty line when the last line is not empty).
inline void SetRenderer(Ini& ini, std::span<const std::string_view> pairs) {
    Section section = FindRenderer(ini);
    if (!section.begin) {
        if (!ini.lines.empty() && !TrimLikeDotNet(ini.lines.back()).empty()) {
            ini.lines.emplace_back();
        }
        ini.lines.emplace_back("[Renderer]");
        section.begin = ini.lines.size();
        section.end = ini.lines.size();
    }
    for (const std::string_view pair : pairs) {
        const std::string prefix = std::string{pair.substr(0, pair.find('='))} + '=';
        bool found = false;
        for (std::size_t i = *section.begin; i < section.end; ++i) {
            if (ini.lines[i].starts_with(prefix)) {
                ini.lines[i] = pair;
                found = true;
            }
        }
        if (!found) {
            std::size_t at = section.end;
            while (at > *section.begin && TrimLikeDotNet(ini.lines[at - 1]).empty()) {
                --at;
            }
            ini.lines.insert(ini.lines.begin() + static_cast<std::ptrdiff_t>(at), std::string{pair});
            ++section.end;
        }
    }
}

/// The edits: a new sdl2-config.ini of recommended and reactive, and sdl2-config-reactive.ini.
inline constexpr std::array<std::string_view, 2> ReactiveOff{
    "use_reactive_flushing\\default=false",
    "use_reactive_flushing=false",
};
inline constexpr std::array<std::string_view, 4> ReactiveOn{
    "use_reactive_flushing\\default=false",
    "use_reactive_flushing=true",
    "barrier_feedback_loops\\default=false",
    "barrier_feedback_loops=true",
};

// ---- what touches the process, the console and the files (anka_profile.cpp) -------------------

/// Remembers the standard output before SDL_AppInit attaches to the console: the launcher's
/// output, when it was redirected to a file or pipe, gets Say's lines.
void CaptureStandardOutput();

/// One line for the user, as the launchers' "echo" (the console or the redirected output).
void Say(std::string_view line);
void SayAll(const std::vector<std::string>& lines);

/// For a start that stops before SDL_AppInit attaches to the console itself: attaches to the
/// launcher's console (unless the output was redirected), so that Say reaches it.
void AttachToConsole();

/// Sets the variables in this process's environment (an empty value removes one); children (the
/// second core) inherit them.
void ApplyEnvironment(const std::vector<Var>& vars);

/// Whether the variables were set from the command line before the program's initializers
/// (anka_profile.cpp's EarlyEnvironment; always with MSVC / clang-cl). SDL_AppInit sets them
/// itself otherwise.
bool AppliedBeforeInitializers();

/// The config folder of the user folder the program uses (user\config next to it, else
/// %APPDATA%\eden\config), without making it.
std::filesystem::path UserConfigDir();

struct ConfigResult {
    bool go = true;                            ///< false: the start stops (exit 1)
    std::optional<std::filesystem::path> config; ///< the .ini to start with (reactive), as -c
    std::vector<std::string> said;             ///< the lines for the user (Say) and the log
};

/// The launcher's .ini step for `profile` in `config_dir` (when -c was not given); says nothing
/// itself: its lines are in the result.
ConfigResult PrepareConfig(Profile profile, const std::filesystem::path& config_dir);

} // namespace AnkaProfile
