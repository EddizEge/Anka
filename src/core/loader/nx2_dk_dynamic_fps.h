// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

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

struct Result {
    bool applied{};
    size_t controller{};
    size_t sleep{};
    size_t step{};
    size_t controller_matches{};
    size_t sleep_matches{};
    size_t step_matches{};
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
inline Result Apply(std::span<u8> text, u32 text_size, u32 aligned_size) {
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
    size_t controller = 0, controller_matches = 0, sleep = 0, sleep_matches = 0;
    size_t step = 0, step_matches = 0;
    for (size_t i = 1; i + 8 < count; ++i) {
        if (word(i) == 0xBD405100 && word(i + 1) == 0x52A84E08 && word(i + 2) == 0x1E270101 &&
            word(i + 6) == 0x5E21D800 && word(i + 7) == 0x1E201820 && word(i + 8) == 0xBD000100) {
            step = i; // ldr s0, [x8, #0x50]; ...; scvtf s0, s0; fdiv s0, s1, s0; str s0, [x8]
            ++step_matches;
        }
        if (word(i) == 0xF9409429 && word(i + 1) == 0x7100015F && word(i + 2) == 0xFA400924) {
            controller = i; // ldr x9, [x1, #0x128]; cmp w10, #0; ccmp x9, #0, #4, eq
            ++controller_matches;
        }
        if (word(i) == 0xEB080120 && word(i - 1) == 0x9E380009 &&
            (word(i + 1) & 0xFF00001F) == 0x5400000D && (word(i + 2) >> 26) == 0x25) {
            sleep = i; // fcvtzs x9, s0; subs x0, x9, x8; b.le; bl
            ++sleep_matches;
        }
    }
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
    return result;
}

/// The loader's entry: nullopt (the module untouched) unless EDEN_NX2_DK_DYN is on and this is
/// Bananza's main module.
inline std::optional<Result> ApplyIfEnabled(const char* switch_value, u64 program_id,
                                            std::string_view module_name, std::span<u8> text,
                                            u32 text_size, u32 aligned_size) {
    if (!Enabled(switch_value) || module_name != "main" || program_id != ProgramId) {
        return std::nullopt;
    }
    return Apply(text, text_size, aligned_size);
}

} // namespace Loader::Nx2DkDynamicFps
