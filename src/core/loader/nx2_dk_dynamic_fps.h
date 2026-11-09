// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"

// EDEN_NX2_DK_DYN=1 (off by default): Donkey Kong Bananza's dynamic frame rate patch, taken from
// the ZendNevs fork (v2.0.5, nso.cpp, setting dk_dynamic_fps, on by default there). It rewrites the
// game's main module as it loads; unset or any other value leaves the module untouched. Applied,
// it sets Core::g_nx2_dk_dyn_applied, which nx2_gsv.h reads: vi2 107 then reports VRR support and
// presents are paced to every vblank (EDEN_NX2_MODE_PACE and the present interval word set aside).
namespace Loader::Nx2DkDynamicFps {

constexpr u64 ProgramId = 0x040074A01BF12000ULL;

/// EDEN_NX2_DK_DYN's value: only "1..." turns the patch on.
constexpr bool Enabled(const char* value) {
    return value != nullptr && value[0] == '1';
}

/// Words of free space the patch needs past the end of the text segment.
constexpr size_t CaveWords = 19;

// EDEN_NX2_DK_DYN_MIN_FPS=<n> (unset: the game's 40): the controller drops to a whole interval
// when its new target is below 40 fps (patched, always interval 2 in the logs: word 12 0x2003c,
// never 0x3003c; its free targets come in steps of 2, 0x10028-0x1003c):
//   controller + 59: movz w14, #40
//   controller + 60: ccmp w1, w14, #0, eq   ; w1 the new target, eq: [fw+0x1cb] clear
//   controller + 61: movz w14, #5
//   controller + 62: csel w14, w14, wzr, lt
// With the switch, 40 becomes n: targets n-40 run free like those above 40, sleeping to the
// target instead of to 30 fps; below n the whole interval as before. Both step the clock by the
// frame up to 2.5 vblanks (24 fps), so the game's speed is the same; what changes is a frame
// rate of 30-40 no longer held at 30.
constexpr size_t LowTargetOffset = 59;
constexpr u32 LowTargetWord = 0x5280050EU;  // movz w14, #40
constexpr u32 LowTargetCheck = 0x7A4E0020U; // ccmp w1, w14, #0, eq
constexpr u32 MinFpsLowest = 1;
constexpr u32 MinFpsHighest = 60;

/// `movz w14, #fps`.
constexpr u32 LowTargetMovz(u32 fps) {
    return 0x5280000EU | (fps << 5);
}

struct MinFps {
    u32 fps{};      // 0: unset or refused, the game's 40
    bool refused{}; // set but not a whole number of 1-60
};

/// EDEN_NX2_DK_DYN_MIN_FPS's value: a whole number of 1-60, nothing else around it.
constexpr MinFps ParseMinFps(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return {};
    }
    u32 fps = 0;
    size_t i = 0;
    for (; value[i] != '\0'; ++i) {
        if (value[i] < '0' || value[i] > '9' || i >= 3) {
            return {.refused = true};
        }
        fps = fps * 10 + static_cast<u32>(value[i] - '0');
    }
    if (fps < MinFpsLowest || fps > MinFpsHighest) {
        return {.refused = true};
    }
    return {.fps = fps};
}

/// Where the patch's three code sequences are, in words from the text segment's start (each
/// the last match), and how many times each was found; Apply needs one of each.
struct Sites {
    size_t controller{};
    size_t sleep{};
    size_t step{};
    size_t controller_matches{};
    size_t sleep_matches{};
    size_t step_matches{};
};

