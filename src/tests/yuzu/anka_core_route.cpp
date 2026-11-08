// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "core/nx2_perf_settings.h"
#include "yuzu_cmd/anka_core_route.h"

namespace {

namespace fs = std::filesystem;
namespace Perf = Core::Nx2PerfSettings;
using namespace AnkaCoreRoute;
using Env = std::vector<std::pair<std::string, std::string>>;

constexpr int Recommended = static_cast<int>(Perf::Preset::Recommended);

// The second core's variables for the Anka tab's boxes and preset, as the menu asks for them
Env EnvFor(const Perf::Switches& switches) {
    return SecondCoreEnv(
        [&switches](std::string_view name) {
            for (const auto& key : Perf::Keys) {
                if (key.name == name) {
                    return switches.*key.field;
                }
            }
            return false;
        },
        static_cast<int>(switches.preset));
}

Env EnvFor(Perf::Preset preset) {
    return EnvFor(Perf::FromPreset(preset, Perf::Recommended()));
}

constexpr std::uint64_t Bananza = 0x040074A01BF12000ULL;

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("anka_core_route_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path / Names::Folder);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void Touch(const fs::path& file) {
    std::ofstream{file} << "x";
}

bool Has(const std::string& text, const std::string& line) {
    return ("\n" + text).find("\n" + line + "\n") != std::string::npos;
}

// Every box of the Anka tab on
bool AllOn(std::string_view) {
    return true;
}

// The parts of a qt-config.ini the second core's settings change
constexpr const char* QtConfig = "[DisabledAddOns]\n"
                                 "size=2\n"
                                 "1\\title_id\\default=false\n"
                                 "1\\title_id=288309953924349952\n"
                                 "1\\disabled\\1\\d\\default=false\n"
                                 "1\\disabled\\1\\d=Dynamic FPS by Dark\n"
                                 "1\\disabled\\2\\d\\default=false\n"
                                 "1\\disabled\\2\\d=Other Mod\n"
                                 "1\\disabled\\size=2\n"
                                 "2\\title_id\\default=false\n"
                                 "2\\title_id=288358607164088320\n"
                                 "2\\disabled\\1\\d\\default=false\n"
                                 "2\\disabled\\1\\d=Dynamic FPS by Dark\n"
                                 "2\\disabled\\size=1\n"
                                 "\n"
                                 "[Controls]\n"
                                 "player_0_button_a\\default=false\n"
                                 "player_0_button_a=\"engine:sdl,button:1\"\n"
                                 "\n"
                                 "[Renderer]\n"
                                 "anti_aliasing\\default=true\n"
                                 "anti_aliasing=0\n"
                                 "scaling_filter\\default=false\n"
                                 "scaling_filter=1\n"
                                 "astc_recompression\\default=true\n"
                                 "astc_recompression=0\n"
                                 "frame_gen\\default=false\n"
                                 "frame_gen=true\n"
                                 "frame_gen_type\\default=true\n"
                                 "frame_gen_type=1\n"
                                 "frame_gen_multiplier\\default=false\n"
                                 "frame_gen_multiplier=2\n"
                                 "frame_gen_target_rate\\default=false\n"
                                 "frame_gen_target_rate=120\n"
                                 "dk_30fps\\default=false\n"
                                 "dk_30fps=true\n"
                                 "\n"
                                 "[System]\n"
                                 "use_docked_mode\\default=true\n"
                                 "use_docked_mode=1\n"
                                 "\n"
                                 "[Data%20Storage]\n"
                                 "nand_directory\\default=true\n"
                                 "nand_directory=C:/old/nand\n"
                                 "save_directory\\default=true\n"
                                 "save_directory=\n";

IniState State(bool docked) {
    return {.docked = docked,
            .data_storage = {{"nand_directory", "D:/Anka/user/nand"},
                             {"sdmc_directory", "D:/Anka/user/sdmc"},
                             {"save_directory", ""}}};
}

} // Anonymous namespace

TEST_CASE("Anka route: only its game goes to the second core", "[yuzu][anka_route]") {
    REQUIRE(RunsOnSecondCore(0x0400486024DEC000ULL));
    REQUIRE(RunsOnSecondCore(0x0400486024DEC800ULL)); // its update
    REQUIRE_FALSE(RunsOnSecondCore(Bananza));
    REQUIRE_FALSE(RunsOnSecondCore(0));
    REQUIRE_FALSE(RunsOnSecondCore(0x0400486024DED000ULL));
    REQUIRE(SecondCoreProgramId == 0x0400486024DEC000ULL);
}

TEST_CASE("Anka route: the second core's names are the contract's", "[yuzu][anka_route]") {
    const fs::path user{"D:/Anka/user"};
    REQUIRE(SecondCoreIniPath(user) == user / "config" / "sdl2-config-core2.ini");
    REQUIRE(SecondCoreLogPath(user) == user / "log" / "core2" / "eden_log.txt");
    REQUIRE(Names::Folder == "core2");
    REQUIRE(Names::CliProgram == "anka-core2-cli.exe");
    REQUIRE(Names::QtProgram == "anka-core2.exe");
    REQUIRE(Names::CacheFolder == "core2");
    REQUIRE(Names::Lock == "Local\\AnkaCore2");
}

TEST_CASE("Anka route: the product's names carry no game", "[yuzu][anka_route]") {
    // The user's decision (08.11.2026): the game is named in what the user reads, never in the
    // package's folders, programs, files or lock
    for (const std::string_view name : {Names::Folder, Names::CliProgram, Names::QtProgram,
                                        Names::Ini, Names::CacheFolder, Names::LogFolder,
                                        Names::LogFile, Names::Lock}) {
        for (const std::string_view game : {"Zelda", "zelda", "ZELDA", "Ocarina", "ocarina", "OOT",
                                            "OoT", "oot", "Donkey", "Bananza", "Kong"}) {
            REQUIRE(name.find(game) == std::string_view::npos);
        }
    }
}

namespace {

// The highest %n of a text (0: none), and whether %1..%n are all there
int HighestArg(std::string_view text) {
    int highest = 0;
    for (std::size_t at = text.find('%'); at != std::string_view::npos;
         at = text.find('%', at + 1)) {
        if (at + 1 < text.size() && text[at + 1] >= '1' && text[at + 1] <= '9') {
            highest = std::max(highest, text[at + 1] - '0');
        }
    }
    for (int n = 1; n <= highest; ++n) {
        if (text.find("%" + std::to_string(n)) == std::string_view::npos) {
            return -1;
        }
    }
    return highest;
}

} // Anonymous namespace

