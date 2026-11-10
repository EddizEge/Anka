// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_rtss_osd.h"

namespace {
using namespace Common::Nx2RtssOsd;

// RTSS's slot: szOSD, szOSDOwner, szOSDEx, v2.12's 256 KiB buffer, v2.20's szOSDEx2.
constexpr u32 FullEntry = 256 + 256 + 4096 + 262144 + 32768;
constexpr u32 ArrayOffset = 0x60;
constexpr u32 Slots = 8;

// A fake shared memory block laid out as RTSS's: the header, then the slot array.
struct Memory {
    std::vector<u32> words; // u32 for the header's alignment, as the mapped view's

    explicit Memory(u32 version, u32 entry_size = FullEntry, u32 slots = Slots,
                    u32 extra = 4096) {
        words.assign((ArrayOffset + u64{slots} * entry_size + extra + 3) / 4, 0xCDCDCDCD);
        std::memset(words.data(), 0, ArrayOffset + u64{slots} * entry_size);
        Header& h = Head();
        h.signature = Signature;
        h.version = version;
        h.osd_entry_size = entry_size;
        h.osd_arr_offset = ArrayOffset;
        h.osd_arr_size = slots;
        h.app_arr_offset = 0x1234; // never read
    }

    std::span<u8> Bytes() {
        return {reinterpret_cast<u8*>(words.data()), words.size() * 4};
    }
    Header& Head() {
        return *reinterpret_cast<Header*>(words.data());
    }
    OsdEntry& Slot(u32 index) {
        return *reinterpret_cast<OsdEntry*>(Bytes().data() + ArrayOffset +
                                            u64{index} * Head().osd_entry_size);
    }
    void Own(u32 index, const char* owner) {
        std::strcpy(Slot(index).owner, owner);
    }
};

std::string_view Str(const char* text, size_t capacity) {
    return {text, strnlen(text, capacity)};
}

bool AllZero(const u8* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}
} // Anonymous namespace

TEST_CASE("Nx2RtssOsd: the switch", "[common]") {
    REQUIRE_FALSE(Enabled(nullptr));
    REQUIRE_FALSE(Enabled(""));
    REQUIRE_FALSE(Enabled("0"));
    REQUIRE_FALSE(Enabled("yes"));
    REQUIRE(Enabled("1"));
    REQUIRE(Enabled("10"));
    static_assert(Signature == ((u32{'R'} << 24) | (u32{'T'} << 16) | (u32{'S'} << 8) | u32{'S'}));
}

TEST_CASE("Nx2RtssOsd: v2.14+: the first free slot past slot 0, under the lock", "[common]") {
    Memory m{0x00020015};
    m.Head().osd_frame = 41;
    REQUIRE(Update(m.Bytes(), "Game 30.0 FPS") == Status::Written);
    REQUIRE(m.Slot(0).owner[0] == '\0'); // slot 0 is the primary client's
    REQUIRE(Str(m.Slot(1).owner, 256) == "EdenNX2");
    REQUIRE(Str(m.Slot(1).text_ex, 4096) == "Game 30.0 FPS");
    REQUIRE(m.Slot(1).text[0] == '\0');
    REQUIRE(m.Head().osd_frame == 42);
    REQUIRE(m.Head().busy == 0);
    for (u32 i = 2; i < Slots; ++i) {
        REQUIRE(m.Slot(i).owner[0] == '\0');
    }
    // The next update finds the same slot, even with slot 1 freed meanwhile... (owned first)
    REQUIRE(Update(m.Bytes(), "Game 31.0 FPS") == Status::Written);
    REQUIRE(Str(m.Slot(1).text_ex, 4096) == "Game 31.0 FPS");
    REQUIRE(m.Head().osd_frame == 43);
    // A shorter text ends where it should.
    REQUIRE(Update(m.Bytes(), "x") == Status::Written);
    REQUIRE(Str(m.Slot(1).text_ex, 4096) == "x");
}

