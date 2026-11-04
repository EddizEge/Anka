// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/loader/nx2_dk_dynamic_fps.h"

namespace {
using namespace Loader::Nx2DkDynamicFps;

constexpr u32 Filler = 0x12345678;
constexpr u32 Nop = 0xD503201F;
// A text segment of 0x2f04 bytes (0xbc1 words) in 0x3000: the cave starts at word 0xbc4, the
// size rounded up to 16 bytes.
constexpr u32 TextSize = 0x2f04;
constexpr u32 AlignedSize = 0x3000;
constexpr size_t Cave = 0xbc4;
constexpr size_t Controller = 0x200;
constexpr size_t Sleep = 0x400;
constexpr size_t Step = 0x600;
constexpr u32 SleepSkip = 0x10; // the b.le's imm19: no sleep at Sleep + 1 + 0x10 = 0x411

struct Text {
    std::vector<u32> words;

    explicit Text(u32 text_size = TextSize, u32 aligned_size = AlignedSize)
        : words(aligned_size / 4, 0) {
        for (size_t i = 0; i < text_size / 4; ++i) {
            words[i] = Filler;
        }
    }

    std::span<u8> Bytes() {
        return {reinterpret_cast<u8*>(words.data()), words.size() * 4};
    }

    void PutController(size_t at, bool frame_time_check = true) {
        words[at] = 0xF9409429;     // ldr x9, [x1, #0x128]
        words[at + 1] = 0x7100015F; // cmp w10, #0
        words[at + 2] = 0xFA400924; // ccmp x9, #0, #4, eq
        if (frame_time_check) {
            words[at + 14] = 0x2944266d;
        }
    }

    void PutSleep(size_t at, bool low_checks = true) {
        words[at - 1] = 0x9E380009;                    // fcvtzs x9, s0
        words[at] = 0xEB080120;                        // subs x0, x9, x8
        words[at + 1] = 0x5400000D | (SleepSkip << 5); // b.le +0x10 words
        words[at + 2] = 0x94000123;                    // bl
        if (low_checks) {
            words[at - 15] = 0x39472e68;
            words[at - 14] = 0x350006a8;
        }
    }

    void PutStep(size_t at) {
        words[at] = 0xBD405100;     // ldr s0, [x8, #0x50]
        words[at + 1] = 0x52A84E08; // mov w8, #60.0
        words[at + 2] = 0x1E270101; // fmov s1, w8
        words[at + 3] = 0xAAAA0003;
        words[at + 4] = 0xAAAA0004;
        words[at + 5] = 0xAAAA0005;
        words[at + 6] = 0x5E21D800; // scvtf s0, s0
        words[at + 7] = 0x1E201820; // fdiv s0, s1, s0
        words[at + 8] = 0xBD000100; // str s0, [x8]
    }

    void PutAll() {
        PutController(Controller);
        PutSleep(Sleep);
        PutStep(Step);
    }

    // The 40 fps check (EDEN_NX2_DK_DYN_MIN_FPS): movz w14, #40; ccmp w1, w14, #0, eq.
    void PutLowTarget(size_t controller) {
        words[controller + 59] = 0x5280050E;
        words[controller + 60] = 0x7A4E0020;
    }
};

// An unconditional branch's target, decoded on its own (sign-extended imm26).
size_t BranchTarget(size_t from, u32 insn) {
    REQUIRE((insn >> 26) == 0x05);
    s64 offset = insn & 0x3FFFFFF;
    if (offset & 0x2000000) {
        offset -= 0x4000000;
    }
    return static_cast<size_t>(static_cast<s64>(from) + offset);
}

void RequireUntouched(const Result& result, const std::vector<u32>& before, const Text& text) {
    REQUIRE(!result.applied);
    REQUIRE(text.words == before);
}
} // Anonymous namespace

