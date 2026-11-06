// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/nx2_perf_settings.h"

namespace {

using namespace Core::Nx2PerfSettings;

constexpr u64 Bananza = 0x040074A01BF12000ULL;
constexpr u64 OtherGame = 0x0100000000010000ULL;

// start-recommended.bat's lines (dist/nx2perf), DK_DYN and OSD at their defaults (1).
const std::vector<Var> RecommendedBananza{
    {"EDEN_NX2_BACK_STAGES", "1"},
    {"EDEN_NX2_BACK_CUTS", "a"},
    {"EDEN_NX2_ACQUIRE_PEEK", "1"},
    {"EDEN_NX2_JIT_SAME", "1"},
    {"EDEN_NX2_FRONT_MACRO_LEAN", "1"},
    {"EDEN_NX2_FG_TIMED", "1"},
    {"EDEN_NX2_OSD", "1"},
    {"EDEN_NX2_DK_DYN", "1"},
    {"EDEN_NX2_PRESENT_INTERVAL_WORD", "12:16:4"},
    {"EDEN_NX2_MODE_PACE", "2"},
    {"EDEN_NX2_PACE_SPIN_US", "1500"},
};

const std::set<std::string_view> BananzaOnly{
    "EDEN_NX2_DK_DYN",
    "EDEN_NX2_PRESENT_INTERVAL_WORD",
    "EDEN_NX2_MODE_PACE",
    "EDEN_NX2_PACE_SPIN_US",
};

// An ini group as the config sees it: "key" and "key\default", both strings.
struct Ini {
    std::map<std::string, bool> values;

    Switches Read() const {
        return Load([this](std::string_view key, bool def) {
            const auto it = values.find(std::string{key});
            return it == values.end() ? def : it->second;
        });
    }
    void Write(const Switches& switches) {
        Save(switches, [this](std::string_view key, bool value, bool) {
            values[std::string{key}] = value;
        });
    }
};

bool NoneOutside(std::string_view) {
    return false;
}

} // Anonymous namespace

TEST_CASE("Nx2PerfSettings: the first start is the recommended setup", "[core]") {
    const Ini empty;
    REQUIRE(empty.Read() == Recommended());
    REQUIRE(Wanted(Recommended(), true) == RecommendedBananza);
}

TEST_CASE("Nx2PerfSettings: every setting reads back what was written", "[core]") {
    for (const Key& key : Keys) {
        Switches switches = Recommended();
        switches.*key.field = false;
        Ini ini;
        ini.Write(switches);
        REQUIRE(ini.values.size() == Keys.size());
        REQUIRE(ini.Read() == switches);
        REQUIRE(ini.values.at(std::string{key.name}) == false);
    }
    Ini ini;
    ini.Write(AllOff());
    REQUIRE(ini.Read() == AllOff());
    ini.Write(Recommended());
    REQUIRE(ini.Read() == Recommended());
}

TEST_CASE("Nx2PerfSettings: keys are distinct and cover every field", "[core]") {
    std::set<std::string_view> names;
    for (const Key& key : Keys) {
        names.insert(key.name);
        Switches one = AllOff();
        one.*key.field = true;
        REQUIRE(one != AllOff());
    }
    REQUIRE(names.size() == Keys.size());
    // AllOff really is all off: no key left on.
    for (const Key& key : Keys) {
        REQUIRE(!(AllOff().*key.field));
        REQUIRE(Recommended().*key.field);
    }
}

TEST_CASE("Nx2PerfSettings: Bananza by the patch's own program id", "[core]") {
    REQUIRE(IsBananza(Loader::Nx2DkDynamicFps::ProgramId));
    REQUIRE(IsBananza(Bananza));
    REQUIRE(!IsBananza(OtherGame));
    REQUIRE(!IsBananza(0));
    REQUIRE(!IsBananza(Bananza + 0x800)); // its update's id: the patch checks the base id only
}