TEST_CASE("Anka route: what the user reads names the game", "[yuzu][anka_route]") {
    REQUIRE(std::string_view{Text::GameName} == "The Legend of Zelda: Ocarina of Time");
    // Each text and its arguments: %1 the game wherever it is named, then the text's own
    struct Named {
        const char* text;
        int args;
    };
    const std::array named{
        Named{Text::Starting, 1},           Named{Text::Closed, 1},
        Named{Text::StillRunning, 1},       Named{Text::StopRunningFirst, 1},
        Named{Text::Missing, 2},            Named{Text::IniNotWritten, 2},
        Named{Text::SettingsNotRead, 2},    Named{Text::NotStarted, 2},
        Named{Text::StartFailed, 2},        Named{Text::AlreadyRunning, 1},
        Named{Text::Crashed, 3},            Named{Text::NoEffectOnSecondCore, 1},
        Named{Text::PresetOnSecondCore, 1}, Named{Text::OptionsTitle, 1},
        Named{Text::PropertiesNote, 0},     Named{Text::AddOnFilesInUse, 1},
        Named{Text::AlwaysOnAddOn, 0},
    };
    std::vector<std::string_view> all;
    for (const auto& [text, args] : named) {
        REQUIRE(HighestArg(text) == args);
        all.push_back(text);
    }
    for (const auto& option : SecondCoreOptions) {
        all.push_back(option.label);
        all.push_back(option.text);
        for (const auto& choice : option.choices) {
            all.push_back(choice.label);
        }
    }
    for (const std::string_view text : all) {
        REQUIRE_FALSE(text.empty());
        // "Second core" means nothing to the user; the name comes from GameName alone
        REQUIRE(text.find("second core") == std::string_view::npos);
        REQUIRE(text.find("Second core") == std::string_view::npos);
        REQUIRE(text.find("Zelda") == std::string_view::npos);
        REQUIRE(text.find("Ocarina") == std::string_view::npos);
    }
    REQUIRE(std::string_view{Text::Starting} == "Starting %1");
    REQUIRE(std::string_view{Text::Closed} == "%1 closed");
    REQUIRE(std::string_view{Text::StillRunning} ==
            "%1 is still running. Close it before starting another game.");
    REQUIRE(std::string_view{Text::StopRunningFirst} == "Stop the running game before starting %1.");
    REQUIRE(std::string_view{Text::PropertiesNote} ==
            "For this game only the add-on choices are used. Changes apply the next time the game "
            "starts.");
    // The missing file and the way out stay
    REQUIRE(std::string_view{Text::Missing} ==
            "%1 cannot start: %2 was not found. Extract the whole Anka package.");
    REQUIRE(std::string_view{Text::StartFailed} ==
            "%1 could not start. The details are in its log:\n%2");
    REQUIRE(std::string_view{Text::Crashed} ==
            "%1 closed unexpectedly (exit code 0x%2). Its log:\n%3");
    REQUIRE(std::string_view{Text::NoEffectOnSecondCore} == "No effect in %1");
    // The add-on window of its game: why its files can't change while it runs, and its kept patch
    REQUIRE(std::string_view{Text::AddOnFilesInUse} ==
            "%1 is running: its add-on files can't be changed until it closes.");
    REQUIRE(std::string_view{Text::AlwaysOnAddOn} == "Always on for this game");
    REQUIRE(SecondCoreKeptAddOn == "Dynamic FPS by Dark");
    REQUIRE(std::string_view{Text::PresetOnSecondCore}.starts_with(
        "In %1 Reactive Flushing stays off"));
    REQUIRE(std::string_view{Text::PresetOnSecondCore}.ends_with(
        "No preset changes that game's own settings."));
    // The group's title as the main core's game group has it
    REQUIRE(std::string_view{Text::OptionsTitle} == "%1 only (off in other games)");
    REQUIRE(std::string_view{Text::MaxResolution} == "Full-resolution 3D");
    REQUIRE(std::string_view{Text::VisualTweak} == "Haze and bloom tweak");
    REQUIRE(std::string_view{Text::OcclusionOff} == "Occlusion off (experimental)");
    // No speed figure is promised; the occlusion patch and the tweak promise none at all
    for (const auto& option : SecondCoreOptions) {
        REQUIRE(HighestArg(option.text) == 0);
        REQUIRE(std::string_view{option.text}.find('%') == std::string_view::npos);
    }
    REQUIRE(std::string_view{Text::OcclusionOffText}.ends_with("no measured speed change."));
    REQUIRE(std::string_view{Text::VisualTweakText}.ends_with("No effect on speed."));
}

TEST_CASE("Anka route: a UTF-8 settings path under a folder named in any script",
          "[yuzu][anka_route]") {
    // "Ö ü ş 日本", as Config::GetConfigFilePath holds it (UTF-8)
    const std::string folder = "\xC3\x96 \xC3\xBC \xC5\x9F \xE6\x97\xA5\xE6\x9C\xAC";
    REQUIRE(PathFromUtf8(folder).u16string() == u"Ö ü ş 日本");
    TempDir t;
    const fs::path dir = t.path / PathFromUtf8(folder);
    fs::create_directories(dir);
    const fs::path file = dir / "qt-config.ini";
    const std::string settings = "[Controls]\nplayer_0_button_a=\"engine:sdl,button:1\"\n";
    std::ofstream{file, std::ios::binary} << settings;
    const auto utf8 = file.u8string();
    const std::string config_path(utf8.begin(), utf8.end());
    REQUIRE(PathFromUtf8(config_path) == file);
    REQUIRE(ReadStartingSettings(PathFromUtf8(config_path)) == settings);
}

TEST_CASE("Anka route: no settings read, no second core settings written", "[yuzu][anka_route]") {
    TempDir t;
    REQUIRE_FALSE(ReadStartingSettings(t.path / "missing.ini").has_value());
    std::ofstream{t.path / "empty.ini"};
    REQUIRE_FALSE(ReadStartingSettings(t.path / "empty.ini").has_value());
    std::ofstream{t.path / "blank.ini"} << "\r\n\n  \n";
    REQUIRE_FALSE(ReadStartingSettings(t.path / "blank.ini").has_value());
    std::ofstream{t.path / "one.ini", std::ios::binary} << "[UI]\n";
    REQUIRE(ReadStartingSettings(t.path / "one.ini") == std::string{"[UI]\n"});
    REQUIRE(std::string_view{Text::SettingsNotRead}.find("%1") != std::string_view::npos);
}

TEST_CASE("Anka route: the second core in the package", "[yuzu][anka_route]") {
    TempDir t;
    const fs::path cli = t.path / Names::Folder / Names::CliProgram;
    const fs::path qt = t.path / Names::Folder / Names::QtProgram;
    REQUIRE_FALSE(FindSecondCore(t.path, true).has_value());
    REQUIRE_FALSE(FindSecondCore(t.path, false).has_value());
    Touch(cli);
    REQUIRE(FindSecondCore(t.path, true) == cli);
    REQUIRE(FindSecondCore(t.path, false) == cli);
    Touch(qt);
    REQUIRE(FindSecondCore(t.path, true) == qt);
    REQUIRE(FindSecondCore(t.path, false) == cli);
}