TEST_CASE("Nx2DkDynamicFps: the patch, word by word", "[core]") {
    Text text;
    text.PutAll();
    std::vector<u32> expected = text.words;
    const Result result = Apply(text.Bytes(), TextSize, AlignedSize);
    REQUIRE(result.applied);
    REQUIRE(result.controller == Controller);
    REQUIRE(result.sleep == Sleep);
    REQUIRE(result.step == Step);
    REQUIRE(result.controller_matches == 1);
    REQUIRE(result.sleep_matches == 1);
    REQUIRE(result.step_matches == 1);

    // Hand-computed: B is 0x14000000 | ((to - from) & 0x3ffffff).
    expected[Cave + 0] = 0xEB080120;   // subs x0, x9, x8
    expected[Cave + 1] = 0x5400004C;   // b.gt +8
    expected[Cave + 2] = 0x17FFF84B;   // b 0x411 (from 0xbc6: -0x7b5)
    expected[Cave + 3] = 0xF9407A6A;   // ldr x10, [x19, #0xf0]
    expected[Cave + 4] = 0x8B00014A;   // add x10, x10, x0
    expected[Cave + 5] = 0xF9007A6A;   // str x10, [x19, #0xf0]
    expected[Cave + 6] = 0x17FFF838;   // b 0x402, the sleep's bl (from 0xbca: -0x7c8)
    expected[Sleep] = 0x140007C4;      // b 0xbc4, the cave (+0x7c4)
    expected[Controller] = 0xF9407429; // ldr x9, [x1, #0xe8]
    expected[Sleep - 14] = Nop;        // low_check
    expected[Cave + 7] = 0xBD005E60;   // str s0, [x19, #0x5c]
    expected[Cave + 8] = 0x2944266d;   // the frame time's own word
    expected[Cave + 9] = 0x17FFF642;   // b 0x20f (from 0xbcd: -0x9be)
    expected[Controller + 14] = 0x140009BD; // b 0xbcb, cave + 7 (+0x9bd)
    expected[Cave + 10] = 0xBD405D02;  // ldr s2, [x8, #0x5c]
    expected[Cave + 11] = 0x52A84E08;  // the step's mov w8, #60.0
    expected[Cave + 12] = 0x1E270101;  // fmov s1, w8
    expected[Cave + 13] = 0x5E21D800;  // scvtf s0, s0
    expected[Cave + 14] = 0x1E201820;  // fdiv s0, s1, s0
    expected[Cave + 15] = 0x1E226800;  // fmaxnm s0, s0, s2
    expected[Cave + 16] = 0x1E209001;  // fmov s1, #2.5
    expected[Cave + 17] = 0x1E217800;  // fminnm s0, s0, s1
    expected[Cave + 18] = 0x17FFFA2D;  // b 0x603 (from 0xbd6: -0x5d3)
    expected[Step + 1] = 0x140005CD;   // b 0xbce, cave + 10 (+0x5cd)
    expected[Step + 6] = Nop;
    expected[Step + 7] = Nop;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (text.words[i] != expected[i]) {
            std::printf("word %#zx: %#x, expected %#x\n", i, text.words[i], expected[i]);
        }
        CHECK(text.words[i] == expected[i]);
    }

    // The branches decoded independently of the encoder.
    REQUIRE(BranchTarget(Cave + 2, text.words[Cave + 2]) == Sleep + 1 + SleepSkip);
    REQUIRE(BranchTarget(Cave + 6, text.words[Cave + 6]) == Sleep + 2);
    REQUIRE(BranchTarget(Sleep, text.words[Sleep]) == Cave);
    REQUIRE(BranchTarget(Cave + 9, text.words[Cave + 9]) == Controller + 15);
    REQUIRE(BranchTarget(Controller + 14, text.words[Controller + 14]) == Cave + 7);
    REQUIRE(BranchTarget(Cave + 18, text.words[Cave + 18]) == Step + 3);
    REQUIRE(BranchTarget(Step + 1, text.words[Step + 1]) == Cave + 10);
    // The cave is past the text and inside its last page.
    static_assert(Cave * 4 >= TextSize && (Cave + CaveWords) * 4 <= AlignedSize);
}

