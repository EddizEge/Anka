// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_front_diag.h"

using namespace VideoCommon::Nx2FrontDiag;

namespace {

constexpr u32 Bit(WriteKind kind) {
    return 1U << static_cast<u32>(kind);
}
constexpr u32 Bit(WorkKind kind) {
    return 1U << static_cast<u32>(kind);
}

// Texture::CalculateSize, tiled (decoders.cpp): GOBs 64 bytes wide, 8 rows high, 1 deep.
u64 CalculateSizeTiled(u32 bytes_per_pixel, u32 width, u32 height, u32 depth, u32 block_height,
                       u32 block_depth) {
    const auto align = [](u64 value, u32 shift) {
        return (value + (u64{1} << shift) - 1) >> shift << shift;
    };
    return align(u64{width} * bytes_per_pixel, 6) * align(height, 3 + block_height) *
           align(depth, block_depth);
}

// MaxwellDMA's single line copy into block linear memory, 16 bytes at a time (maxwell_dma.cpp).
u64 LinearToBlockLinear(u64 address) {
    return (address & ~0x1f0ULL) | ((address & 0x40) >> 2) | ((address & 0x10) << 1) |
           ((address & 0x180) >> 1) | ((address & 0x20) << 3);
}

} // Anonymous namespace

TEST_CASE("Nx2FrontDiag: sent end bound", "[video_core]") {
    constexpr u64 Ring = 1 << 20;
    REQUIRE(SentEndBound(1000, 0, Ring, 64) == 1000);
    REQUIRE(SentEndBound(1000, 1, Ring, 64) == 1065);
    REQUIRE(SentEndBound(1000, 64, Ring, 64) == 1065);
    // The batch would cross the ring's end: it goes at the next start.
    REQUIRE(SentEndBound(Ring - 10, 3, Ring, 64) == Ring + 65);
    REQUIRE(SentEndBound(3 * Ring - 10, 3, Ring, 64) == 3 * Ring + 65);
    // Exactly to the end still fits.
    REQUIRE(SentEndBound(Ring - 65, 1, Ring, 64) == Ring);
    REQUIRE(SentEndBound(Ring - 64, 1, Ring, 64) == Ring + 65);
}

TEST_CASE("Nx2FrontDiag: sent end bound against a batching ring", "[video_core]") {
    // GpuSplit's front: entries never cross the ring's end (a skip to its start), held macro
    // writes go as one batch entry (a header and the writes) before any other entry, at most
    // MaxHeld of them. Every write's batch must end by the bound taken when it was held, every
    // other entry by the bound taken right after it.
    constexpr u64 Ring = 64;
    constexpr u32 MaxHeld = 8;
    std::mt19937 rng{7};
    for (int round = 0; round < 200; ++round) {
        u64 position = 0;
        u32 held = 0;
        std::vector<u64> held_bounds;
        const auto place = [&](u64 words) {
            if ((position & (Ring - 1)) + words > Ring) {
                position += Ring - (position & (Ring - 1));
            }
            position += words;
        };
        const auto flush = [&] {
            if (held == 0) {
                return;
            }
            place(1 + u64{held});
            for (const u64 bound : held_bounds) {
                REQUIRE(position <= bound);
            }
            held = 0;
            held_bounds.clear();
        };
        for (int step = 0; step < 400; ++step) {
            if (rng() % 3 != 0) {
                if (held == MaxHeld) {
                    flush();
                }
                ++held;
                held_bounds.push_back(SentEndBound(position, held, Ring, MaxHeld));
            } else {
                flush();
                place(1 + rng() % 6);
                REQUIRE(SentEndBound(position, 0, Ring, MaxHeld) == position);
            }
        }
        flush();
    }
}