inline Sites FindSites(std::span<const u8> text, u32 text_size) {
    Sites sites{};
    if (text.size() < text_size) {
        return sites;
    }
    const u8* const code = text.data();
    const size_t count = text_size / sizeof(u32);
    const auto word = [code](size_t index) {
        u32 value;
        std::memcpy(&value, code + index * sizeof(u32), sizeof(value));
        return value;
    };
    for (size_t i = 1; i + 8 < count; ++i) {
        if (word(i) == 0xBD405100 && word(i + 1) == 0x52A84E08 && word(i + 2) == 0x1E270101 &&
            word(i + 6) == 0x5E21D800 && word(i + 7) == 0x1E201820 && word(i + 8) == 0xBD000100) {
            sites.step = i; // ldr s0, [x8, #0x50]; ...; scvtf s0, s0; fdiv s0, s1, s0; str s0, [x8]
            ++sites.step_matches;
        }
        if (word(i) == 0xF9409429 && word(i + 1) == 0x7100015F && word(i + 2) == 0xFA400924) {
            sites.controller = i; // ldr x9, [x1, #0x128]; cmp w10, #0; ccmp x9, #0, #4, eq
            ++sites.controller_matches;
        }
        if (word(i) == 0xEB080120 && word(i - 1) == 0x9E380009 &&
            (word(i + 1) & 0xFF00001F) == 0x5400000D && (word(i + 2) >> 26) == 0x25) {
            sites.sleep = i; // fcvtzs x9, s0; subs x0, x9, x8; b.le; bl
            ++sites.sleep_matches;
        }
    }
    return sites;
}

struct Result {
    bool applied{};
    size_t controller{};
    size_t sleep{};
    size_t step{};
    size_t controller_matches{};
    size_t sleep_matches{};
    size_t step_matches{};
    u32 min_fps{};       // EDEN_NX2_DK_DYN_MIN_FPS put in (0: the game's 40 kept)
    bool min_fps_missed{}; // asked for, but controller + 59 / + 60 were not the words above
};