TEST_CASE("Anka route: the second core's command line and environment", "[yuzu][anka_route]") {
    const fs::path core = fs::path{"D:/Anka"} / Names::Folder / Names::CliProgram;
    const fs::path user = fs::path{"D:/Anka"} / "user";
    const auto ini = SecondCoreIniPath(user).make_preferred().u8string();
    const std::string ini_utf8(ini.begin(), ini.end());

    const auto windowed = PlanSecondCore(core, user, "D:/Games/Game.nup", false, false);
    REQUIRE(windowed.program == fs::path{core}.make_preferred());
    REQUIRE(windowed.working_dir == fs::path{core}.make_preferred().parent_path());
    REQUIRE(windowed.args == std::vector<std::string>{"-c", ini_utf8, "-g", "D:/Games/Game.nup"});
    REQUIRE(windowed.env.size() == 1);
    REQUIRE(windowed.env[0].first == "ANKA_USER_DIR");

    const auto full = PlanSecondCore(core, user, "D:/Games/Game.nup", true, true);
    REQUIRE(full.args.front() == "-f");
    REQUIRE(full.args.size() == 5);
    REQUIRE(full.env.size() == 2);
    REQUIRE(full.env[1] == std::pair<std::string, std::string>{"EDEN_CLI_RESTART_HANDOFF", "1"});

    // Settings variables between ANKA_USER_DIR and the handoff
    const auto with_settings =
        PlanSecondCore(core, user, "D:/Games/Game.nup", false, true, Env{{"EDEN_X", "1"}});
    REQUIRE(with_settings.env.size() == 3);
    REQUIRE(with_settings.env[0].first == "ANKA_USER_DIR");
    REQUIRE(with_settings.env[1] == std::pair<std::string, std::string>{"EDEN_X", "1"});
    REQUIRE(with_settings.env[2].first == "EDEN_CLI_RESTART_HANDOFF");
}

TEST_CASE("Anka route: the settings table gives the second core's variables",
          "[yuzu][anka_route]") {
    constexpr std::array<SecondCoreSetting, 2> table{{
        {"osd", "EDEN_OOT_OSD", "1"},
        {"jit_same", "EDEN_OOT_JIT", "2"},
    }};
    constexpr std::array<SecondCorePresetVar, 2> presets{{
        {1, "EDEN_OOT_REACTIVE", "1"},
        {2, "EDEN_OOT_SAFE", "1"},
    }};
    const auto osd_only = [](std::string_view key) { return key == "osd"; };
    REQUIRE(SecondCoreEnv(osd_only, 0, table, presets) == Env{{"EDEN_OOT_OSD", "1"}});
    REQUIRE(SecondCoreEnv(AllOn, 1, table, presets) ==
            Env{{"EDEN_OOT_OSD", "1"}, {"EDEN_OOT_JIT", "2"}, {"EDEN_OOT_REACTIVE", "1"}});
    REQUIRE(SecondCoreEnv([](std::string_view) { return false; }, 3, table, presets).empty());
    REQUIRE(ReachesSecondCore("osd", table));
    REQUIRE_FALSE(ReachesSecondCore("bananza_dynamic_fps", table));
}

TEST_CASE("Anka route: only OSD and timed frame generation reach the second core",
          "[yuzu][anka_route]") {
    REQUIRE(ReachesSecondCore("osd"));
    REQUIRE(ReachesSecondCore("fg_timed"));
    // No counterpart there: the tab marks them, they set nothing
    for (const std::string_view key :
         {"split_gpu_thread", "acquire_peek", "jit_same", "front_macro_lean",
          "bananza_dynamic_fps", "bananza_frame_pacing"}) {
        REQUIRE_FALSE(ReachesSecondCore(key));
        REQUIRE(SecondCoreEnv([key](std::string_view k) { return k == key; }, Recommended)
                    .empty());
    }
    // Every table key is one of the Anka tab's
    for (const auto& setting : SecondCoreSettings) {
        REQUIRE(std::any_of(Perf::Keys.begin(), Perf::Keys.end(),
                            [&](const Perf::Key& key) { return key.name == setting.key; }));
    }
}

TEST_CASE("Anka route: each preset's variables on the second core", "[yuzu][anka_route]") {
    const Env boxes{{"EDEN_NX2_FG_TIMED", "1"}, {"EDEN_NX2_OSD", "1"}};
    REQUIRE(EnvFor(Perf::Preset::Recommended) == boxes);
    // Reactive: as Recommended (RX_RT_FIT has no reader there)
    REQUIRE(EnvFor(Perf::Preset::Reactive) == boxes);
    // Safe: its boxes are off, 8 shortcuts at 0 and the second core's own GPU thread split off
    REQUIRE(EnvFor(Perf::Preset::Safe) == Env{{"EDEN_NX2_UBO_BIND_SKIP", "0"},
                                              {"EDEN_NX2_MARK_WORDS", "0"},
                                              {"EDEN_NX2_SLOT_SKIP", "0"},
                                              {"EDEN_NX2_TICK_SKIP", "0"},
                                              {"EDEN_NX2_HLE_INDIRECT", "0"},
                                              {"EDEN_NX2_LAYOUT_MEMO", "0"},
                                              {"EDEN_NX2_FAST_PRIORITY_SCAN", "0"},
                                              {"EDEN_NX2_PBFLUSH_WINDOW_KIB", "0"},
                                              {"EDEN_NX2_SPLIT", "0"}});
    // Safe with OSD turned back on is Custom: the box alone
    Perf::Switches custom = Perf::AfterBoxChange([] {
        Perf::Switches s = Perf::AllOff();
        s.osd = true;
        return s;
    }());
    REQUIRE(custom.preset == Perf::Preset::Custom);
    REQUIRE(EnvFor(custom) == Env{{"EDEN_NX2_OSD", "1"}});
    // The Safe names are SafeOff's (but the second core's own split), the preset number
    // Perf::Preset::Safe
    REQUIRE(SafePreset == static_cast<int>(Perf::Preset::Safe));
    for (const auto& var : SecondCorePresetVars) {
        REQUIRE(var.preset == SafePreset);
        REQUIRE(var.value == "0");
        if (var.env == "EDEN_NX2_SPLIT") {
            continue;
        }
        REQUIRE(std::find(Perf::SafeOff.begin(), Perf::SafeOff.end(), var.env) !=
                Perf::SafeOff.end());
    }
}

TEST_CASE("Anka route: boxes off, the second core gets ANKA_USER_DIR alone", "[yuzu][anka_route]") {
    const auto none = [](std::string_view) { return false; };
    for (const int preset : {Recommended, static_cast<int>(Perf::Preset::Reactive),
                             static_cast<int>(Perf::Preset::Custom)}) {
        REQUIRE(SecondCoreEnv(none, preset).empty());
        const auto launch = PlanSecondCore("D:/Anka/core2/x.exe", "D:/Anka/user", "g.nup", false,
                                           false, SecondCoreEnv(none, preset));
        REQUIRE(launch.env.size() == 1);
        REQUIRE(launch.env[0].first == "ANKA_USER_DIR");
    }
}

TEST_CASE("Anka route: never the main core's 256 KiB flush window", "[yuzu][anka_route]") {
    for (int preset = 0; preset <= 3; ++preset) {
        for (const bool on : {false, true}) {
            for (const auto& [name, value] :
                 SecondCoreEnv([on](std::string_view) { return on; }, preset)) {
                REQUIRE(value != "256");
                if (name == "EDEN_NX2_PBFLUSH_WINDOW_KIB") {
                    REQUIRE(value == "0");
                }
            }
        }
    }
    // Left in the menu's environment by a main core game: not passed on either
    const auto launch = PlanSecondCore("D:/Anka/core2/x.exe", "D:/Anka/user", "g.nup", false, false,
                                       EnvFor(Perf::Preset::Recommended));
    for (const auto& [name, value] :
         SecondCoreEnvironment({{"EDEN_NX2_PBFLUSH_WINDOW_KIB", "256"}}, launch)) {
        REQUIRE(value != "256");
    }
}