TEST_CASE("Nx2FrontDiag: pending writes intersect until the back ran them", "[video_core]") {
    PendingWrites pending;
    pending.NoteWrite(WriteKind::CbData, 0x1000, 0x1010, 100);
    pending.NoteWrite(WriteKind::Release, 0x2000, 0x2010, 200);
    REQUIRE(pending.Query(0x1008, 0x100c, 50).writes == Bit(WriteKind::CbData));
    REQUIRE(pending.Query(0x0ff0, 0x2004, 50).writes ==
            (Bit(WriteKind::CbData) | Bit(WriteKind::Release)));
    // Half open: touching is not intersecting.
    REQUIRE(pending.Query(0x1010, 0x2000, 50).writes == 0);
    REQUIRE(pending.Query(0x0f00, 0x1000, 50).writes == 0);
    // Ran once the back reached the bound.
    REQUIRE(pending.Query(0x1000, 0x1004, 99).writes == Bit(WriteKind::CbData));
    REQUIRE(pending.Query(0x1000, 0x1004, 100).writes == 0);
    REQUIRE(pending.Size() == 1);
    REQUIRE(pending.Query(0x2000, 0x2004, 199).writes == Bit(WriteKind::Release));
    REQUIRE(pending.Query(0x2000, 0x2004, 200).writes == 0);
    REQUIRE(pending.Size() == 0);
}

TEST_CASE("Nx2FrontDiag: pending writes of a kind in a row are one range", "[video_core]") {
    PendingWrites pending;
    for (u64 word = 0; word < 16; ++word) {
        pending.NoteWrite(WriteKind::CbData, 0x4000 + word * 4, 0x4004 + word * 4, 10 + word);
    }
    REQUIRE(pending.Size() == 1);
    // The range ran with its last word.
    REQUIRE(pending.Query(0x4000, 0x4004, 24).writes == Bit(WriteKind::CbData));
    REQUIRE(pending.Query(0x4000, 0x4004, 25).writes == 0);
    // Another kind next to it stays apart, and so does a gap.
    pending.NoteWrite(WriteKind::CbData, 0x5000, 0x5004, 30);
    pending.NoteWrite(WriteKind::Report, 0x5004, 0x5014, 31);
    pending.NoteWrite(WriteKind::CbData, 0x5100, 0x5104, 32);
    REQUIRE(pending.Size() == 3);
    REQUIRE(pending.Query(0x5004, 0x5008, 0).writes == Bit(WriteKind::Report));
    REQUIRE(pending.Query(0x5000, 0x5004, 0).writes == Bit(WriteKind::CbData));
    REQUIRE(pending.Query(0x5050, 0x5060, 0).writes == 0);
}

TEST_CASE("Nx2FrontDiag: a merged range keeps the later bound", "[video_core]") {
    PendingWrites pending;
    pending.NoteWrite(WriteKind::Inline, 0x100, 0x200, 100);
    // A lower bound noted later (a held batch's bound is above the next entry's) merges in.
    pending.NoteWrite(WriteKind::Inline, 0x200, 0x300, 50);
    REQUIRE(pending.Size() == 1);
    REQUIRE(pending.Query(0x100, 0x104, 60).writes == Bit(WriteKind::Inline));
    REQUIRE(pending.Query(0x2f0, 0x2f4, 99).writes == Bit(WriteKind::Inline));
    REQUIRE(pending.Query(0x2f0, 0x2f4, 100).writes == 0);
}

TEST_CASE("Nx2FrontDiag: bounds noted out of order retire in order", "[video_core]") {
    PendingWrites pending;
    pending.NoteWrite(WriteKind::Dma, 0x1000, 0x2000, 100);
    pending.NoteWrite(WriteKind::Release, 0x8000, 0x8004, 40);
    // The release is behind the copy: still pending while the copy is.
    REQUIRE(pending.Query(0x8000, 0x8004, 60).writes == Bit(WriteKind::Release));
    REQUIRE(pending.Query(0x8000, 0x8004, 100).writes == 0);
}

TEST_CASE("Nx2FrontDiag: empty writes are not noted", "[video_core]") {
    PendingWrites pending;
    pending.NoteWrite(WriteKind::CbData, 0x500, 0x500, 10);
    pending.NoteWrite(WriteKind::CbData, 0x600, 0x5ff, 10);
    REQUIRE(pending.Size() == 0);
    REQUIRE(pending.Query(0x0, 0x1000, 0).writes == 0);
}