// Donkey Kong Bananza picks its frame rate target from the time of a frame: above 40 fps it
// sleeps to the target and advances its clock by 60/target, below 40 it uses a whole interval.
// That choice reads [fw+0x128], the CPU's calc time, which is short under emulation where the
// GPU thread is the slow part: it stayed at 60 and the game ran in slow motion below that.
// Point it at [fw+0xe8], the whole frame, and take the game's own sleep out of that time
// (start of frame + sleep), or a lowered target could never rise again. The sleep's check moves
// to free space in the text segment's last page.
// Below 40 fps the game ran with no sleep, so a frame shorter than its interval sped it up and
// a longer one slowed it down: sleep there too, to 60/interval.
// The clock's step was still 60/target, so a frame longer than its target ran in slow motion.
// With the sleep, a frame lasts the longer of its target and its work: step by that, up to 2.5
// (24 fps), since 4 fell into a 3 fps loop: a long step makes a long frame, so a long step.
// The controller, which has the frame time in 60ths, keeps it in an unused field.
//
// `text` starts at the text segment and holds at least `aligned_size` bytes, the segment's size
// rounded up to whole pages (the free space at its end is the cave); `text_size` is its size in
// the NSO header. Changes nothing unless all three patterns match once and the checks hold.
// `min_fps` (1-60, ParseMinFps; 0: none) replaces the 40 at controller + 59 when the patch
// applies and the two words there are as expected; otherwise that word is left alone.
inline Result Apply(std::span<u8> text, u32 text_size, u32 aligned_size, u32 min_fps = 0) {
    Result result{};
    if (text.size() < aligned_size || aligned_size < text_size) {
        return result;
    }
    u8* const code = text.data();
    const size_t count = text_size / sizeof(u32);
    const auto word = [code](size_t index) {
        u32 value;
        std::memcpy(&value, code + index * sizeof(u32), sizeof(value));
        return value;
    };
    const auto put = [code](size_t index, u32 value) {
        std::memcpy(code + index * sizeof(u32), &value, sizeof(value));
    };
    const auto branch = [](size_t from, size_t to) {
        return 0x14000000U |
               (static_cast<u32>(static_cast<s64>(to) - static_cast<s64>(from)) & 0x3FFFFFFU);
    };
    const Sites sites = FindSites(text, text_size);
    const size_t controller = sites.controller, controller_matches = sites.controller_matches;
    const size_t sleep = sites.sleep, sleep_matches = sites.sleep_matches;
    const size_t step = sites.step, step_matches = sites.step_matches;
    result.controller = controller;
    result.sleep = sleep;
    result.step = step;
    result.controller_matches = controller_matches;
    result.sleep_matches = sleep_matches;
    result.step_matches = step_matches;
    // The two checks sit at fixed distances from the matches above. (Their words are read only
    // inside the text segment: ZendNevs reads them unchecked.)
    const size_t low_check = sleep - 14, frame_time = controller + 14;
    const bool extras_found = sleep_matches == 1 && controller_matches == 1 && step_matches == 1 &&
                              sleep >= 15 && frame_time < count &&
                              word(low_check - 1) == 0x39472e68U && word(low_check) == 0x350006a8U &&
                              word(frame_time) == 0x2944266dU;
    const size_t cave = (text_size + 15) / 16 * 4;
    if (!extras_found || (cave + CaveWords) * sizeof(u32) > aligned_size) {
        return result;
    }
    const size_t no_sleep = sleep + 1 + ((word(sleep + 1) >> 5) & 0x7FFFF);
    put(cave, 0xEB080120);                         // subs x0, x9, x8
    put(cave + 1, 0x5400004C);                     // b.gt +8
    put(cave + 2, branch(cave + 2, no_sleep));     // b (the old b.le's target)
    put(cave + 3, 0xF9407A6A);                     // ldr x10, [x19, #0xf0]
    put(cave + 4, 0x8B00014A);                     // add x10, x10, x0
    put(cave + 5, 0xF9007A6A);                     // str x10, [x19, #0xf0]
    put(cave + 6, branch(cave + 6, sleep + 2));    // back to the sleep
    put(sleep, branch(sleep, cave));
    put(controller, 0xF9407429);                   // ldr x9, [x1, #0xe8]
    put(low_check, 0xD503201F);                    // nop: sleep below 40 fps too
    put(cave + 7, 0xBD005E60);                     // str s0, [x19, #0x5c]: frame in 60ths
    put(cave + 8, word(frame_time));
    put(cave + 9, branch(cave + 9, frame_time + 1));
    put(frame_time, branch(frame_time, cave + 7));
    put(cave + 10, 0xBD405D02);                    // ldr s2, [x8, #0x5c]
    put(cave + 11, word(step + 1));                // mov w8, #60.0
    put(cave + 12, word(step + 2));                // fmov s1, w8
    put(cave + 13, word(step + 6));                // scvtf s0, s0
    put(cave + 14, word(step + 7));                // fdiv s0, s1, s0
    put(cave + 15, 0x1E226800);                    // fmaxnm s0, s0, s2
    put(cave + 16, 0x1E209001);                    // fmov s1, #2.5
    put(cave + 17, 0x1E217800);                    // fminnm s0, s0, s1
    put(cave + 18, branch(cave + 18, step + 3));
    put(step + 1, branch(step + 1, cave + 10));
    put(step + 6, 0xD503201F);                     // nop, done in the cave
    put(step + 7, 0xD503201F);                     // nop, done in the cave
    result.applied = true;
    if (min_fps >= MinFpsLowest && min_fps <= MinFpsHighest) {
        const size_t low_target = controller + LowTargetOffset;
        if (low_target + 1 < count && word(low_target) == LowTargetWord &&
            word(low_target + 1) == LowTargetCheck) {
            put(low_target, LowTargetMovz(min_fps));
            result.min_fps = min_fps;
        } else {
            result.min_fps_missed = true;
        }
    }
    return result;
}