TEST_CASE("Nx2RtssOsd: other owners are left alone, ours is found before a free one",
          "[common]") {
    Memory m{0x00020015};
    m.Own(1, "MSIAfterburner");
    m.Own(2, "SomeTool");
    m.Own(4, "EdenNX2");
    std::strcpy(m.Slot(1).text_ex, "theirs");
    REQUIRE(Update(m.Bytes(), "ours") == Status::Written);
    REQUIRE(Str(m.Slot(4).text_ex, 4096) == "ours");
    REQUIRE(m.Slot(3).owner[0] == '\0'); // free, but ours came first
    REQUIRE(Str(m.Slot(1).text_ex, 4096) == "theirs");
    REQUIRE(Str(m.Slot(1).owner, 256) == "MSIAfterburner");
    REQUIRE(Str(m.Slot(2).owner, 256) == "SomeTool");
    // A prefix of our name is not our name.
    Memory p{0x00020015};
    p.Own(1, "EdenNX");
    p.Own(2, "EdenNX2x");
    REQUIRE(Update(p.Bytes(), "t") == Status::Written);
    REQUIRE(Str(p.Slot(3).owner, 256) == "EdenNX2");
}

TEST_CASE("Nx2RtssOsd: every slot taken: nothing written", "[common]") {
    Memory m{0x00020015};
    for (u32 i = 1; i < Slots; ++i) {
        m.Own(i, "other");
    }
    const auto before = m.words;
    REQUIRE(Update(m.Bytes(), "t") == Status::NoSlot);
    REQUIRE(m.words == before);
}

TEST_CASE("Nx2RtssOsd: the busy lock held: nothing written, the lock left set", "[common]") {
    Memory m{0x0002000e};
    m.Head().busy = 1;
    m.Head().osd_frame = 7;
    REQUIRE(Update(m.Bytes(), "t") == Status::Busy);
    REQUIRE(m.Head().busy == 1);
    REQUIRE(m.Head().osd_frame == 7);
    REQUIRE(m.Slot(1).text_ex[0] == '\0');
    // Released by its holder: the next update writes.
    m.Head().busy = 0;
    REQUIRE(Update(m.Bytes(), "t") == Status::Written);
    REQUIRE(Str(m.Slot(1).text_ex, 4096) == "t");
    REQUIRE(m.Head().busy == 0);
    REQUIRE(m.Head().osd_frame == 8);
}

TEST_CASE("Nx2RtssOsd: before v2.14 there is no lock, before v2.7 no szOSDEx", "[common]") {
    // v2.7-2.13: dwBusy is not a lock yet (whatever is there is ignored and kept).
    Memory a{0x0002000d};
    a.Head().busy = 1;
    REQUIRE(Update(a.Bytes(), "ex") == Status::Written);
    REQUIRE(Str(a.Slot(1).text_ex, 4096) == "ex");
    REQUIRE(a.Head().busy == 1);
    // v2.0-2.6: szOSD, slots of 512 bytes (no szOSDEx to write past).
    Memory b{0x00020006, 512};
    b.Head().busy = 1;
    REQUIRE(Update(b.Bytes(), "old") == Status::Written);
    REQUIRE(Str(b.Slot(1).text, 256) == "old");
    REQUIRE(Str(b.Slot(1).owner, 256) == "EdenNX2");
    REQUIRE(b.Head().osd_frame == 1);
    // Long text: cut at the field's size, terminated.
    const std::string long_text(5000, 'a');
    REQUIRE(Update(b.Bytes(), long_text) == Status::Written);
    REQUIRE(Str(b.Slot(1).text, 256).size() == 255);
    REQUIRE(Str(b.Slot(1).owner, 256) == "EdenNX2"); // the next field intact
    REQUIRE(Update(a.Bytes(), long_text) == Status::Written);
    REQUIRE(Str(a.Slot(1).text_ex, 4096).size() == 4095);
    REQUIRE(a.Slot(1).text_ex[4095] == '\0');
}