TEST_CASE("Anka route: a main core game's variables are not inherited", "[yuzu][anka_route]") {
    // The menu after Bananza: Nx2PerfSession set its plan in the menu's environment (the 11
    // variables cb99db20b9 passed on), and a launcher may have set more
    Env inherited{{"PATH", "C:/Windows"},
                  {"ANKA_USER_DIR", "C:/elsewhere"},
                  {"__GL_SHADER_DISK_CACHE_PATH", "C:/cache"},
                  {"EDEN_CLI_RESTART_HANDOFF", "1"},
                  {"EDEN_NX2_PBFLUSH_WINDOW_KIB", "256"}};
    const auto bananza_plan = Perf::Wanted(Perf::Recommended(), true);
    REQUIRE(bananza_plan.size() == 11);
    for (const auto& var : bananza_plan) {
        inherited.emplace_back(var.name, var.value);
    }
    for (const auto preset : {Perf::Preset::Recommended, Perf::Preset::Reactive,
                              Perf::Preset::Safe}) {
        const Env own = EnvFor(preset);
        const auto launch = PlanSecondCore("D:/Anka/core2/x.exe", "D:/Anka/user", "g.nup", false,
                                           false, own);
        const auto env = SecondCoreEnvironment(inherited, launch);
        // PATH, then ANKA_USER_DIR (its own, once), then the table's: nothing else
        Env expected{{"PATH", "C:/Windows"}, launch.env[0]};
        expected.insert(expected.end(), own.begin(), own.end());
        REQUIRE(env == expected);
        REQUIRE(launch.env[0].second != "C:/elsewhere");
    }
    // Names set by the launch replace inherited ones in any case (Windows)
    const auto launch = PlanSecondCore("D:/Anka/core2/x.exe", "D:/Anka/user", "g.nup", false, true);
    const auto env = SecondCoreEnvironment({{"anka_user_dir", "C:/x"}, {"Path", "C:/W"}}, launch);
    REQUIRE(env.size() == 3);
    REQUIRE(env[0] == std::pair<std::string, std::string>{"Path", "C:/W"});
    REQUIRE(env[1].first == "ANKA_USER_DIR");
    REQUIRE(env[2] == std::pair<std::string, std::string>{"EDEN_CLI_RESTART_HANDOFF", "1"});
}

TEST_CASE("Anka route: the starting program's switches are not passed on", "[yuzu][anka_route]") {
    REQUIRE(ClearedForSecondCore("EDEN_NX2_PBFLUSH"));
    REQUIRE(ClearedForSecondCore("EDEN_NX2_DK_DYN"));
    REQUIRE(ClearedForSecondCore("EDEN_OOT_ANYTHING"));
    REQUIRE(ClearedForSecondCore("EDEN_NUP_CACHE_MIB"));
    REQUIRE(ClearedForSecondCore("EDEN_CLI_RESTART_HANDOFF"));
    REQUIRE(ClearedForSecondCore("__GL_SHADER_DISK_CACHE"));
    REQUIRE(ClearedForSecondCore("__GL_SHADER_DISK_CACHE_PATH"));
    // Windows' variable names have no case
    REQUIRE(ClearedForSecondCore("eden_nx2_x"));
    REQUIRE(ClearedForSecondCore("Eden_Nx2_Osd"));
    REQUIRE(ClearedForSecondCore("__gl_shader_disk_cache_path"));
    static_assert(ClearedForSecondCore("eden_cli_restart_handoff"));
    REQUIRE_FALSE(ClearedForSecondCore("eden"));
    REQUIRE_FALSE(ClearedForSecondCore("anka_user_dir"));
    REQUIRE_FALSE(ClearedForSecondCore("__gl_threaded_optimizations"));
    REQUIRE_FALSE(ClearedForSecondCore("ANKA_USER_DIR"));
    REQUIRE_FALSE(ClearedForSecondCore("PATH"));
    REQUIRE_FALSE(ClearedForSecondCore("XEDEN_NX2"));
    REQUIRE_FALSE(ClearedForSecondCore("__GL_THREADED_OPTIMIZATIONS"));
}

TEST_CASE("Anka route: Windows command line quoting", "[yuzu][anka_route]") {
    REQUIRE(QuoteWindowsArg("-g") == "-g");
    REQUIRE(QuoteWindowsArg("") == "\"\"");
    REQUIRE(QuoteWindowsArg("D:\\My Games\\Game.nup") == "\"D:\\My Games\\Game.nup\"");
    REQUIRE(QuoteWindowsArg("D:\\path\\") == "D:\\path\\");
    REQUIRE(QuoteWindowsArg("D:\\a b\\") == "\"D:\\a b\\\\\"");
    REQUIRE(QuoteWindowsArg("say \"hi\"") == "\"say \\\"hi\\\"\"");
    REQUIRE(WindowsCommandLine(fs::path{"C:/x y/z.exe"}, {"-f", "-g", "a b"}) ==
            "\"C:/x y/z.exe\" -f -g \"a b\"");
}

TEST_CASE("Anka route: what the exit codes mean", "[yuzu][anka_route]") {
    REQUIRE(ForExitCode(0) == ExitReaction::None);
    REQUIRE(ForExitCode(1) == ExitReaction::StartFailed);
    REQUIRE(ForExitCode(75) == ExitReaction::Restart);
    REQUIRE(ForExitCode(76) == ExitReaction::AlreadyRunning);
    REQUIRE(ForExitCode(2) == ExitReaction::Crashed);
    REQUIRE(ForExitCode(0xC0000005U) == ExitReaction::Crashed);
}

TEST_CASE("Anka route: written keys carry their \\default=false line", "[yuzu][anka_route]") {
    const auto ini = MakeSecondCoreIni(QtConfig, State(true));
    REQUIRE(Has(ini, "use_docked_mode\\default=false"));
    REQUIRE(Has(ini, "nand_directory\\default=false"));
    REQUIRE(Has(ini, "sdmc_directory\\default=false"));
    REQUIRE(Has(ini, "save_directory\\default=false"));
    REQUIRE_FALSE(Has(ini, "use_docked_mode\\default=true"));
    REQUIRE_FALSE(Has(ini, "nand_directory\\default=true"));
}

TEST_CASE("Anka route: docked or handheld always written", "[yuzu][anka_route]") {
    REQUIRE(Has(MakeSecondCoreIni(QtConfig, State(true)), "use_docked_mode=1"));
    REQUIRE(Has(MakeSecondCoreIni(QtConfig, State(false)), "use_docked_mode=0"));
    // A config without a [System] section gets one
    const auto ini = MakeSecondCoreIni("[Renderer]\nanti_aliasing=0\n", State(false));
    REQUIRE(Has(ini, "[System]"));
    REQUIRE(Has(ini, "use_docked_mode=0"));
}