TEST_CASE("Nx2FrontDiag: an empty query meets no write, work still pending", "[video_core]") {
    PendingWrites pending;
    pending.NoteWrite(WriteKind::CbData, 0, 0x100, 10);
    pending.NoteWork(WorkKind::Syncpoint, 20);
    const auto empty = pending.Query(0, 0, 5);
    REQUIRE(empty.writes == 0);
    REQUIRE(empty.work == Bit(WorkKind::Syncpoint));
    REQUIRE(empty.entries == 1);
    REQUIRE(pending.Query(0, 1, 5).writes == Bit(WriteKind::CbData));
}

TEST_CASE("Nx2FrontDiag: work is pending until its latest bound ran", "[video_core]") {
    PendingWrites pending;
    pending.NoteWork(WorkKind::Draw, 100);
    pending.NoteWork(WorkKind::Draw, 50); // An earlier bound never takes the later one back
    pending.NoteWork(WorkKind::Compute, 70);
    REQUIRE(pending.Query(0, 4, 60).work == (Bit(WorkKind::Draw) | Bit(WorkKind::Compute)));
    REQUIRE(pending.Query(0, 4, 70).work == Bit(WorkKind::Draw));
    REQUIRE(pending.Query(0, 4, 99).work == Bit(WorkKind::Draw));
    REQUIRE(pending.Query(0, 4, 100).work == 0);
    REQUIRE(pending.Query(0, 4, 100).writes == 0);
}

TEST_CASE("Nx2FrontDiag: a full list is a hit until the dropped writes ran", "[video_core]") {
    PendingWrites pending;
    for (u64 i = 0; i < PendingWrites::MaxEntries; ++i) {
        pending.NoteWrite(i % 2 == 0 ? WriteKind::CbData : WriteKind::Report, i * 0x100,
                          i * 0x100 + 4, 10 + i);
    }
    REQUIRE(pending.Size() == PendingWrites::MaxEntries);
    REQUIRE(!pending.Query(0, 4, 0).overflow);
    const u64 last = 10 + PendingWrites::MaxEntries;
    pending.NoteWrite(WriteKind::Dma, 0x7000'0000, 0x7000'0004, last);
    REQUIRE(pending.Size() == PendingWrites::MaxEntries);
    const auto dropped = pending.Query(0x7000'0000, 0x7000'0004, last - 1);
    REQUIRE(dropped.overflow);
    REQUIRE(dropped.writes == 0);
    // Every write ran.
    const auto ran = pending.Query(0x7000'0000, 0x7000'0004, last);
    REQUIRE(!ran.overflow);
    REQUIRE(pending.Size() == 0);
    // The first entries were kept intact.
    PendingWrites again;
    for (u64 i = 0; i < PendingWrites::MaxEntries + 8; ++i) {
        again.NoteWrite(WriteKind::CbData, i * 0x100, i * 0x100 + 4, 10 + i);
    }
    REQUIRE(again.Query(0, 4, 0).writes == Bit(WriteKind::CbData));
    REQUIRE(again.Query(0x100, 0x104, 0).writes == Bit(WriteKind::CbData));
    REQUIRE(again.Query(0x200, 0x204, 11).writes == Bit(WriteKind::CbData));
    REQUIRE(again.Query(0x100, 0x104, 11).writes == 0);
    REQUIRE(again.Query(0, 4, 11).writes == 0);
    REQUIRE(again.Size() == PendingWrites::MaxEntries - 2);
}

TEST_CASE("Nx2FrontDiag: reset starts over", "[video_core]") {
    PendingWrites pending;
    pending.NoteWrite(WriteKind::CbData, 0x10, 0x20, 1000);
    pending.NoteWork(WorkKind::Run, 1000);
    pending.Reset();
    const auto now = pending.Query(0x0, 0x100, 0);
    REQUIRE(now.writes == 0);
    REQUIRE(now.work == 0);
    REQUIRE(pending.Size() == 0);
    // Bounds start over too (a new session's positions are small again).
    pending.NoteWrite(WriteKind::CbData, 0x10, 0x20, 5);
    REQUIRE(pending.Query(0x10, 0x14, 4).writes == Bit(WriteKind::CbData));
    REQUIRE(pending.Query(0x10, 0x14, 5).writes == 0);
}