TEST_CASE("Nx2RtssOsd: no server: nothing touched", "[common]") {
    for (const u32 signature : {0U, 0xDEADU, 0x53535452U /* bytes reversed */}) {
        Memory m{0x00020015};
        m.Head().signature = signature;
        const auto before = m.words;
        REQUIRE(Update(m.Bytes(), "t") == Status::NoServer);
        REQUIRE(Release(m.Bytes(), true) == Status::NoServer);
        REQUIRE(m.words == before);
    }
    Memory v1{0x00010004};
    const auto before = v1.words;
    REQUIRE(Update(v1.Bytes(), "t") == Status::NoServer);
    REQUIRE(v1.words == before);
    // Shorter than the header.
    std::vector<u8> tiny(39, 0);
    REQUIRE(Update(tiny, "t") == Status::NoServer);
    REQUIRE(Update({}, "t") == Status::NoServer);
}

TEST_CASE("Nx2RtssOsd: a slot array outside the view is refused", "[common]") {
    const auto refused = [](auto change) {
        Memory m{0x00020015};
        change(m.Head());
        const auto before = m.words;
        REQUIRE(Update(m.Bytes(), "t") == Status::BadLayout);
        REQUIRE(Release(m.Bytes(), true) == Status::BadLayout);
        REQUIRE(m.words == before);
    };
    refused([](Header& h) { h.osd_arr_size = 9; });                 // one slot past the end
    refused([](Header& h) { h.osd_arr_size = 0xFFFFFFFF; });         // no 32-bit wrap
    refused([](Header& h) { h.osd_entry_size = 0xFFFFFFFF; });
    refused([](Header& h) { h.osd_arr_offset = 0xFFFFFFF0; });
    refused([](Header& h) { h.osd_entry_size = 4607; });             // szOSDEx cut off (v2.7+)
    refused([](Header& h) { h.osd_arr_size = 1; });                  // slot 0 only
    refused([](Header& h) { h.osd_arr_offset = 8; });                // over the header
    Memory old{0x00020006, 511};
    REQUIRE(Update(old.Bytes(), "t") == Status::BadLayout);
    // Exactly fitting: accepted (the view ends at the last slot's end).
    Memory exact{0x00020015, FullEntry, Slots, 0};
    REQUIRE(Update(exact.Bytes().first(ArrayOffset + u64{Slots} * FullEntry), "t") ==
            Status::Written);
}

TEST_CASE("Nx2RtssOsd: release empties our slot whole, under the lock", "[common]") {
    Memory m{0x00020015};
    m.Own(1, "other");
    std::strcpy(m.Slot(1).text_ex, "theirs");
    REQUIRE(Update(m.Bytes(), "ours") == Status::Written);
    REQUIRE(Str(m.Slot(2).owner, 256) == "EdenNX2");
    m.Slot(2).text[0] = 'z'; // anything in the slot goes
    std::memset(reinterpret_cast<u8*>(&m.Slot(2)) + FullEntry - 16, 0x77, 16);
    const u32 frame = m.Head().osd_frame;
    REQUIRE(Release(m.Bytes(), false) == Status::Released);
    REQUIRE(AllZero(reinterpret_cast<const u8*>(&m.Slot(2)), FullEntry));
    REQUIRE(m.Head().osd_frame == frame + 1);
    REQUIRE(m.Head().busy == 0);
    REQUIRE(Str(m.Slot(1).owner, 256) == "other");
    REQUIRE(Str(m.Slot(1).text_ex, 4096) == "theirs");
    // Nothing of ours left: no frame bump.
    REQUIRE(Release(m.Bytes(), false) == Status::Released);
    REQUIRE(m.Head().osd_frame == frame + 1);
}

TEST_CASE("Nx2RtssOsd: release with the lock held: waits, or clears without it", "[common]") {
    Memory m{0x00020015};
    REQUIRE(Update(m.Bytes(), "ours") == Status::Written);
    m.Head().busy = 1;
    const auto before = m.words;
    REQUIRE(Release(m.Bytes(), false) == Status::Busy);
    REQUIRE(m.words == before);
    REQUIRE(Release(m.Bytes(), true) == Status::Released);
    REQUIRE(m.Slot(1).owner[0] == '\0');
    REQUIRE(m.Slot(1).text_ex[0] == '\0');
    REQUIRE(m.Head().busy == 1); // not ours to clear
    // v2.7-2.13: no lock, a set bit does not stop it.
    Memory a{0x0002000d};
    REQUIRE(Update(a.Bytes(), "ours") == Status::Written);
    a.Head().busy = 1;
    REQUIRE(Release(a.Bytes(), false) == Status::Released);
    REQUIRE(a.Slot(1).owner[0] == '\0');
}