TEST_CASE("Anka route: Reactive Flushing always off on the second core", "[yuzu][anka_route]") {
    // As each preset leaves the menu's file: Recommended off, Reactive on, Safe / Custom as it
    // was (here: the default, no key)
    const std::string base = QtConfig;
    const std::string renderer = "[Renderer]\n";
    const auto with = [&](const std::string& lines) {
        std::string text = base;
        text.insert(text.find(renderer) + renderer.size(), lines);
        return text;
    };
    for (const auto& config :
         {with("use_reactive_flushing\\default=false\nuse_reactive_flushing=false\n"),
          with("use_reactive_flushing\\default=false\nuse_reactive_flushing=true\n"),
          with("use_reactive_flushing\\default=true\nuse_reactive_flushing=true\n"), base}) {
        const auto ini = MakeSecondCoreIni(config, State(true));
        REQUIRE(Has(ini, "use_reactive_flushing\\default=false"));
        REQUIRE(Has(ini, "use_reactive_flushing=false"));
        REQUIRE_FALSE(Has(ini, "use_reactive_flushing=true"));
        REQUIRE_FALSE(Has(ini, "use_reactive_flushing\\default=true"));
        // In [Renderer], where the settings read it (Category::RendererAdvanced)
        const auto section = ini.find(renderer);
        const auto key = ini.find("use_reactive_flushing=");
        REQUIRE(section != std::string::npos);
        REQUIRE(key > section);
        REQUIRE(ini.find("\n[", section + 1) > key);
    }
    // Barrier feedback loops always on there (its default; the main core's Recommended turns them
    // off), in [Renderer] as well
    for (const auto& config :
         {with("barrier_feedback_loops\\default=false\nbarrier_feedback_loops=false\n"),
          with("barrier_feedback_loops\\default=false\nbarrier_feedback_loops=true\n"),
          with("barrier_feedback_loops\\default=true\nbarrier_feedback_loops=true\n"), base}) {
        const auto ini = MakeSecondCoreIni(config, State(true));
        REQUIRE(Has(ini, "barrier_feedback_loops\\default=false"));
        REQUIRE(Has(ini, "barrier_feedback_loops=true"));
        REQUIRE_FALSE(Has(ini, "barrier_feedback_loops=false"));
        const auto section = ini.find(renderer);
        const auto key = ini.find("barrier_feedback_loops=");
        REQUIRE(key > section);
        REQUIRE(ini.find("\n[", section + 1) > key);
    }
    // A file without [Renderer] gets one
    const auto ini = MakeSecondCoreIni("[System]\nuse_docked_mode=1\n", State(true));
    REQUIRE(Has(ini, "[Renderer]"));
    REQUIRE(Has(ini, "use_reactive_flushing=false"));
    REQUIRE(Has(ini, "barrier_feedback_loops=true"));
}

TEST_CASE("Anka route: unchanged AA, filter and ASTC stay the second core's",
          "[yuzu][anka_route]") {
    const auto ini = MakeSecondCoreIni(QtConfig, State(true));
    REQUIRE(Has(ini, "anti_aliasing\\default=true"));
    REQUIRE(Has(ini, "astc_recompression\\default=true"));
    // Changed in the menu: the menu's value
    REQUIRE(Has(ini, "scaling_filter\\default=false"));
    REQUIRE(Has(ini, "scaling_filter=1"));
    // Settings only the second core has are not written
    REQUIRE(ini.find("dk_30fps") == std::string::npos);
    // Controls go over as they are
    REQUIRE(Has(ini, "player_0_button_a=\"engine:sdl,button:1\""));
}

TEST_CASE("Anka route: frame generation goes over in the settings file", "[yuzu][anka_route]") {
    const auto ini = MakeSecondCoreIni(QtConfig, State(true));
    for (const char* line :
         {"frame_gen\\default=false", "frame_gen=true", "frame_gen_type\\default=true",
          "frame_gen_type=1", "frame_gen_multiplier\\default=false", "frame_gen_multiplier=2",
          "frame_gen_target_rate\\default=false", "frame_gen_target_rate=120"}) {
        REQUIRE(Has(ini, line));
    }
}

TEST_CASE("Anka route: its game keeps its frame rate patch", "[yuzu][anka_route]") {
    const auto ini = MakeSecondCoreIni(QtConfig, State(true));
    REQUIRE(Has(ini, "size=2"));
    REQUIRE(Has(ini, "1\\title_id=288309953924349952"));
    REQUIRE(Has(ini, "1\\disabled\\1\\d=Other Mod"));
    REQUIRE(Has(ini, "1\\disabled\\size=1"));
    REQUIRE_FALSE(Has(ini, "1\\disabled\\2\\d=Other Mod"));
    // Another title's list is left alone
    REQUIRE(Has(ini, "2\\disabled\\1\\d=Dynamic FPS by Dark"));
    REQUIRE(Has(ini, "2\\disabled\\size=1"));
}

TEST_CASE("Anka route: add-on entries past the sizes stay gone", "[yuzu][anka_route]") {
    // The menu's list got shorter: Config::SaveDisabledAddOnValues rewrote the sizes and left the
    // old entries (title 2, and the third add-on of title 1) where they were
    const std::string shortened = "[DisabledAddOns]\n"
                                  "size=1\n"
                                  "1\\title_id\\default=false\n"
                                  "1\\title_id=288309953924349952\n"
                                  "1\\disabled\\1\\d\\default=false\n"
                                  "1\\disabled\\1\\d=Dynamic FPS by Dark\n"
                                  "1\\disabled\\2\\d\\default=false\n"
                                  "1\\disabled\\2\\d=Other Mod\n"
                                  "1\\disabled\\3\\d\\default=false\n"
                                  "1\\disabled\\3\\d=Old Mod\n"
                                  "1\\disabled\\size=2\n"
                                  "2\\title_id\\default=false\n"
                                  "2\\title_id=288358607164088320\n"
                                  "2\\disabled\\1\\d\\default=false\n"
                                  "2\\disabled\\1\\d=Stale Mod\n"
                                  "2\\disabled\\size=1\n";
    const auto ini = MakeSecondCoreIni(shortened, State(true));
    REQUIRE(Has(ini, "size=1"));
    REQUIRE(Has(ini, "1\\title_id=288309953924349952"));
    REQUIRE(Has(ini, "1\\disabled\\1\\d=Other Mod"));
    REQUIRE(Has(ini, "1\\disabled\\size=1"));
    REQUIRE(ini.find("Dynamic FPS by Dark") == std::string::npos);
    REQUIRE(ini.find("Old Mod") == std::string::npos);
    REQUIRE(ini.find("Stale Mod") == std::string::npos);
    REQUIRE(ini.find("288358607164088320") == std::string::npos);
    REQUIRE(ini.find("2\\") == std::string::npos);
}

TEST_CASE("Anka route: add-on entries without a size are not read", "[yuzu][anka_route]") {
    // No size line: the reader takes no entry, so there is nothing to keep on and nothing moves
    const std::string no_size = "[DisabledAddOns]\n"
                                "1\\title_id=288309953924349952\n"
                                "1\\disabled\\1\\d=Dynamic FPS by Dark\n"
                                "1\\disabled\\size=1\n";
    const auto ini = MakeSecondCoreIni(no_size, State(true));
    REQUIRE(ini.starts_with(no_size));
    REQUIRE_FALSE(Has(ini, "size=0"));
    // A size but no disabled\size: that title disables nothing, and its stale add-on stays unread
    const std::string no_disabled_size = "[DisabledAddOns]\n"
                                         "size=1\n"
                                         "1\\title_id=288309953924349952\n"
                                         "1\\disabled\\1\\d=Dynamic FPS by Dark\n";
    REQUIRE(MakeSecondCoreIni(no_disabled_size, State(true)).starts_with(no_disabled_size));
}

