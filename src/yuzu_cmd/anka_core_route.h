// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Anka 1.1 ships two cores: this program (Donkey Kong Bananza and every other game) and the Zelda
// core next to it, <package>/zelda/anka-zelda-cli.exe. The menu (yuzu/main_window.cpp) and
// anka-cli (yuzu_cmd/yuzu.cpp) start the Zelda core for Ocarina of Time; the decisions without Qt
// or the emulator (src/tests/yuzu/anka_core_route.cpp). The contract is the Zelda core's
// (anka-birlesim 455f2a90cd): ANKA_USER_DIR, -f / -c / -g, exit codes 0 / 1 / 75 / 76.
namespace AnkaCoreRoute {

/// The Legend of Zelda: Ocarina of Time (its updates share the base id's upper bits).
constexpr std::uint64_t ZeldaProgramId = 0x0400486024DEC000ULL;
/// ZeldaProgramId as [DisabledAddOns] stores it.
constexpr std::uint64_t ZeldaTitleId = ZeldaProgramId;

constexpr bool IsZelda(std::uint64_t program_id) {
    return (program_id & ~std::uint64_t{0xFFF}) == ZeldaProgramId;
}

/// The Zelda core in the package: zelda/anka-zelda.exe (with a window of its own) when the menu
/// starts it and it is there, else zelda/anka-zelda-cli.exe. Empty: neither is there.
inline std::optional<std::filesystem::path> FindZeldaCore(const std::filesystem::path& package_dir,
                                                          bool prefer_qt) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = package_dir / "zelda";
    if (prefer_qt && fs::is_regular_file(dir / "anka-zelda.exe", ec)) {
        return dir / "anka-zelda.exe";
    }
    if (fs::is_regular_file(dir / "anka-zelda-cli.exe", ec)) {
        return dir / "anka-zelda-cli.exe";
    }
    return std::nullopt;
}

/// The Zelda core's settings file, written by the starting program at every start.
inline std::filesystem::path ZeldaIniPath(const std::filesystem::path& user_dir) {
    return user_dir / "config" / "sdl2-config-zelda.ini";
}

/// The Zelda core's log (for the messages after a failed start).
inline std::filesystem::path ZeldaLogPath(const std::filesystem::path& user_dir) {
    return user_dir / "log" / "zelda" / "eden_log.txt";
}

struct Launch {
    std::filesystem::path program;
    std::filesystem::path working_dir;
    std::vector<std::string> args; ///< UTF-8, without the program
    std::vector<std::pair<std::string, std::string>> env;
};

/// The Zelda core's command line: `-f` when full screen, `-c <user>/config/sdl2-config-zelda.ini`,
/// `-g <game>`; ANKA_USER_DIR always, EDEN_CLI_RESTART_HANDOFF=1 only to restart after exit 75.
inline Launch PlanZelda(const std::filesystem::path& core, const std::filesystem::path& user_dir,
                        const std::string& game_utf8, bool fullscreen, bool restart_handoff) {
    Launch launch;
    launch.program = std::filesystem::path{core}.make_preferred();
    launch.working_dir = launch.program.parent_path();
    if (fullscreen) {
        launch.args.emplace_back("-f");
    }
    launch.args.emplace_back("-c");
    const auto ini = ZeldaIniPath(user_dir).make_preferred().u8string();
    launch.args.emplace_back(ini.begin(), ini.end());
    launch.args.emplace_back("-g");
    launch.args.push_back(game_utf8);
    const auto user = std::filesystem::path{user_dir}.make_preferred().u8string();
    launch.env.emplace_back("ANKA_USER_DIR", std::string(user.begin(), user.end()));
    if (restart_handoff) {
        launch.env.emplace_back("EDEN_CLI_RESTART_HANDOFF", "1");
    }
    return launch;
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

/// What the starting program does when the Zelda core exits.
enum class ExitReaction {
    None,           ///< 0: closed normally
    StartFailed,    ///< 1: the game did not start; the details are in its log
    Restart,        ///< 75: the game asked for a restart: the same arguments + the handoff
    AlreadyRunning, ///< 76: another Zelda core is open; it touched nothing
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

/// The menu's state the Zelda core's settings take over (besides the config file itself).
struct IniState {
    bool docked{};
    /// [Data%20Storage] keys and the paths in use (nand_directory, sdmc_directory, ...).
    std::vector<std::pair<std::string, std::string>> data_storage;
};

/// Settings only the Zelda core has; the menu has no value for them and leaves its own.
constexpr std::string_view ZeldaOnlyKeys[] = {"nvidia_dlss", "game_dlss_network", "mkw_30fps",
                                              "mkw_dynamic_fps", "mkw_smooth", "dk_30fps"};

/// The add-on the Zelda core needs on (its dynamic frame rate patch).
constexpr std::string_view ZeldaKeptAddOn = "Dynamic FPS by Dark";

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

/// `key\default=false` and `key=value`: without the first line the Zelda core takes its own
/// default and ignores the value.
inline void SetKey(Section& section, std::string_view key, std::string_view value) {
    DropKey(section, key);
    Append(section, std::string{key} + "\\default=false");
    Append(section, std::string{key} + "=" + Quote(value));
}

/// [DisabledAddOns] without ZeldaKeptAddOn for the Zelda title, renumbered.
inline void KeepZeldaAddOn(Section& section) {
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
        if (e.title_id != std::to_string(ZeldaTitleId)) {
            continue;
        }
        const auto before = e.disabled.size();
        std::erase_if(e.disabled, [](const std::string& raw) {
            return Unquote(raw) == ZeldaKeptAddOn;
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

/// The Zelda core's settings from the menu's (qt-config.ini, or anka-cli's sdl2-config.ini: the
/// same sections and keys): the file as it is, then
/// - [System] use_docked_mode always the starting program's (the Zelda core's default differs);
/// - [Data%20Storage] the paths in use, the Zelda core writing them back as absolute paths;
/// - [DisabledAddOns] without the Zelda core's own frame rate patch for Ocarina of Time;
/// - no setting only the Zelda core has.
/// anti_aliasing / scaling_filter / astc_recompression go over as they are: left at the
/// default (`\default=true`) the Zelda core keeps its own (FXAA, ScaleForce, BC3).
/// TODO(contract): the full key list and any renamed keys come from anka-birlesim's contract.
inline std::string MakeZeldaIni(std::string_view config_text, const IniState& state) {
    using namespace Detail;
    auto sections = Parse(config_text);
    const std::string newline = config_text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";

    for (auto& section : sections) {
        for (const auto key : ZeldaOnlyKeys) {
            DropKey(section, key);
        }
    }
    SetKey(SectionNamed(sections, "System"), "use_docked_mode", state.docked ? "1" : "0");
    if (!state.data_storage.empty()) {
        auto& storage = SectionNamed(sections, "Data%20Storage");
        for (const auto& [key, value] : state.data_storage) {
            SetKey(storage, key, value);
        }
    }
    for (auto& section : sections) {
        if (section.header == "[DisabledAddOns]") {
            KeepZeldaAddOn(section);
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