TEST_CASE("Nx2RtssOsd: a slot left by a process ended from outside: emptied by a start without "
          "the OSD",
          "[common]") {
    // The killed process wrote its text and never released it; RTSS's memory outlives it.
    Memory m{0x00020015};
    m.Own(1, "other");
    std::strcpy(m.Slot(1).text, "theirs");
    REQUIRE(Update(m.Bytes(), "Game 30 FPS | Display 60 FPS | Speed 100%") == Status::Written);
    REQUIRE(Str(m.Slot(2).owner, 256) == "EdenNX2");
    const u32 frame = m.Head().osd_frame;
    // The next process starts with EDEN_NX2_OSD off: the switch says so, and it releases ours.
    REQUIRE_FALSE(Enabled(nullptr));
    REQUIRE_FALSE(Enabled("0"));
    REQUIRE(Release(m.Bytes(), false) == Status::Released);
    REQUIRE(AllZero(reinterpret_cast<const u8*>(&m.Slot(2)), FullEntry));
    REQUIRE(m.Head().osd_frame == frame + 1);
    REQUIRE(m.Head().busy == 0);
    REQUIRE(Str(m.Slot(1).owner, 256) == "other");
    REQUIRE(Str(m.Slot(1).text, 256) == "theirs");
    // Nothing of ours (the usual start without the OSD): RTSS's memory is left as it was.
    Memory clean{0x00020015};
    clean.Own(1, "other");
    const auto before = clean.words;
    REQUIRE(Release(clean.Bytes(), false) == Status::Released);
    REQUIRE(clean.words == before);
}

TEST_CASE("Nx2RtssOsd: RTSS restarted (memory laid out again): the slot taken again",
          "[common]") {
    Memory m{0x00020015};
    REQUIRE(Update(m.Bytes(), "a") == Status::Written);
    m.Head().signature = 0xDEAD; // closing
    REQUIRE(Update(m.Bytes(), "b") == Status::NoServer);
    Memory fresh{0x00020015};
    fresh.Own(1, "MSIAfterburner"); // another client got there first this time
    REQUIRE(Update(fresh.Bytes(), "c") == Status::Written);
    REQUIRE(Str(fresh.Slot(2).owner, 256) == "EdenNX2");
    REQUIRE(Str(fresh.Slot(2).text_ex, 4096) == "c");
}

TEST_CASE("Nx2RtssOsd: the text", "[common]") {
    REQUIRE(Format({34.5, 120.0, 0.99}) == "Game 34.5 FPS | Display 120 FPS | Speed 99%");
    REQUIRE(Format({60.0, 120.2, 1.0}) == "Game 60.0 FPS | Display 120 FPS | Speed 100%");
    REQUIRE(Format({20.04, 59.6, 0.8333}) ==
            "Game 20.0 FPS | Display 60 FPS | <C=FF5050>Speed 83%<C>");
    REQUIRE(Format({0.0, 0.0, std::nullopt}) == "Game 0.0 FPS | Display 0 FPS | Speed -");
    // The red starts below 95%.
    REQUIRE(Format({30.0, 120.0, 0.95}) == "Game 30.0 FPS | Display 120 FPS | Speed 95%");
    REQUIRE(Format({30.0, 120.0, 0.9499}).find("<C=FF5050>") != std::string::npos);
    for (const char c : Format({34.5, 120.0, 0.5})) {
        REQUIRE(static_cast<unsigned char>(c) < 0x80); // ASCII only
    }
    // Frame generation: the multiplier after the display FPS, one decimal.
    REQUIRE(Format({40.0, 120.0, 1.0, 3.0}) == "Game 40.0 FPS | Display 120 FPS (3.0x) | Speed 100%");
    REQUIRE(Format({34.5, 120.0, 0.99, 3.4783}) ==
            "Game 34.5 FPS | Display 120 FPS (3.5x) | Speed 99%");
    REQUIRE(Format({20.0, 60.0, 0.8333, 3.0}) ==
            "Game 20.0 FPS | Display 60 FPS (3.0x) | <C=FF5050>Speed 83%<C>");
    REQUIRE(Format({30.0, 30.0, std::nullopt, 1.0}) ==
            "Game 30.0 FPS | Display 30 FPS (1.0x) | Speed -");
    // None: the text as before.
    REQUIRE(Format({40.0, 120.0, 1.0, std::nullopt}) == "Game 40.0 FPS | Display 120 FPS | Speed 100%");
    for (const char c : Format({34.5, 120.0, 0.5, 3.5})) {
        REQUIRE(static_cast<unsigned char>(c) < 0x80);
    }
}