TEST_CASE("Nx2FrontDiag: pending writes against a model of the ring", "[video_core]") {
    // Entries go into a ring at increasing positions; the back runs them in order, consumed
    // moving to an entry's end at most. A write not run yet that intersects a read must be
    // reported (sound); with exact bounds and no ranges touching, nothing else is (exact).
    std::mt19937_64 rng{11};
    for (int round = 0; round < 300; ++round) {
        const bool exact = round % 2 == 0;
        struct Sent {
            WriteKind kind;
            u64 begin;
            u64 end;
            u64 entry_end;
        };
        std::vector<Sent> sent;
        std::vector<std::pair<WorkKind, u64>> work;
        PendingWrites pending;
        u64 position = 0;
        u64 consumed = 0;
        u64 slot = 0;
        for (int step = 0; step < 300; ++step) {
            const u32 action = static_cast<u32>(rng() % 10);
            if (action < 5) {
                position += 1 + rng() % 4;
                const auto kind =
                    static_cast<WriteKind>(rng() % static_cast<u64>(WriteKind::Count));
                u64 begin;
                u64 size;
                if (exact) {
                    // Apart from each other: slots 0x100 apart, a gap after each.
                    begin = (slot++ % 64) * 0x100 + (rng() % 8) * 4;
                    size = 4 + (rng() % 8) * 4;
                } else {
                    begin = (rng() % 64) * 4;
                    size = 4 * (1 + rng() % 8);
                }
                // Exact bounds, or a held batch's (up to 9 words further).
                const u64 bound = exact ? position : position + rng() % 10;
                sent.push_back({kind, begin, begin + size, position});
                pending.NoteWrite(kind, begin, begin + size, bound);
            } else if (action < 6) {
                position += 2;
                const auto kind = static_cast<WorkKind>(rng() % static_cast<u64>(WorkKind::Count));
                work.emplace_back(kind, position);
                pending.NoteWork(kind, exact ? position : position + rng() % 10);
            } else if (action < 8) {
                // The back runs some entries.
                if (consumed < position) {
                    consumed += rng() % (position - consumed + 1);
                }
            } else {
                const u64 begin = (rng() % 80) * 0x40;
                const u64 end = begin + 4 * (1 + rng() % 16);
                u32 writes = 0;
                for (const Sent& entry : sent) {
                    if (entry.entry_end > consumed && entry.begin < end && begin < entry.end) {
                        writes |= Bit(entry.kind);
                    }
                }
                u32 busy = 0;
                for (const auto& [kind, end_position] : work) {
                    if (end_position > consumed) {
                        busy |= Bit(kind);
                    }
                }
                const auto result = pending.Query(begin, end, consumed);
                REQUIRE(!result.overflow);
                REQUIRE((result.writes & writes) == writes);
                REQUIRE((result.work & busy) == busy);
                if (exact) {
                    REQUIRE(result.writes == writes);
                    REQUIRE(result.work == busy);
                }
            }
        }
    }
}

TEST_CASE("Nx2FrontDiag: read classes, first that applies", "[video_core]") {
    PendingWrites::Pending none;
    PendingWrites::Pending hit;
    hit.writes = Bit(WriteKind::CbData);
    PendingWrites::Pending dropped;
    dropped.overflow = true;
    PendingWrites::Pending busy;
    busy.work = Bit(WorkKind::Draw);
    PendingWrites::Pending both = hit;
    both.work = Bit(WorkKind::Compute);
    REQUIRE(Classify(true, both, true) == ReadClass::Idle);
    REQUIRE(Classify(false, both, true) == ReadClass::WriteHit);
    REQUIRE(Classify(false, dropped, true) == ReadClass::WriteHit);
    REQUIRE(Classify(false, busy, true) == ReadClass::GpuPage);
    REQUIRE(Classify(false, none, true) == ReadClass::GpuPage);
    REQUIRE(Classify(false, busy, false) == ReadClass::Work);
    REQUIRE(Classify(false, none, false) == ReadClass::Free);
}