// Another exefs mod on the same module (a "60 FPS" IPS / IPSwitch patch) can change the frame
// rate code too: both at once ran the game twice as fast above 30 fps (a user's report, X,
// 07.11.2026). The code the patch rewrites or depends on, with a margin of the functions around
// it, and the controller's present interval sites (the 30 FPS lock's, nso.cpp): a mod changing
// any of it turns the patch off for that session. A mod elsewhere is only named in the log.
struct Zone {
    size_t begin{}; ///< bytes from the text segment's start
    size_t end{};
    std::string_view what;
};

constexpr size_t ZoneMarginWords = 32;

inline std::vector<Zone> FpsZones(std::span<const u8> text, u32 text_size) {
    std::vector<Zone> zones;
    if (text.size() < text_size) {
        return zones;
    }
    const size_t count = text_size / sizeof(u32);
    const auto add = [&zones, count](size_t first, size_t last, std::string_view what) {
        first = first > ZoneMarginWords ? first - ZoneMarginWords : 0;
        last = std::min(last + ZoneMarginWords, count);
        zones.push_back({first * sizeof(u32), last * sizeof(u32), what});
    };
    const Sites sites = FindSites(text, text_size);
    if (sites.controller_matches == 1) {
        add(sites.controller, sites.controller + LowTargetOffset + 2, "frame rate controller");
    }
    if (sites.sleep_matches == 1) {
        add(sites.sleep >= 14 ? sites.sleep - 14 : 0, sites.sleep + 3, "frame sleep");
    }
    if (sites.step_matches == 1) {
        add(sites.step, sites.step + 9, "game clock step");
    }
    // The free space Apply writes to (past the end of the code, in its last page)
    const size_t cave = (text_size + 15) / 16 * 4;
    zones.push_back({cave * sizeof(u32), (cave + CaveWords) * sizeof(u32), "free space it uses"});

    const u8* const code = text.data();
    const auto word = [code](size_t index) {
        u32 value;
        std::memcpy(&value, code + index * sizeof(u32), sizeof(value));
        return value;
    };
    // SetLayerExtendedSwapInterval(layer, 1, fps) calls and the vblank divisor (nso.cpp)
    for (size_t i = 0; i + 4 < count; ++i) {
        if ((word(i + 3) & 0xFC000000) == 0x94000000 && word(i + 1) == 0x52800021 &&
            ((word(i) == 0xF940C260 && word(i + 2) == 0xB940E262) ||
             (word(i) == 0xF940C000 && word(i + 2) == 0x2A1303E2))) {
            add(i, i + 4, "present interval");
        }
        if (word(i) == 0x52800788 && word(i + 1) == 0xB9402689 && word(i + 2) == 0xB9004295 &&
            word(i + 3) == 0x1AD50D08 && word(i + 4) == 0x2A0803E1) {
            add(i, i + 5, "present interval");
        }
    }
    return zones;
}

// A "60 FPS" mod need not touch the zones above: one changing the frame rate somewhere else ran
// the game twice as fast above 30 fps all the same, and only the log said so. What
// EDEN_NX2_DK_DYN_MOD_GUARD does with an exefs mod changing main outside the zones (only the
// mod's folder and file names are looked at, never what it changes):
//   unset / anything else: All, any such mod turns the patch off for that boot;
//   "name":  Names, only a mod whose folder or file name reads like a frame rate mod (FpsLikeName);
//   "0":     Zones, Anka 1.1.1's rule: only a mod in the zones, the others named in the log.
enum class ModGuard { All, Names, Zones };

constexpr ModGuard ParseModGuard(const char* value) {
    if (value == nullptr) {
        return ModGuard::All;
    }
    const std::string_view text{value};
    if (text == "0") {
        return ModGuard::Zones;
    }
    if (text == "name" || text == "names") {
        return ModGuard::Names;
    }
    return ModGuard::All;
}