TEST_CASE("Anka route: Dark's patch as the only disabled add-on", "[yuzu][anka_route]") {
    const std::string only_dark = "[DisabledAddOns]\n"
                                  "size=1\n"
                                  "1\\title_id\\default=false\n"
                                  "1\\title_id=288309953924349952\n"
                                  "1\\disabled\\1\\d\\default=false\n"
                                  "1\\disabled\\1\\d=Dynamic FPS by Dark\n"
                                  "1\\disabled\\size=1\n";
    const auto ini = MakeSecondCoreIni(only_dark, State(true));
    REQUIRE(Has(ini, "size=1"));
    REQUIRE(Has(ini, "1\\title_id=288309953924349952"));
    REQUIRE(Has(ini, "1\\disabled\\size=0"));
    REQUIRE(ini.find("1\\disabled\\1\\d") == std::string::npos);
    REQUIRE(ini.find("Dynamic FPS by Dark") == std::string::npos);
}

TEST_CASE("Anka route: the data storage paths as the menu uses them", "[yuzu][anka_route]") {
    const auto ini = MakeSecondCoreIni(QtConfig, State(true));
    REQUIRE(Has(ini, "nand_directory=D:/Anka/user/nand"));
    REQUIRE(Has(ini, "sdmc_directory=D:/Anka/user/sdmc"));
    REQUIRE(Has(ini, "save_directory="));
    REQUIRE(ini.find("C:/old/nand") == std::string::npos);
    // A path QSettings quotes
    IniState state = State(true);
    state.data_storage = {{"nand_directory", "D:/a,b/nand"}};
    REQUIRE(Has(MakeSecondCoreIni(QtConfig, state), "nand_directory=\"D:/a,b/nand\""));
}

TEST_CASE("Anka route: CRLF files stay CRLF and nothing else moves", "[yuzu][anka_route]") {
    const std::string crlf = "[UI]\r\ntheme=dark\r\n\r\n[System]\r\nuse_docked_mode=1\r\n";
    const auto ini = MakeSecondCoreIni(crlf, IniState{.docked = true, .data_storage = {}});
    REQUIRE(ini.starts_with("[UI]\r\ntheme=dark\r\n\r\n[System]\r\n"));
    REQUIRE(ini.find("\r\nuse_docked_mode\\default=false\r\nuse_docked_mode=1\r\n") !=
            std::string::npos);
    REQUIRE(ini.find("\n\n") == std::string::npos);
}

namespace {

constexpr std::size_t MaxResolutionAt = 0;
constexpr std::size_t VisualTweakAt = 1;
constexpr std::size_t OcclusionOffAt = 2;

SecondCoreValues Options(int max_resolution, int visual_tweak, int occlusion_off) {
    SecondCoreValues values{};
    values[MaxResolutionAt] = max_resolution;
    values[VisualTweakAt] = visual_tweak;
    values[OcclusionOffAt] = occlusion_off;
    return values;
}

Env OptionEnv(int max_resolution, int visual_tweak, int occlusion_off) {
    return Env{{"EDEN_OOT_MAX_RESOLUTION", max_resolution ? "1" : "0"},
               {"EDEN_NX2_OOT_VISUAL_WORKAROUND", visual_tweak ? "1" : "0"},
               {"EDEN_OOT_OCCLUSION_OFF", occlusion_off ? "1" : "0"}};
}

// A line pair as Config::WritePreparedSetting writes it: the default line, then the value
void WriteLine(std::string& text, std::string_view key, const std::string& value,
               bool is_default) {
    text += std::string{key} + "\\default=" + (is_default ? "true" : "false") + "\n";
    text += std::string{key} + "=" + value + "\n";
}

// The [Anka] lines QtConfig writes for the options of `table`
template <std::size_t N>
std::string MenuAnkaGroup(std::span<const int> values,
                          const std::array<SecondCoreOption, N>& table) {
    std::string text = "[Anka]\n";
    SaveSecondCoreOptions(
        table, values,
        [&](std::string_view key, bool value, bool def) {
            WriteLine(text, key, value ? "true" : "false", value == def);
        },
        [&](std::string_view key, int value, int def) {
            WriteLine(text, key, std::to_string(value), value == def);
        });
    return text;
}

std::string MenuAnkaGroup(const SecondCoreValues& values) {
    return MenuAnkaGroup(values, SecondCoreOptions);
}

// Every variable the main core's plan can set, for any boxes, preset and game
std::vector<std::string_view> MainCoreNames() {
    std::vector<std::string_view> names{Perf::SafeOff.begin(), Perf::SafeOff.end()};
    for (const bool bananza : {false, true}) {
        for (const auto& switches : {Perf::Recommended(), Perf::AllOff()}) {
            for (const auto& var : Perf::Wanted(switches, bananza)) {
                names.push_back(var.name);
            }
        }
    }
    return names;
}

} // Anonymous namespace

TEST_CASE("Anka route: the second core's options are one table", "[yuzu][anka_route]") {
    REQUIRE(SecondCoreOptions.size() == 3);
    REQUIRE(SecondCoreOptions[MaxResolutionAt].key == "second_core_max_resolution");
    REQUIRE(SecondCoreOptions[VisualTweakAt].key == "second_core_visual_tweak");
    REQUIRE(SecondCoreOptions[OcclusionOffAt].key == "second_core_occlusion_off");
    REQUIRE(SecondCoreOptions[MaxResolutionAt].name == "EDEN_OOT_MAX_RESOLUTION");
    REQUIRE(SecondCoreOptions[VisualTweakAt].name == "EDEN_NX2_OOT_VISUAL_WORKAROUND");
    REQUIRE(SecondCoreOptions[OcclusionOffAt].name == "EDEN_OOT_OCCLUSION_OFF");
    // The second core's own defaults: full-resolution 3D on, the haze and bloom tweak on,
    // occlusion off off
    REQUIRE(SecondCoreDefaults() == Options(1, 1, 0));
    const auto main_core = MainCoreNames();
    for (std::size_t i = 0; i < SecondCoreOptions.size(); ++i) {
        const auto& option = SecondCoreOptions[i];
        REQUIRE(option.kind == OptionKind::Toggle);
        REQUIRE(option.target == OptionTarget::Env);
        // Not a key of the main core's settings: no preset, plan or DKB variable sees it
        REQUIRE(std::none_of(Perf::Keys.begin(), Perf::Keys.end(),
                             [&](const Perf::Key& key) { return key.name == option.key; }));
        REQUIRE(option.key != "preset");
        REQUIRE_FALSE(ReachesSecondCore(option.key));
        for (std::size_t j = i + 1; j < SecondCoreOptions.size(); ++j) {
            REQUIRE(SecondCoreOptions[j].key != option.key);
            REQUIRE(SecondCoreOptions[j].name != option.name);
        }
        REQUIRE(option.label != nullptr);
        REQUIRE(option.text != nullptr);
        // An EDEN_* name: the starting program's own value of it is never passed on
        REQUIRE(option.name.starts_with("EDEN_"));
        REQUIRE(ClearedForSecondCore(option.name));
        // Never a name the presets or the tab's boxes set there, nor one of the main core's plan
        for (const auto& var : SecondCorePresetVars) {
            REQUIRE(var.env != option.name);
        }
        for (const auto& setting : SecondCoreSettings) {
            REQUIRE(setting.env != option.name);
        }
        REQUIRE(std::find(main_core.begin(), main_core.end(), option.name) == main_core.end());
    }
}