TEST_CASE("Nx2FrontDiag: inline to memory ranges", "[video_core]") {
    // Pitch: the lines, pitch apart.
    REQUIRE(InlineRange(0x1000, 16, 1, 0, true, 0, 0, 0, 0, 0) ==
            std::pair<u64, u64>{0x1000, 0x1010});
    REQUIRE(InlineRange(0x1000, 16, 4, 0x100, true, 0, 0, 0, 0, 0) ==
            std::pair<u64, u64>{0x1000, 0x1310});
    REQUIRE(InlineRange(0x1000, 0, 4, 0x100, true, 0, 0, 0, 0, 0).second == 0x1000);
    REQUIRE(InlineRange(0x1000, 16, 0, 0x100, true, 0, 0, 0, 0, 0).second == 0x1000);
    // Block linear: at least the surface Upload::State swizzles into (as it works out the
    // bytes per pixel from the width, line length, x offset and address).
    std::mt19937 rng{5};
    for (int i = 0; i < 20000; ++i) {
        const u32 width = 1 + rng() % 4096;
        const u32 height = 1 + rng() % 512;
        const u32 depth = 1 + rng() % 8;
        const u32 block_height = rng() % 6;
        const u32 block_depth = rng() % 6;
        const u32 line_length = 1 + rng() % width;
        const u32 x = rng() % 64;
        const u64 address = 0x10'0000'0000ULL + (rng() % 4096) * 16 + (rng() % 2) * 4;
        u32 shift = 4;
        for (const u64 value : {u64{width}, u64{line_length}, u64{x}, address & 0xffffffff}) {
            shift = std::min(shift, value == 0 ? 32U : static_cast<u32>(std::countr_zero(value)));
        }
        const u64 needed = CalculateSizeTiled(1U << shift, width >> shift, height, depth,
                                              block_height, block_depth);
        const auto [begin, end] =
            InlineRange(address, line_length, 1 + rng() % height, 0, false, width, height, depth,
                        block_height, block_depth);
        REQUIRE(begin == address);
        REQUIRE(end - begin >= needed);
    }
}

