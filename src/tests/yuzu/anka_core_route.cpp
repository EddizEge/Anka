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

TEST_CASE("Anka route: what the user reads names no game", "[yuzu][anka_route]") {
    const std::array texts{Text::Starting,      Text::Closed,         Text::StillRunning,
                           Text::StopRunningFirst, Text::PropertiesNote, Text::Missing,
                           Text::IniNotWritten, Text::NotStarted,     Text::StartFailed,
                           Text::AlreadyRunning, Text::Crashed, Text::NoEffectOnSecondCore,
                           Text::PresetOnSecondCore};
    for (const std::string_view text : texts) {
        REQUIRE(text.find("Zelda") == std::string_view::npos);
        REQUIRE(text.find("zelda") == std::string_view::npos);
        REQUIRE(text.find("Ocarina") == std::string_view::npos);
    }
    REQUIRE(std::string_view{Text::Starting} == "Starting this game on Anka's second core");
    REQUIRE(std::string_view{Text::StillRunning}.starts_with(
        "Another game is still running on Anka's second core"));
    REQUIRE(std::string_view{Text::PropertiesNote} ==
            "These settings have no effect on this game's core.");
    REQUIRE(std::string_view{Text::NoEffectOnSecondCore} == "Anka's second core: no effect");
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
    // A file without [Renderer] gets one
    const auto ini = MakeSecondCoreIni("[System]\nuse_docked_mode=1\n", State(true));
    REQUIRE(Has(ini, "[Renderer]"));
    REQUIRE(Has(ini, "use_reactive_flushing=false"));
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