TEST_CASE("Anka route: the options' variables, always written as 1 or 0", "[yuzu][anka_route]") {
    // The default: all written, at the second core's own default
    REQUIRE(SecondCoreOptionEnv(SecondCoreDefaults()) == OptionEnv(1, 1, 0));
    for (const int a : {0, 1}) {
        for (const int b : {0, 1}) {
            for (const int c : {0, 1}) {
                REQUIRE(SecondCoreOptionEnv(Options(a, b, c)) == OptionEnv(a, b, c));
            }
        }
    }
    // A box's value is 0 or 1 whatever the file held
    REQUIRE(SecondCoreOptionEnv(Options(7, 0, -2)) == OptionEnv(1, 0, 1));
    // Fewer values than options: the defaults
    REQUIRE(SecondCoreOptionEnv(std::span<const int>{}) == OptionEnv(1, 1, 0));
    // Exactly "1" / "0": the second core reads the first character, the tweak "0" alone
    for (const auto& [name, value] : SecondCoreOptionEnv(Options(1, 0, 1))) {
        REQUIRE((value == "1" || value == "0"));
    }
    // Nothing for the settings file
    for (const auto& out : SecondCoreOptionOutputs(SecondCoreDefaults())) {
        REQUIRE(out.target == OptionTarget::Env);
    }
}

TEST_CASE("Anka route: the section's value wins over the starting program's",
          "[yuzu][anka_route]") {
    // A launcher or the user set them for the menu: they never reach the second core
    const Env inherited{{"PATH", "C:/Windows"},
                        {"EDEN_OOT_MAX_RESOLUTION", "1"},
                        {"eden_oot_occlusion_off", "1"},
                        {"EDEN_NX2_OOT_VISUAL_WORKAROUND", "1"},
                        {"Eden_Nx2_Oot_Visual_Workaround", "0"},
                        {"EDEN_OOT_DYNAMIC_FPS", "0"}};
    for (const int a : {0, 1}) {
        for (const int b : {0, 1}) {
            for (const int c : {0, 1}) {
                Env own = EnvFor(Perf::Preset::Recommended);
                const auto options = SecondCoreOptionEnv(Options(a, b, c));
                own.insert(own.end(), options.begin(), options.end());
                const auto launch = PlanSecondCore("D:/Anka/core2/x.exe", "D:/Anka/user", "g.nup",
                                                   false, false, own);
                const auto env = SecondCoreEnvironment(inherited, launch);
                for (const auto& [name, value] : OptionEnv(a, b, c)) {
                    REQUIRE(std::count_if(env.begin(), env.end(), [&](const auto& var) {
                                return Detail::SameText(var.first, name);
                            }) == 1);
                    REQUIRE(std::count(env.begin(), env.end(),
                                       std::pair<std::string, std::string>{name, value}) == 1);
                }
                for (const auto& [name, value] : env) {
                    REQUIRE(name != "EDEN_OOT_DYNAMIC_FPS");
                }
            }
        }
    }
}

TEST_CASE("Anka route: the main core never gets the tweak's variable", "[yuzu][anka_route]") {
    // Its plan (Nx2PerfSession sets it in the menu's own environment) has no such name, whatever
    // the boxes, preset or game
    for (const bool bananza : {false, true}) {
        for (const auto preset : {Perf::Preset::Recommended, Perf::Preset::Reactive,
                                  Perf::Preset::Safe, Perf::Preset::Custom}) {
            for (const auto& var :
                 Perf::Wanted(Perf::FromPreset(preset, Perf::Recommended()), bananza)) {
                REQUIRE_FALSE(var.name.starts_with("EDEN_OOT_"));
                REQUIRE(var.name != "EDEN_NX2_OOT_VISUAL_WORKAROUND");
            }
        }
    }
    REQUIRE(std::find(Perf::SafeOff.begin(), Perf::SafeOff.end(),
                      "EDEN_NX2_OOT_VISUAL_WORKAROUND") == Perf::SafeOff.end());
    // The second core's tab settings and presets do not set it either: only the section
    for (int preset = 0; preset <= 3; ++preset) {
        for (const auto& [name, value] : SecondCoreEnv(AllOn, preset)) {
            REQUIRE(name != "EDEN_NX2_OOT_VISUAL_WORKAROUND");
        }
    }
}

TEST_CASE("Anka route: the game's DLSS setting never goes over", "[yuzu][anka_route]") {
    // No effect in the second core's game: the menu's value is dropped, nothing is written
    const std::string renderer = "[Renderer]\n";
    std::string config = QtConfig;
    config.insert(config.find(renderer) + renderer.size(),
                  "game_dlss_network\\default=false\ngame_dlss_network=true\n");
    for (const auto& values : {Options(1, 1, 0), Options(0, 0, 1)}) {
        IniState state = State(true);
        state.options.assign(values.begin(), values.end());
        const auto ini = MakeSecondCoreIni(config, state);
        REQUIRE(ini.find("game_dlss_network") == std::string::npos);
        // The variables are not written into the file
        REQUIRE(ini.find("EDEN_") == std::string::npos);
    }
}

TEST_CASE("Anka route: presets leave the second core's options alone", "[yuzu][anka_route]") {
    // The options are none of the preset's boxes: every preset keeps them as chosen
    const SecondCoreValues chosen = Options(0, 0, 1);
    for (const auto preset : {Perf::Preset::Recommended, Perf::Preset::Reactive,
                              Perf::Preset::Safe, Perf::Preset::Custom}) {
        const Perf::Switches switches = Perf::FromPreset(preset, Perf::Recommended());
        // The tab's whole [Anka] group: the boxes and preset, then the options
        std::string group = "[Anka]\n";
        Perf::Save(
            switches,
            [&](std::string_view key, bool value, bool def) {
                WriteLine(group, key, value ? "true" : "false", value == def);
            },
            [&](std::string_view key, int value, int def) {
                WriteLine(group, key, std::to_string(value), value == def);
            });
        group += MenuAnkaGroup(chosen);
        REQUIRE(SecondCoreOptionsFromMenu(group) == chosen);
        // The second core's environment carries them beside the preset's, once each
        Env own = EnvFor(switches);
        const auto options = SecondCoreOptionEnv(chosen);
        own.insert(own.end(), options.begin(), options.end());
        for (const auto& var : OptionEnv(0, 0, 1)) {
            REQUIRE(std::count(own.begin(), own.end(), var) == 1);
        }
    }
}