TEST_CASE("Nx2RtssOsd: the rates over the window", "[common]") {
    const Sample a{.seconds = 10.0,
                   .game_frames = 1000,
                   .output_frames = 4000,
                   .speed_frame_us = 5'000'000,
                   .speed_game_us = 4'900'000,
                   .system_us = 7'000'000};
    // 1 s later: 34 game frames, 120 shown, presents stepped 0.98 s of game time in 1 s.
    const Sample b{.seconds = 11.0,
                   .game_frames = 1034,
                   .output_frames = 4120,
                   .speed_frame_us = 6'000'000,
                   .speed_game_us = 5'880'000,
                   .system_us = 8'000'000};
    const Rates r = Compute(a, b);
    REQUIRE(r.game_fps == 34.0);
    REQUIRE(r.display_fps == 120.0);
    REQUIRE(r.speed.has_value());
    REQUIRE((*r.speed > 0.9799 && *r.speed < 0.9801));
    // Frame generation never ran: no multiplier, the text as before.
    REQUIRE_FALSE(r.multiplier.has_value());
    REQUIRE(Format(r) == "Game 34.0 FPS | Display 120 FPS | Speed 98%");
    // No present meanwhile, after presents timed the game: it stood still (a stall), 0, whatever
    // the clock says.
    Sample c = b;
    c.speed_frame_us = a.speed_frame_us;
    c.speed_game_us = a.speed_game_us;
    c.system_us = 7'500'000;
    REQUIRE(Compute(a, c).speed.has_value());
    REQUIRE(*Compute(a, c).speed == 0.0);
    REQUIRE(Format(Compute(a, c)).find("<C=FF5050>Speed 0%<C>") != std::string::npos);
    // The presents never timed the game: the emulated clock against the wall clock (single core).
    Sample f = a, g = c;
    f.speed_frame_us = f.speed_game_us = g.speed_frame_us = g.speed_game_us = 0;
    REQUIRE(*Compute(f, g).speed == 0.5);
    // No clock either (multicore passes none): no speed.
    Sample d = g, e = f;
    d.system_us = e.system_us = 0;
    REQUIRE_FALSE(Compute(e, d).speed.has_value());
    REQUIRE(Format(Compute(e, d)).find("Speed -") != std::string::npos);
    // No time passed: nothing.
    const Rates z = Compute(a, a);
    REQUIRE(z.game_fps == 0.0);
    REQUIRE_FALSE(z.speed.has_value());
    // A window of 0.5 s (the first text).
    Sample h = b;
    h.seconds = 10.5;
    REQUIRE(Compute(a, h).game_fps == 68.0);
}