TEST_CASE("Nx2DkDynamicFps: the cave at the page's end", "[core]") {
    // 0x2fb0 bytes: the cave at word 0xbec, its last word at 0xbfe, inside 0x3000.
    {
        Text text{0x2fb0};
        text.PutAll();
        const Result result = Apply(text.Bytes(), 0x2fb0, AlignedSize);
        REQUIRE(result.applied);
        REQUIRE(BranchTarget(Sleep, text.words[Sleep]) == 0xbec);
        REQUIRE(text.words[0xbec] == 0xEB080120);
        REQUIRE(BranchTarget(0xbec + 18, text.words[0xbec + 18]) == Step + 3);
        REQUIRE(text.words[0xbff] == 0);
    }
    // 0x2fb1: the cave would start at word 0xbf0 and end past the page.
    {
        Text text{0x2fb1};
        text.PutAll();
        const std::vector<u32> before = text.words;
        RequireUntouched(Apply(text.Bytes(), 0x2fb1, AlignedSize), before, text);
    }
}

TEST_CASE("Nx2DkDynamicFps: not applied leaves the text as it was", "[core]") {
    const auto check = [](Text& text, size_t controller_matches, size_t sleep_matches,
                          size_t step_matches) {
        const std::vector<u32> before = text.words;
        const Result result = Apply(text.Bytes(), TextSize, AlignedSize);
        RequireUntouched(result, before, text);
        REQUIRE(result.controller_matches == controller_matches);
        REQUIRE(result.sleep_matches == sleep_matches);
        REQUIRE(result.step_matches == step_matches);
    };
    {
        Text text; // no pattern
        check(text, 0, 0, 0);
    }
    {
        Text text;
        text.PutController(Controller);
        text.PutSleep(Sleep);
        check(text, 1, 1, 0);
    }
    {
        Text text;
        text.PutAll();
        text.PutStep(0x700); // two step matches
        check(text, 1, 1, 2);
    }
    {
        Text text;
        text.PutAll();
        text.PutController(0x300); // two controller matches
        check(text, 2, 1, 1);
    }
    {
        Text text;
        text.PutAll();
        text.PutSleep(0x500); // two sleep matches
        check(text, 1, 2, 1);
    }
    {
        Text text;
        text.PutAll();
        text.words[Sleep - 15] = 0x39472e69; // low_check - 1 differs
        check(text, 1, 1, 1);
    }
    {
        Text text;
        text.PutAll();
        text.words[Sleep - 14] = 0x350006a9; // low_check differs
        check(text, 1, 1, 1);
    }
    {
        Text text;
        text.PutAll();
        text.words[Controller + 14] = 0x2944266c; // the frame time word differs
        check(text, 1, 1, 1);
    }
    {
        Text text;
        text.PutAll();
        text.words[Sleep + 1] = 0x5400000C | (SleepSkip << 5); // b.gt, not b.le
        check(text, 1, 0, 1);
    }
    {
        Text text;
        text.PutAll();
        text.words[Sleep + 2] = 0x14000123; // b, not bl
        check(text, 1, 0, 1);
    }
    {
        // The sleep too close to the start for its checks (they would sit before the text).
        Text text;
        text.PutController(Controller);
        text.PutSleep(10, false);
        text.PutStep(Step);
        check(text, 1, 1, 1);
    }
    {
        // The frame time word past the end of the text.
        Text text;
        text.PutController(0xbc1 - 9, false);
        text.PutSleep(Sleep);
        text.PutStep(Step);
        check(text, 1, 1, 1);
    }
    {
        // A buffer shorter than the aligned size.
        Text text;
        text.PutAll();
        const std::vector<u32> before = text.words;
        RequireUntouched(Apply(text.Bytes().first(AlignedSize - 4), TextSize, AlignedSize), before,
                         text);
    }
}

