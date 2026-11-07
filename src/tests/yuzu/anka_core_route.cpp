// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "yuzu_cmd/anka_core_route.h"

namespace {

namespace fs = std::filesystem;
using namespace AnkaCoreRoute;

constexpr std::uint64_t Bananza = 0x040074A01BF12000ULL;

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("anka_core_route_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path / "zelda");
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

// The parts of a qt-config.ini the Zelda core's settings change
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

TEST_CASE("Anka route: only Ocarina of Time goes to the Zelda core", "[yuzu][anka_route]") {
    REQUIRE(IsZelda(0x0400486024DEC000ULL));
    REQUIRE(IsZelda(0x0400486024DEC800ULL)); // its update
    REQUIRE_FALSE(IsZelda(Bananza));
    REQUIRE_FALSE(IsZelda(0));
    REQUIRE_FALSE(IsZelda(0x0400486024DED000ULL));
}

TEST_CASE("Anka route: the Zelda core in the package", "[yuzu][anka_route]") {
    TempDir t;
    REQUIRE_FALSE(FindZeldaCore(t.path, true).has_value());
    REQUIRE_FALSE(FindZeldaCore(t.path, false).has_value());
    Touch(t.path / "zelda" / "anka-zelda-cli.exe");
    REQUIRE(FindZeldaCore(t.path, true) == t.path / "zelda" / "anka-zelda-cli.exe");
    REQUIRE(FindZeldaCore(t.path, false) == t.path / "zelda" / "anka-zelda-cli.exe");
    Touch(t.path / "zelda" / "anka-zelda.exe");
    REQUIRE(FindZeldaCore(t.path, true) == t.path / "zelda" / "anka-zelda.exe");
    REQUIRE(FindZeldaCore(t.path, false) == t.path / "zelda" / "anka-zelda-cli.exe");
}

TEST_CASE("Anka route: the Zelda core's command line and environment", "[yuzu][anka_route]") {
    const fs::path core = fs::path{"D:/Anka"} / "zelda" / "anka-zelda-cli.exe";
    const fs::path user = fs::path{"D:/Anka"} / "user";
    const auto ini = ZeldaIniPath(user).make_preferred().u8string();
    const std::string ini_utf8(ini.begin(), ini.end());

    const auto windowed = PlanZelda(core, user, "D:/Games/Zelda.nup", false, false);
    REQUIRE(windowed.program == fs::path{core}.make_preferred());
    REQUIRE(windowed.working_dir == fs::path{core}.make_preferred().parent_path());
    REQUIRE(windowed.args == std::vector<std::string>{"-c", ini_utf8, "-g", "D:/Games/Zelda.nup"});
    REQUIRE(windowed.env.size() == 1);
    REQUIRE(windowed.env[0].first == "ANKA_USER_DIR");

    const auto full = PlanZelda(core, user, "D:/Games/Zelda.nup", true, true);
    REQUIRE(full.args.front() == "-f");
    REQUIRE(full.args.size() == 5);
    REQUIRE(full.env.size() == 2);
    REQUIRE(full.env[1] == std::pair<std::string, std::string>{"EDEN_CLI_RESTART_HANDOFF", "1"});
}

TEST_CASE("Anka route: Windows command line quoting", "[yuzu][anka_route]") {
    REQUIRE(QuoteWindowsArg("-g") == "-g");
    REQUIRE(QuoteWindowsArg("") == "\"\"");
    REQUIRE(QuoteWindowsArg("D:\\My Games\\Zelda.nup") == "\"D:\\My Games\\Zelda.nup\"");
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
    const auto ini = MakeZeldaIni(QtConfig, State(true));
    REQUIRE(Has(ini, "use_docked_mode\\default=false"));
    REQUIRE(Has(ini, "nand_directory\\default=false"));
    REQUIRE(Has(ini, "sdmc_directory\\default=false"));
    REQUIRE(Has(ini, "save_directory\\default=false"));
    REQUIRE_FALSE(Has(ini, "use_docked_mode\\default=true"));
    REQUIRE_FALSE(Has(ini, "nand_directory\\default=true"));
}

TEST_CASE("Anka route: docked or handheld always written", "[yuzu][anka_route]") {
    REQUIRE(Has(MakeZeldaIni(QtConfig, State(true)), "use_docked_mode=1"));
    REQUIRE(Has(MakeZeldaIni(QtConfig, State(false)), "use_docked_mode=0"));
    // A config without a [System] section gets one
    const auto ini = MakeZeldaIni("[Renderer]\nanti_aliasing=0\n", State(false));
    REQUIRE(Has(ini, "[System]"));
    REQUIRE(Has(ini, "use_docked_mode=0"));
}

TEST_CASE("Anka route: unchanged AA, filter and ASTC stay the Zelda core's", "[yuzu][anka_route]") {
    const auto ini = MakeZeldaIni(QtConfig, State(true));
    REQUIRE(Has(ini, "anti_aliasing\\default=true"));
    REQUIRE(Has(ini, "astc_recompression\\default=true"));
    // Changed in the menu: the menu's value
    REQUIRE(Has(ini, "scaling_filter\\default=false"));
    REQUIRE(Has(ini, "scaling_filter=1"));
    // Settings only the Zelda core has are not written
    REQUIRE(ini.find("dk_30fps") == std::string::npos);
    // Controls go over as they are
    REQUIRE(Has(ini, "player_0_button_a=\"engine:sdl,button:1\""));
}

TEST_CASE("Anka route: Ocarina of Time keeps its frame rate patch", "[yuzu][anka_route]") {
    const auto ini = MakeZeldaIni(QtConfig, State(true));
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
    const auto ini = MakeZeldaIni(QtConfig, State(true));
    REQUIRE(Has(ini, "nand_directory=D:/Anka/user/nand"));
    REQUIRE(Has(ini, "sdmc_directory=D:/Anka/user/sdmc"));
    REQUIRE(Has(ini, "save_directory="));
    REQUIRE(ini.find("C:/old/nand") == std::string::npos);
    // A path QSettings quotes
    IniState state = State(true);
    state.data_storage = {{"nand_directory", "D:/a,b/nand"}};
    REQUIRE(Has(MakeZeldaIni(QtConfig, state), "nand_directory=\"D:/a,b/nand\""));
}

TEST_CASE("Anka route: CRLF files stay CRLF and nothing else moves", "[yuzu][anka_route]") {
    const std::string crlf = "[UI]\r\ntheme=dark\r\n\r\n[System]\r\nuse_docked_mode=1\r\n";
    const auto ini = MakeZeldaIni(crlf, IniState{.docked = true, .data_storage = {}});
    REQUIRE(ini.starts_with("[UI]\r\ntheme=dark\r\n\r\n[System]\r\n"));
    REQUIRE(ini.find("\r\nuse_docked_mode\\default=false\r\nuse_docked_mode=1\r\n") !=
            std::string::npos);
    REQUIRE(ini.find("\n\n") == std::string::npos);
}