/// A mod folder / patch file name that reads like a frame rate mod: "fps", "frame", "vsync",
/// "hz" or "60", any case ("60 FPS", "Framerate unlock", "dkb_60hz.pchtxt").
inline bool FpsLikeName(std::string_view name) {
    std::string lower(name);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    for (const std::string_view word : {"fps", "frame", "vsync", "hz", "60"}) {
        if (lower.find(word) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// A patch file named by the module's build id ("<hex>.ips" / "<hex>.pchtxt"): its name says
/// nothing (and "60" can turn up in any hex).
constexpr bool BuildIdName(std::string_view file) {
    const auto dot = file.rfind('.');
    const std::string_view stem = file.substr(0, dot);
    if (stem.empty()) {
        return false;
    }
    for (const char c : stem) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

/// Whether a mod changing main outside the zones turns the patch off under `guard`.
inline bool BlocksOutsideZones(ModGuard guard, std::string_view mod, std::string_view file) {
    switch (guard) {
    case ModGuard::All:
        return true;
    case ModGuard::Names:
        return FpsLikeName(mod) || (!BuildIdName(file) && FpsLikeName(file));
    case ModGuard::Zones:
        break;
    }
    return false;
}

// EDEN_NX2_DK_DYN_MIN_FPS below 40 (Anka 1.1.2): the game's free targets stop at 40. In the
// 05.11 runs with 30 (four runs, ~44k presents, EDEN_NX2_PRESENT_DIAG) word 12 never went below
// 0x10028 (interval 1, 40 fps) yet the whole interval (0x2003c) came 2-5 times as often, and the
// game ran 4-5% slower (and 32.1 against 37.0 fps in a user's scene, 08.11): the 30-40 band it
// was meant to open does not exist, the frames there only got longer. So a value below 40 keeps
// the game's 40 unless EDEN_NX2_DK_DYN_MIN_FPS_LOW=1 (the old behaviour, for tests); 40-60 go in.
constexpr u32 MinFpsFree = 40;

struct MinFpsChoice {
    u32 fps{};          // what goes to Apply (0: the game's 40)
    bool held_low{};    // a value below 40 kept out
};

constexpr MinFpsChoice ChooseMinFps(MinFps parsed, const char* low_switch) {
    if (parsed.fps != 0 && parsed.fps < MinFpsFree &&
        !(low_switch != nullptr && low_switch[0] == '1')) {
        return {.fps = 0, .held_low = true};
    }
    return {.fps = parsed.fps};
}

struct Conflict {
    size_t offset{}; ///< the first changed byte inside a zone, in module code offsets
    std::string_view what;
};

/// The first byte of `changes` ([begin, end) in module code offsets, PatchManager's
/// NsoModChange) inside one of `zones`; the text segment starts at `text_location` there.
inline std::optional<Conflict> FirstConflict(const std::vector<Zone>& zones, size_t text_location,
                                             const std::vector<std::pair<size_t, size_t>>& changes) {
    std::optional<Conflict> first;
    for (const Zone& zone : zones) {
        const size_t zone_begin = text_location + zone.begin;
        const size_t zone_end = text_location + zone.end;
        for (const auto& [begin, end] : changes) {
            if (begin < zone_end && zone_begin < end) {
                const size_t offset = std::max(begin, zone_begin);
                if (!first || offset < first->offset) {
                    first = Conflict{offset, zone.what};
                }
            }
        }
    }
    return first;
}

/// The loader's entry: nullopt (the module untouched) unless EDEN_NX2_DK_DYN is on and this is
/// Bananza's main module.
inline std::optional<Result> ApplyIfEnabled(const char* switch_value, u64 program_id,
                                            std::string_view module_name, std::span<u8> text,
                                            u32 text_size, u32 aligned_size, u32 min_fps = 0) {
    if (!Enabled(switch_value) || module_name != "main" || program_id != ProgramId) {
        return std::nullopt;
    }
    return Apply(text, text_size, aligned_size, min_fps);
}

} // namespace Loader::Nx2DkDynamicFps