TEST_CASE("Nx2DkDynamicFps: the switch, the title and the module", "[core]") {
    REQUIRE(!Enabled(nullptr));
    REQUIRE(!Enabled(""));
    REQUIRE(!Enabled("0"));
    REQUIRE(!Enabled("2"));
    REQUIRE(!Enabled("yes"));
    REQUIRE(Enabled("1"));
    const auto untouched = [](const char* value, u64 program_id, std::string_view module) {
        Text text;
        text.PutAll();
        const std::vector<u32> before = text.words;
        const auto result =
            ApplyIfEnabled(value, program_id, module, text.Bytes(), TextSize, AlignedSize);
        REQUIRE(!result.has_value());
        REQUIRE(text.words == before);
    };
    untouched(nullptr, ProgramId, "main");
    untouched("", ProgramId, "main");
    untouched("0", ProgramId, "main");
    untouched("yes", ProgramId, "main");
    untouched("1", 0x0400C3F00006E000ULL, "main"); // another title
    untouched("1", ProgramId, "sdk");
    untouched("1", ProgramId, "subsdk0");
    untouched("1", ProgramId, "rtld");
    Text text;
    text.PutAll();
    const auto result = ApplyIfEnabled("1", ProgramId, "main", text.Bytes(), TextSize, AlignedSize);
    REQUIRE(result.has_value());
    REQUIRE(result->applied);
    REQUIRE(text.words[Controller] == 0xF9407429);
}

TEST_CASE("Nx2DkDynamicFps: EDEN_NX2_DK_DYN_MIN_FPS's value", "[core]") {
    const auto none = [](const char* value) {
        const MinFps parsed = ParseMinFps(value);
        REQUIRE(parsed.fps == 0);
        REQUIRE(!parsed.refused);
    };
    const auto refused = [](const char* value) {
        const MinFps parsed = ParseMinFps(value);
        REQUIRE(parsed.fps == 0);
        REQUIRE(parsed.refused);
    };
    const auto fps = [](const char* value, u32 expected) {
        const MinFps parsed = ParseMinFps(value);
        REQUIRE(parsed.fps == expected);
        REQUIRE(!parsed.refused);
    };
    none(nullptr);
    none("");
    fps("1", 1);
    fps("24", 24);
    fps("25", 25);
    fps("30", 30);
    fps("40", 40);
    fps("60", 60);
    fps("060", 60);
    for (const char* value : {"0", "00", "61", "100", "1000", "0030", "-5", "+30", " 30", "30 ",
                              "30x", "3.5", "abc", "x"}) {
        refused(value);
    }
    // movz w14, #fps: the game's word for 40, imm16 at bits 5-20, rd 14.
    REQUIRE(LowTargetMovz(40) == LowTargetWord);
    for (u32 value = MinFpsLowest; value <= MinFpsHighest; ++value) {
        const u32 insn = LowTargetMovz(value);
        REQUIRE((insn & 0xFFE00000U) == 0x52800000U); // movz, 32-bit, hw 0
        REQUIRE(((insn >> 5) & 0xFFFF) == value);
        REQUIRE((insn & 31) == 14);
    }
}

TEST_CASE("Nx2DkDynamicFps: EDEN_NX2_DK_DYN_MIN_FPS: one word, nothing else", "[core]") {
    // Unset (0): the text exactly as before the switch existed (the 40 kept).
    Text plain;
    plain.PutAll();
    plain.PutLowTarget(Controller);
    Text unset = plain;
    const Result old_result = Apply(plain.Bytes(), TextSize, AlignedSize);
    const Result unset_result = Apply(unset.Bytes(), TextSize, AlignedSize, 0);
    REQUIRE(old_result.applied);
    REQUIRE(unset.words == plain.words);
    REQUIRE(unset_result.min_fps == 0);
    REQUIRE(!unset_result.min_fps_missed);
    REQUIRE(plain.words[Controller + 59] == 0x5280050E);
    for (const u32 value : {30U, 25U, 24U, 1U, 60U}) {
        Text text;
        text.PutAll();
        text.PutLowTarget(Controller);
        const Result result = Apply(text.Bytes(), TextSize, AlignedSize, value);
        REQUIRE(result.applied);
        REQUIRE(result.min_fps == value);
        REQUIRE(!result.min_fps_missed);
        REQUIRE(text.words[Controller + 59] == LowTargetMovz(value));
        for (size_t i = 0; i < text.words.size(); ++i) {
            if (i != Controller + 59) {
                CHECK(text.words[i] == plain.words[i]);
            }
        }
    }
    REQUIRE(LowTargetMovz(30) == 0x528003CE);
    REQUIRE(LowTargetMovz(25) == 0x5280032E);
    // Out of range values reaching Apply are ignored.
    for (const u32 value : {0U, 61U, 0xFFFFU, 0xFFFFFFFFU}) {
        Text text;
        text.PutAll();
        text.PutLowTarget(Controller);
        const Result result = Apply(text.Bytes(), TextSize, AlignedSize, value);
        REQUIRE(result.applied);
        REQUIRE(result.min_fps == 0);
        REQUIRE(text.words == plain.words);
    }
}