TEST_CASE("Nx2PerfSettings: the Bananza settings never reach another game", "[core]") {
    const auto other = Wanted(Recommended(), IsBananza(OtherGame));
    for (const Var& var : other) {
        REQUIRE(BananzaOnly.count(var.name) == 0);
    }
    REQUIRE(other.size() == RecommendedBananza.size() - 4);
    // Bananza's own switches off: the others as for any game.
    Switches no_dk = Recommended();
    no_dk.dk_dyn = false;
    no_dk.dk_pacing = false;
    REQUIRE(Wanted(no_dk, true) == other);
    // One of the two alone.
    Switches dyn_only = no_dk;
    dyn_only.dk_dyn = true;
    const auto dyn = Wanted(dyn_only, true);
    REQUIRE(dyn.size() == other.size() + 1);
    REQUIRE(dyn.back() == Var{"EDEN_NX2_DK_DYN", "1"});
    Switches pacing_only = no_dk;
    pacing_only.dk_pacing = true;
    const auto pacing = Wanted(pacing_only, true);
    REQUIRE(pacing.size() == other.size() + 3);
    REQUIRE(pacing.back() == Var{"EDEN_NX2_PACE_SPIN_US", "1500"});
}

TEST_CASE("Nx2PerfSettings: all off sets nothing", "[core]") {
    REQUIRE(Wanted(AllOff(), true).empty());
    REQUIRE(Wanted(AllOff(), false).empty());
    REQUIRE(Plan(AllOff(), true, NoneOutside).empty());
}

TEST_CASE("Nx2PerfSettings: a variable set outside the program wins", "[core]") {
    REQUIRE(Plan(Recommended(), true, NoneOutside) == RecommendedBananza);

    // A launcher (or the user) set two of them: those are left out, whatever their value.
    const std::set<std::string_view> outside{"EDEN_NX2_JIT_SAME", "EDEN_NX2_MODE_PACE"};
    const auto plan = Plan(Recommended(), true,
                           [&](std::string_view name) { return outside.count(name) != 0; });
    REQUIRE(plan.size() == RecommendedBananza.size() - 2);
    for (const Var& var : plan) {
        REQUIRE(outside.count(var.name) == 0);
    }

    // Set outside and turned off here: still theirs (the plan never clears a variable).
    Switches off = Recommended();
    off.jit_same = false;
    const auto plan_off = Plan(off, true, [&](std::string_view name) {
        return outside.count(name) != 0;
    });
    REQUIRE(plan_off == plan);

    // Everything set outside: nothing to set.
    REQUIRE(Plan(Recommended(), true, [](std::string_view) { return true; }).empty());
}

TEST_CASE("Nx2PerfSettings: every name is one Wanted can set", "[core]") {
    std::set<std::string_view> wanted;
    for (const Var& var : Wanted(Recommended(), true)) {
        wanted.insert(var.name);
    }
    REQUIRE(wanted.size() == Names.size());
    for (const std::string_view name : Names) {
        REQUIRE(wanted.count(name) == 1);
    }
}

TEST_CASE("Nx2PerfSettings: a boot that needs other variables differs from the session's", "[core]") {
    // What the session compares (yuzu/nx2perf_session.cpp): the plan of the first boot.
    const auto first_bananza = Plan(Recommended(), IsBananza(Bananza), NoneOutside);
    REQUIRE(Plan(Recommended(), IsBananza(Bananza), NoneOutside) == first_bananza);
    REQUIRE(Plan(Recommended(), IsBananza(OtherGame), NoneOutside) != first_bananza);
    Switches changed = Recommended();
    changed.osd = false;
    REQUIRE(Plan(changed, true, NoneOutside) != first_bananza);
    // Bananza's own switches set outside: another game needs nothing else, no restart.
    const auto outside = [](std::string_view name) { return BananzaOnly.count(name) != 0; };
    REQUIRE(Plan(Recommended(), true, outside) == Plan(Recommended(), false, outside));
}