TEST_CASE("Nx2RtssOsd: the frame generation multiplier", "[common]") {
    const Sample a{.seconds = 10.0,
                   .game_frames = 1000,
                   .output_frames = 4000,
                   .generated_frames = 2500,
                   .speed_frame_us = 5'000'000,
                   .speed_game_us = 5'000'000};
    // 1 s: 40 game frames, 80 generated (2 per frame): 3.0x, whatever the presents counted.
    Sample b = a;
    b.seconds = 11.0;
    b.game_frames = 1040;
    b.output_frames = 4118; // two still in the queue
    b.generated_frames = 2580;
    b.speed_frame_us = b.speed_game_us = 6'000'000;
    const Rates r = Compute(a, b);
    REQUIRE(r.multiplier.has_value());
    REQUIRE(*r.multiplier == 3.0);
    REQUIRE(Format(r) == "Game 40.0 FPS | Display 118 FPS (3.0x) | Speed 100%");
    // A varying count (FSR3's pacing): 23 frames, 57 generated in 0.5 s: 80 / 23 = 3.478.
    Sample c = a;
    c.seconds = 10.5;
    c.game_frames = 1023;
    c.output_frames = 4080;
    c.generated_frames = 2557;
    REQUIRE(Format(Compute(a, c)).find("Display 160 FPS (3.5x) | ") != std::string::npos);
    // On but adding nothing this window (the game at the target rate): 1.0x.
    Sample d = b;
    d.generated_frames = a.generated_frames;
    REQUIRE(*Compute(a, d).multiplier == 1.0);
    // No game frame in the window (a stall): no multiplier, though generation is on.
    Sample e = a;
    e.seconds = 11.0;
    REQUIRE_FALSE(Compute(a, e).multiplier.has_value());
    REQUIRE(Format(Compute(a, e)).find("Display 0 FPS | ") != std::string::npos);
    // Generation off since the start (its counter 0): none, the text as before.
    Sample f = a, g = b;
    f.generated_frames = g.generated_frames = 0;
    REQUIRE_FALSE(Compute(f, g).multiplier.has_value());
    REQUIRE(Format(Compute(f, g)) == "Game 40.0 FPS | Display 118 FPS | Speed 100%");
    // The first generated images inside the window count from 0.
    Sample h = b;
    h.generated_frames = 40;
    f.generated_frames = 0;
    REQUIRE(*Compute(f, h).multiplier == 2.0);
}

TEST_CASE("Nx2RtssOsd: the DK_DYN conflict line", "[common]") {
    using namespace Common::Nx2RtssOsd;
    // Shown before the first game frame (the 30 s not started) and for 30 s after it
    REQUIRE(ConflictLineShown(std::nullopt));
    REQUIRE(ConflictLineShown(0.0));
    REQUIRE(ConflictLineShown(29.9));
    REQUIRE_FALSE(ConflictLineShown(30.0));
    REQUIRE_FALSE(ConflictLineShown(120.0));
    // A mod; a cheat with its source
    REQUIRE(ConflictLine("Unlock 60fps", "") ==
            "Dynamic FPS off: turn off \"Unlock 60fps\" in Add-Ons");
    REQUIRE(ConflictLine("DKB Cheats", "60 FPS") ==
            "Dynamic FPS off: cheat [60 FPS] in \"DKB Cheats\" (Add-Ons)");
    // No mod named: the free space it writes to was in use
    REQUIRE(ConflictLine("", "") == "Dynamic FPS off: code space in use (see the log)");
    // Long names cut with "...", the quote after the cut
    const std::string longer(60, 'x');
    REQUIRE(ConflictLine(longer, "") ==
            "Dynamic FPS off: turn off \"" + std::string(37, 'x') + "...\" in Add-Ons");
    REQUIRE(ConflictLine(longer, longer) == "Dynamic FPS off: cheat [" + std::string(29, 'x') +
                                                "...] in \"" + std::string(29, 'x') +
                                                "...\" (Add-Ons)");
    // UTF-8: one '?' per character, never half of one; control bytes and RTSS tags too
    REQUIRE(OsdName("\xC3\x87\xC3\xB6z\xC3\xBCn\xC3\xBCrl\xC3\xBCk") == "??z?n?rl?k");
    REQUIRE(OsdName("\xE2\x82\xAC 60") == "? 60");
    REQUIRE(OsdName("a\nb<C=FF0000>") == "a?b?C=FF0000?");
    REQUIRE(OsdName(std::string(10, 'a') + "\xC3\x87", 8) == "aaaaa...");
    REQUIRE(OsdName("\x80\x80z") == "?z");
    REQUIRE(OsdName("") == "");
}