TEST_CASE("Nx2DkDynamicFps: EDEN_NX2_DK_DYN_MIN_FPS where the words differ", "[core]") {
    Text reference;
    reference.PutAll();
    reference.PutLowTarget(Controller);
    REQUIRE(Apply(reference.Bytes(), TextSize, AlignedSize).applied);
    const auto missed = [&](auto change) {
        Text text;
        text.PutAll();
        text.PutLowTarget(Controller);
        change(text);
        Text expected = text;
        REQUIRE(Apply(expected.Bytes(), TextSize, AlignedSize).applied);
        const Result result = Apply(text.Bytes(), TextSize, AlignedSize, 30);
        REQUIRE(result.applied); // the dynamic FPS patch itself still goes in
        REQUIRE(result.min_fps == 0);
        REQUIRE(result.min_fps_missed);
        REQUIRE(text.words == expected.words);
    };
    missed([](Text& t) { t.words[Controller + 59] = 0x528003CE; }); // another value there
    missed([](Text& t) { t.words[Controller + 59] = 0x5280050D; }); // another register
    missed([](Text& t) { t.words[Controller + 60] = 0x7A4E0021; }); // another ccmp
    missed([](Text& t) { t.words[Controller + 60] = Filler; });
    // Past the end of the text (the controller near its end): refused, no read past it.
    {
        Text text;
        text.PutController(0xbc1 - 60);
        text.PutSleep(Sleep);
        text.PutStep(Step);
        text.words[0xbc1 - 1] = 0x5280050E; // + 59, but + 60 is past the text
        const Result result = Apply(text.Bytes(), TextSize, AlignedSize, 30);
        REQUIRE(result.applied);
        REQUIRE(result.min_fps_missed);
        REQUIRE(text.words[0xbc1 - 1] == 0x5280050E);
    }
    // Not applied: the switch changes nothing either.
    {
        Text text;
        text.PutController(Controller);
        text.PutSleep(Sleep);
        text.PutLowTarget(Controller);
        const std::vector<u32> before = text.words;
        const Result result = Apply(text.Bytes(), TextSize, AlignedSize, 30);
        RequireUntouched(result, before, text);
        REQUIRE(result.min_fps == 0);
        REQUIRE(!result.min_fps_missed);
    }
}

TEST_CASE("Nx2DkDynamicFps: EDEN_NX2_DK_DYN_MIN_FPS through the loader's entry", "[core]") {
    {
        Text text;
        text.PutAll();
        text.PutLowTarget(Controller);
        const auto result =
            ApplyIfEnabled("1", ProgramId, "main", text.Bytes(), TextSize, AlignedSize, 25);
        REQUIRE(result.has_value());
        REQUIRE(result->min_fps == 25);
        REQUIRE(text.words[Controller + 59] == 0x5280032E);
    }
    {
        // DK-DYN off: the threshold alone does nothing.
        Text text;
        text.PutAll();
        text.PutLowTarget(Controller);
        const std::vector<u32> before = text.words;
        REQUIRE(!ApplyIfEnabled(nullptr, ProgramId, "main", text.Bytes(), TextSize, AlignedSize, 25)
                     .has_value());
        REQUIRE(text.words == before);
    }
}