TEST_CASE("Anka route: anka-cli reads the options the menu saved", "[yuzu][anka_route]") {
    // No file, no group: the defaults
    REQUIRE(SecondCoreOptionsFromMenu("") == SecondCoreDefaults());
    REQUIRE(SecondCoreOptionsFromMenu(QtConfig) == SecondCoreDefaults());
    // Every combination, as QtConfig writes it
    for (const int a : {0, 1}) {
        for (const int b : {0, 1}) {
            for (const int c : {0, 1}) {
                const auto values = Options(a, b, c);
                REQUIRE(SecondCoreOptionsFromMenu(std::string{QtConfig} + "\n" +
                                                  MenuAnkaGroup(values)) == values);
            }
        }
    }
    // As Config::ReadBooleanSetting: `\default=true` is the default whatever the value says; no
    // `\default` line reads the value
    REQUIRE(SecondCoreOptionsFromMenu("[Anka]\n"
                                      "second_core_max_resolution\\default=true\n"
                                      "second_core_max_resolution=false\n") ==
            SecondCoreDefaults());
    REQUIRE(SecondCoreOptionsFromMenu("[Anka]\nsecond_core_max_resolution=false\n") ==
            Options(0, 1, 0));
    // As SimpleIni reads it: CRLF, spaces, any case, 1 / on / yes / off, the last line of a key
    REQUIRE(SecondCoreOptionsFromMenu("[anka]\r\n"
                                      "Second_Core_Occlusion_Off = 1\r\n"
                                      "second_core_visual_tweak=on\r\n"
                                      "second_core_visual_tweak=yes\r\n"
                                      "second_core_visual_tweak=off\r\n") == Options(1, 0, 1));
    // Not a boolean: the default
    REQUIRE(SecondCoreOptionsFromMenu("[Anka]\nsecond_core_occlusion_off=maybe\n") ==
            SecondCoreDefaults());
    // Only the [Anka] group counts
    REQUIRE(SecondCoreOptionsFromMenu("[Renderer]\nsecond_core_occlusion_off=true\n") ==
            SecondCoreDefaults());
}

TEST_CASE("Anka route: options of other kinds follow the same table", "[yuzu][anka_route]") {
    // As later rows may be: the dynamic frame rate floor (a choice of a variable's value), a
    // number in the settings file, and frame generation (a choice writing two keys there)
    static constexpr std::array<OptionChoice, 2> floors{{{10, "10 FPS"}, {20, "20 FPS"}}};
    static constexpr std::array<IniWrite, 2> times_two{{
        {"Renderer", "frame_gen_target_rate", "0"},
        {"Renderer", "frame_gen_multiplier", "2"},
    }};
    static constexpr std::array<IniWrite, 1> dynamic{{{"Renderer", "frame_gen_target_rate", "120"}}};
    static constexpr std::array<OptionChoice, 2> frame_gen{{
        {0, "2x (recommended)", times_two},
        {1, "Dynamic", dynamic},
    }};
    constexpr std::array table{
        SecondCoreOption{.key = "second_core_dynfps_floor",
                         .kind = OptionKind::Choice,
                         .default_value = 10,
                         .target = OptionTarget::Env,
                         .name = "EDEN_OOT_DYNFPS_MIN_FPS",
                         .label = "Floor",
                         .text = "x",
                         .choices = floors},
        SecondCoreOption{.key = "second_core_count",
                         .kind = OptionKind::Number,
                         .default_value = 2,
                         .target = OptionTarget::Ini,
                         .name = "count",
                         .section = "Renderer",
                         .label = "Count",
                         .text = "x",
                         .min = 1,
                         .max = 4},
        SecondCoreOption{.key = "second_core_frame_gen",
                         .kind = OptionKind::Choice,
                         .default_value = 0,
                         .target = OptionTarget::IniWrites,
                         .label = "Frame generation",
                         .text = "x",
                         .choices = frame_gen},
    };
    using Values = std::array<int, 3>;
    REQUIRE(OptionDefaults(table) == Values{10, 2, 0});
    REQUIRE(ValidOption(table[0], 20) == 20);
    REQUIRE(ValidOption(table[0], 15) == 10);
    REQUIRE(ValidOption(table[1], 9) == 4);
    REQUIRE(ValidOption(table[1], -1) == 1);
    REQUIRE(ValidOption(table[2], 5) == 0);
    REQUIRE(SecondCoreOptionOutputs(Values{20, 3, 0}, table) ==
            std::vector<OptionOutput>{
                {OptionTarget::Env, "", "EDEN_OOT_DYNFPS_MIN_FPS", "20"},
                {OptionTarget::Ini, "Renderer", "count", "3"},
                {OptionTarget::Ini, "Renderer", "frame_gen_target_rate", "0"},
                {OptionTarget::Ini, "Renderer", "frame_gen_multiplier", "2"}});
    REQUIRE(SecondCoreOptionEnv(Values{15, 9, 1}, table) ==
            Env{{"EDEN_OOT_DYNFPS_MIN_FPS", "10"}});
    // Saved and read back; a number without `\default=false` is its default (ReadIntegerSetting)
    REQUIRE(SecondCoreOptionsFromMenu(MenuAnkaGroup(Values{20, 3, 1}, table), table) ==
            Values{20, 3, 1});
    REQUIRE(SecondCoreOptionsFromMenu("[Anka]\nsecond_core_count=3\n", table) == Values{10, 2, 0});
    REQUIRE(SecondCoreOptionsFromMenu("[Anka]\nsecond_core_count\\default=false\n"
                                      "second_core_count=x\n",
                                      table) == Values{10, 2, 0});
    REQUIRE(SecondCoreOptionsFromMenu("[Anka]\nsecond_core_dynfps_floor\\default=false\n"
                                      "second_core_dynfps_floor=15\n",
                                      table) == Values{10, 2, 0});
    // The frame generation choice wins over the menu's frame_gen_* (QtConfig has 2x at 120), with
    // `\default=false`, in [Renderer], each key once; frame_gen itself stays the menu's
    const auto count_of = [](const std::string& ini, std::string_view key) {
        std::size_t n = 0;
        for (auto at = ini.find(key); at != std::string::npos; at = ini.find(key, at + 1)) {
            ++n;
        }
        return n;
    };
    IniState state = State(true);
    state.options = {10, 2, 0};
    auto ini = MakeSecondCoreIni(QtConfig, state, table);
    REQUIRE(Has(ini, "frame_gen_target_rate\\default=false"));
    REQUIRE(Has(ini, "frame_gen_target_rate=0"));
    REQUIRE(Has(ini, "frame_gen_multiplier\\default=false"));
    REQUIRE(Has(ini, "frame_gen_multiplier=2"));
    REQUIRE(count_of(ini, "frame_gen_target_rate") == 2);
    REQUIRE(count_of(ini, "frame_gen_multiplier") == 2);
    REQUIRE_FALSE(Has(ini, "frame_gen_target_rate=120"));
    REQUIRE(Has(ini, "frame_gen=true"));
    REQUIRE(Has(ini, "count=2"));
    const auto section = ini.find("[Renderer]");
    REQUIRE(ini.find("frame_gen_target_rate=0") > section);
    REQUIRE(ini.find("\n[", section + 1) > ini.find("frame_gen_target_rate=0"));
    REQUIRE(ini.find("EDEN_OOT") == std::string::npos);
    // Dynamic writes only its own key: the multiplier stays the menu's
    state.options = {10, 2, 1};
    ini = MakeSecondCoreIni(QtConfig, state, table);
    REQUIRE(Has(ini, "frame_gen_target_rate=120"));
    REQUIRE(Has(ini, "frame_gen_target_rate\\default=false"));
    REQUIRE(Has(ini, "frame_gen_multiplier=2"));
    REQUIRE(count_of(ini, "frame_gen_target_rate") == 2);
}