TEST_CASE("Nx2FrontDiag: DMA copy ranges", "[video_core]") {
    // Multi-line from pitch into pitch memory: the lines.
    const auto lines = DmaRange(0x2000, 0x400, 0x100, 8, true, true, true, false);
    REQUIRE(lines.known);
    REQUIRE(lines.begin == 0x2000);
    REQUIRE(lines.end == 0x2000 + 7 * 0x400 + 0x100);
    // From block linear memory (CopyBlockLinearToPitch): |pitch| bytes a line written back.
    const auto unswizzled = DmaRange(0x2000, 0x400, 0x100, 8, true, false, true, false);
    REQUIRE(unswizzled.known);
    REQUIRE(unswizzled.begin == 0x2000);
    REQUIRE(unswizzled.end == 0x2000 + 8 * 0x400);
    // ... a negative pitch the same (the copy writes forward from offset_out).
    const auto backwards = DmaRange(0x2000, 0xfffffc00U, 0x100, 8, true, false, true, false);
    REQUIRE(backwards.known);
    REQUIRE(backwards.end == 0x2000 + 8 * 0x400);
    // ... a remap: lines of up to 16 bytes an element, wider than the pitch here.
    const auto remapped = DmaRange(0x2000, 0x400, 0x100, 8, true, false, true, true);
    REQUIRE(remapped.end == 0x2000 + 7 * 0x400 + 0x100 * 16);
    // The copy's writes, modelled: whole lines and each line's elements.
    std::mt19937 model_rng{11};
    for (int i = 0; i < 20000; ++i) {
        const u32 magnitude = 16 * (1 + model_rng() % 256);
        const u32 pitch = (model_rng() & 1) != 0 ? magnitude : 0U - magnitude;
        const u32 length = 1 + model_rng() % 512;
        const u32 count = 1 + model_rng() % 64;
        const bool remap = (model_rng() & 1) != 0;
        const u32 bpp = remap ? 1 + model_rng() % 16 : 1;
        const auto copy = DmaRange(0x10'0000, pitch, length, count, true, false, true, remap);
        REQUIRE(copy.known);
        REQUIRE(copy.begin == 0x10'0000);
        REQUIRE(copy.end >= 0x10'0000 + u64{magnitude} * count);
        REQUIRE(copy.end >= 0x10'0000 + u64{magnitude} * (count - 1) + u64{length} * bpp);
    }
    // From pitch memory with a negative pitch, or into block linear memory: not known.
    REQUIRE(!DmaRange(0x2000, 0xfffffc00U, 0x100, 8, true, true, true, false).known);
    REQUIRE(!DmaRange(0x2000, 0x400, 0x100, 8, true, true, false, false).known);
    REQUIRE(!DmaRange(0x2000, 0x400, 0x100, 8, true, false, false, false).known);
    // Single line: the bytes in whole 512-byte blocks (block linear swizzles within them).
    std::mt19937 rng{3};
    for (int i = 0; i < 20000; ++i) {
        const u64 offset = 0x40'0000ULL + (rng() % 8192) * 16;
        const u32 length = 16 * (1 + rng() % 128);
        const auto copy = DmaRange(offset, 0, length, 1, false, false, false, false);
        REQUIRE(copy.known);
        REQUIRE(copy.begin <= offset);
        REQUIRE(copy.end >= offset + length);
        for (u32 chunk = 0; chunk < length; chunk += 16) {
            const u64 target = LinearToBlockLinear(offset + chunk);
            REQUIRE(target >= copy.begin);
            REQUIRE(target + 16 <= copy.end);
        }
    }
    // A remap writes words.
    const auto fill = DmaRange(0x3000, 0, 0x100, 1, false, true, true, true);
    REQUIRE(fill.begin <= 0x3000);
    REQUIRE(fill.end >= 0x3000 + 0x100 * 4);
    REQUIRE(DmaRange(0x3000, 0, 0, 1, false, true, true, false).end ==
            DmaRange(0x3000, 0, 0, 1, false, true, true, false).begin);
}

TEST_CASE("Nx2FrontDiag: sampling rate, also for periodic callers", "[video_core]") {
    Sampler sampler;
    u64 sampled = 0;
    constexpr u64 Calls = 1 << 20;
    for (u64 i = 0; i < Calls; ++i) {
        sampled += sampler.Next(3) ? 1 : 0;
    }
    REQUIRE(sampled > Calls / 8 * 9 / 10);
    REQUIRE(sampled < Calls / 8 * 11 / 10);
    // Eight macros called round robin: each gets about its share.
    Sampler round_robin;
    std::array<u64, 8> per{};
    for (u64 i = 0; i < Calls; ++i) {
        per[i % 8] += round_robin.Next(3) ? 1 : 0;
    }
    for (const u64 count : per) {
        REQUIRE(count > Calls / 64 * 8 / 10);
        REQUIRE(count < Calls / 64 * 12 / 10);
    }
    Sampler sixteenth;
    u64 sampled16 = 0;
    for (u64 i = 0; i < Calls; ++i) {
        sampled16 += sixteenth.Next(4) ? 1 : 0;
    }
    REQUIRE(sampled16 > Calls / 16 * 9 / 10);
    REQUIRE(sampled16 < Calls / 16 * 11 / 10);
}

TEST_CASE("Nx2FrontDiag: macro table", "[video_core]") {
    MacroTable table;
    auto& a = table.Find(0x120);
    a.calls = 100;
    a.sampled = 10;
    a.ticks = 1000; // 100 per call: 10,000
    auto& b = table.Find(0x340);
    b.calls = 10;
    b.sampled = 10;
    b.ticks = 50000; // 5,000 per call: 50,000
    REQUIRE(&table.Find(0x120) == &a);
    REQUIRE(&table.Find(0x340) == &b);
    REQUIRE(table.Used() == 2);
    REQUIRE(a.EstimatedTicks() == 10000.0);
    auto& c = table.Find(0x777);
    c.calls = 5; // Never sampled: no time, after the timed ones
    const auto top = table.Top(16);
    REQUIRE(top.size() == 3);
    REQUIRE(top[0]->start == 0x340);
    REQUIRE(top[1]->start == 0x120);
    REQUIRE(top[2]->start == 0x777);
    REQUIRE(table.Top(1).size() == 1);
    // Full: the rest share one entry.
    MacroTable full;
    for (u32 start = 0; start < MacroTable::Slots; ++start) {
        full.Find(start * 7).calls = 1;
    }
    auto& rest = full.Find(0xffff0);
    REQUIRE(rest.start == ~0U);
    REQUIRE(&full.Find(0xffff4) == &rest);
    for (u32 start = 0; start < MacroTable::Slots; ++start) {
        REQUIRE(full.Find(start * 7).start == start * 7);
    }
    full.Clear();
    REQUIRE(full.Used() == 0);
    REQUIRE(full.Top(4).empty());
}

TEST_CASE("Nx2FrontDiag: register writes by method", "[video_core]") {
    RegisterWrites writes;
    for (int i = 0; i < 10; ++i) {
        writes.Note(0x200, true, true);
    }
    writes.Note(0x200, false, false);
    for (int i = 0; i < 4; ++i) {
        writes.Note(0x8e4, true, false);
    }
    writes.Note(0x300, false, false);
    writes.Note(0xE00, true, true); // A macro method: not a register
    const auto top = writes.Top(16);
    REQUIRE(top.size() == 2);
    REQUIRE(top[0].method == 0x200);
    REQUIRE(top[0].total == 11);
    REQUIRE(top[0].same == 10);
    REQUIRE(top[0].skipped == 10);
    REQUIRE(top[1].method == 0x8e4);
    REQUIRE(top[1].same == 4);
    REQUIRE(top[1].skipped == 0);
    const auto totals = writes.Totals();
    REQUIRE(totals[0] == 16);
    REQUIRE(totals[1] == 14);
    REQUIRE(totals[2] == 10);
    REQUIRE(writes.Top(1).size() == 1);
    writes.Clear();
    REQUIRE(writes.Totals()[0] == 0);
}

TEST_CASE("Nx2FrontDiag: drain wait bands", "[video_core]") {
    DrainHistogram histogram;
    histogram.Note(false, 123456); // No wait: a call only
    histogram.Note(true, 1000);    // 1 us at 1 ns a tick
    histogram.Note(true, 5000);    // 4096..8191: ~6 us
    histogram.Note(true, 100'000); // ~98 us
    histogram.Note(true, 3'000'000);
    histogram.Note(true, 20'000'000);
    histogram.Note(true, 0);
    REQUIRE(histogram.Calls() == 7);
    const auto bands = histogram.ToBands(1.0);
    REQUIRE(bands[0] == 2); // 0 and 1 us
    REQUIRE(bands[1] == 1); // 6 us
    REQUIRE(bands[2] == 0);
    REQUIRE(bands[3] == 1); // 98 us
    REQUIRE(bands[4] == 0);
    REQUIRE(bands[5] == 0);
    REQUIRE(bands[6] == 1); // 3 ms
    REQUIRE(bands[7] == 1); // 20 ms
    // A faster counter: the same ticks are shorter.
    const auto quarter = histogram.ToBands(0.25);
    REQUIRE(quarter[0] == 3);
    REQUIRE(quarter[3] == 0);
    REQUIRE(quarter[2] == 1);
}

TEST_CASE("Nx2FrontDiag: first writers of pages", "[video_core]") {
    FirstWriters first{1 << 12};
    REQUIRE(first.Of(0x5000) == 0);
    first.Note(WriteKind::Inline, 0x5000, 0x7004);
    first.Note(WriteKind::CbData, 0x6000, 0x6004);
    REQUIRE(first.Of(0x5fff) == static_cast<u8>(WriteKind::Inline) + 1);
    REQUIRE(first.Of(0x6000) == static_cast<u8>(WriteKind::Inline) + 1);
    REQUIRE(first.Of(0x7000) == static_cast<u8>(WriteKind::Inline) + 1);
    REQUIRE(first.Of(0x8000) == 0);
    first.Note(WriteKind::CbData, 0x8000, 0x8000);
    REQUIRE(first.Of(0x8000) == 0);
    first.Note(WriteKind::Release, 0x8000, 0x8004);
    REQUIRE(first.Of(0x8000) == static_cast<u8>(WriteKind::Release) + 1);
    // Past the pages kept: nothing.
    first.Note(WriteKind::Dma, u64{1} << 40, (u64{1} << 40) + 4);
    REQUIRE(first.Of(u64{1} << 40) == 0);
    first.Clear();
    REQUIRE(first.Of(0x5000) == 0);
}
