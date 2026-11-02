// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_stage_draw.h"

// Back stages, cut c: the draw packet B writes to ring 3 and C reads (nx2_stage_draw.h).
namespace {
using namespace VideoCommon::Nx2Stages;

/// A descriptor entry's words, made from `tag`.
std::array<u64, DrawEntryWords> Entry(u64 tag) {
    return {tag, tag + 0x100, tag + 0x200};
}

std::vector<u64> Write(const StageDrawWriter& writer) {
    std::vector<u64> words(writer.Words() + 1, 0xdeadbeefdeadbeefULL);
    writer.WriteTo(words.data());
    REQUIRE(words.back() == 0xdeadbeefdeadbeefULL); // Wrote exactly Words()
    words.pop_back();
    return words;
}
} // Anonymous namespace

TEST_CASE("Nx2StageDraw: an empty draw is its head", "[video_core]") {
    StageDrawWriter writer;
    writer.Begin();
    writer.Head().seq = 1ULL << 40;
    writer.Head().kind = DrawKind::LightPlain;
    REQUIRE(writer.Words() == DrawHeadWords);
    const std::vector<u64> words = Write(writer);
    const StageDrawReader reader{words.data(), static_cast<u32>(words.size())};
    REQUIRE(reader.Head().seq == 1ULL << 40);
    REQUIRE(reader.Head().kind == DrawKind::LightPlain);
    REQUIRE(reader.EntryCount() == 0);
    for (u32 stage = 0; stage < DrawStages; ++stage) {
        REQUIRE(reader.StageEntryCount(stage) == 0);
    }
    REQUIRE(reader.Handles().empty());
    REQUIRE(reader.Geometry().empty());
    REQUIRE(reader.DrawWords().empty());
}

TEST_CASE("Nx2StageDraw: the tail round-trips in its order", "[video_core]") {
    StageDrawWriter writer;
    writer.Begin();
    writer.Head().seq = 77;
    writer.Head().is_indexed = true;
    writer.Head().light.epoch = 9;
    // Stage 0 with two entries, stage 2 none, stage 4 with three: stages may be skipped.
    const std::array<std::array<u64, DrawEntryWords>, 2> vertex{Entry(10), Entry(20)};
    const std::array<std::array<u64, DrawEntryWords>, 3> fragment{Entry(30), Entry(40), Entry(50)};
    writer.AddStageEntries(0, vertex.data(), 2);
    writer.AddStageEntries(2, nullptr, 0);
    writer.AddStageEntries(4, fragment.data(), 3);
    // An odd count of handles: the last word's high half stays zero.
    for (u32 handle : {5U, 0xfffffU, 7U}) {
        writer.AddHandle(handle);
    }
    const GeometrySlot slot{
        .slot = 32, .index_format = 2, .reserved = 0, .stride = 0, .buffer = 0x1234,
        .offset = 64, .size = 4096};
    writer.AddGeometry(slot);
    const std::array<u64, 2> draw{0xaa, 0xbb};
    writer.AddDrawWords(draw.data(), 2);
    REQUIRE(writer.EntryCount() == 5);
    REQUIRE(writer.Words() == DrawHeadWords + 5 * DrawEntryWords + 2 + GeometrySlotWords + 2);

    const std::vector<u64> words = Write(writer);
    // The handles' last word: 7, then a zero high half.
    REQUIRE(words[DrawHeadWords + 5 * DrawEntryWords + 1] == 7);

    const StageDrawReader reader{words.data(), static_cast<u32>(words.size())};
    const DrawToC& head = reader.Head();
    REQUIRE(head.seq == 77);
    REQUIRE(head.is_indexed);
    REQUIRE(head.light.epoch == 9);
    REQUIRE(head.buffer_entries == std::array<u8, DrawStages>{2, 0, 0, 0, 3});
    REQUIRE(head.handles == 3);
    REQUIRE(head.geometry_slots == 1);
    REQUIRE(head.draw_words == 2);
    REQUIRE(reader.EntryCount() == 5);
    REQUIRE(reader.StageEntryCount(0) == 2);
    REQUIRE(reader.StageEntryCount(1) == 0);
    REQUIRE(reader.StageEntryCount(4) == 3);
    REQUIRE(std::memcmp(reader.StageEntries(0), vertex.data(), sizeof(vertex)) == 0);
    REQUIRE(std::memcmp(reader.StageEntries(4), fragment.data(), sizeof(fragment)) == 0);
    // An empty stage's entries start where the next one's do.
    REQUIRE(reader.StageEntries(2) == reader.StageEntries(4));
    REQUIRE(reader.Handles().size() == 3);
    REQUIRE(reader.Handles()[0] == 5);
    REQUIRE(reader.Handles()[1] == 0xfffff);
    REQUIRE(reader.Handles()[2] == 7);
    REQUIRE(reader.Geometry().size() == 1);
    REQUIRE(reader.Geometry()[0].slot == 32);
    REQUIRE(reader.Geometry()[0].buffer == 0x1234);
    REQUIRE(reader.Geometry()[0].size == 4096);
    REQUIRE(reader.DrawWords().size() == 2);
    REQUIRE(reader.DrawWords()[1] == 0xbb);
}

TEST_CASE("Nx2StageDraw: the writer starts over", "[video_core]") {
    StageDrawWriter writer;
    writer.Begin();
    const auto first = Entry(1);
    writer.AddStageEntries(1, first.data(), 1);
    writer.AddHandle(1);
    writer.AddHandle(2);
    const std::array<u64, 1> draw{3};
    writer.AddDrawWords(draw.data(), 1);
    // A setup that gave up (ConfigureUniformsBuffers): its entries and handles go, the draw's
    // own parts stay, and the stages start over.
    writer.ClearSetup();
    REQUIRE(writer.EntryCount() == 0);
    REQUIRE(writer.Handles().empty());
    const auto second = Entry(2);
    writer.AddStageEntries(0, second.data(), 1);
    REQUIRE(writer.Words() == DrawHeadWords + DrawEntryWords + 1);
    std::vector<u64> words = Write(writer);
    StageDrawReader reader{words.data(), static_cast<u32>(words.size())};
    REQUIRE(reader.StageEntryCount(0) == 1);
    REQUIRE(reader.StageEntryCount(1) == 0);
    REQUIRE(reader.StageEntries(0)[0] == 2);
    REQUIRE(reader.DrawWords()[0] == 3);

    // Begin empties everything, the head too.
    writer.Head().seq = 5;
    writer.Begin();
    REQUIRE(writer.Head().seq == 0);
    REQUIRE(writer.Words() == DrawHeadWords);
    words = Write(writer);
    reader = StageDrawReader{words.data(), static_cast<u32>(words.size())};
    REQUIRE(reader.EntryCount() == 0);
    REQUIRE(reader.DrawWords().empty());
}

TEST_CASE("Nx2StageDraw: a repair reads other handles", "[video_core]") {
    StageDrawWriter writer;
    writer.Begin();
    const auto entry = Entry(4);
    writer.AddStageEntries(3, entry.data(), 1);
    const std::vector<u64> words = Write(writer);
    const StageDrawReader reader{words.data(), static_cast<u32>(words.size())};
    REQUIRE(reader.Handles().empty());
    // A light draw's packet holds no handles; C repairs with the anchor's.
    const std::array<u32, 4> anchor{8, 9, 10, 11};
    const StageDrawReader repair = reader.WithHandles(anchor);
    REQUIRE(repair.Handles().size() == 4);
    REQUIRE(repair.Handles()[3] == 11);
    REQUIRE(repair.StageEntryCount(3) == 1);
    REQUIRE(repair.StageEntries(3)[2] == entry[2]);
    REQUIRE(reader.Handles().empty());
}

TEST_CASE("Nx2StageDraw: the head is six words", "[video_core]") {
    // What C reads (k2hiz): the seq, pipeline, records, B's light epoch, the counts, the stages'
    // entries, kind and is_indexed.
    static_assert(sizeof(DrawToC) == 6 * sizeof(u64));
    REQUIRE(DrawHeadWords == 6);
}

TEST_CASE("Nx2StageDraw: draw arguments round-trip packed by their command", "[video_core]") {
    DrawArgs direct{};
    direct.command = DrawCommand::Direct;
    direct.is_indexed = true;
    direct.topology = 0x0d;
    direct.base_instance = 0xfffffffe;
    direct.num_instances = 1;
    direct.base_vertex = 0x80000001; // a negative base vertex, as the u32 it is
    direct.num_vertices = 0x12345;
    direct.first_index = 0xabcdef01;
    DrawArgs indirect{};
    indirect.command = DrawCommand::Indirect;
    indirect.indirect_buffer = 0x1122334455667788ULL;
    indirect.indirect_offset = 0x100000040ULL; // past 4 GiB: kept whole
    indirect.max_draw_counts = 77;
    indirect.stride = 20;
    indirect.topology = 4;
    DrawArgs count = indirect;
    count.command = DrawCommand::IndirectCount;
    count.is_indexed = true;
    count.count_buffer = 0x99;
    count.count_offset = 0x10;
    DrawArgs bytes{};
    bytes.command = DrawCommand::ByteCount;
    bytes.indirect_buffer = 5;
    bytes.indirect_offset = 6;
    bytes.stride = 12;
    DrawArgs none{};
    none.topology = 3;
    const std::array<std::pair<DrawArgs, u32>, 5> cases{{
        {direct, 3}, {indirect, 4}, {count, 6}, {bytes, 4}, {none, 1}}};
    for (const auto& [args, words] : cases) {
        std::array<u64, MaxDrawArgWords + 1> packed;
        packed.fill(0x5a5a5a5a5a5a5a5aULL);
        REQUIRE(PackDrawArgs(args, packed.data()) == words);
        REQUIRE(packed[words] == 0x5a5a5a5a5a5a5a5aULL); // nothing past its words
        const DrawArgs back = UnpackDrawArgs({packed.data(), words});
        REQUIRE(back.command == args.command);
        REQUIRE(back.is_indexed == args.is_indexed);
        REQUIRE(back.topology == args.topology);
        if (args.command == DrawCommand::Direct) {
            REQUIRE(back.base_instance == args.base_instance);
            REQUIRE(back.num_instances == args.num_instances);
            REQUIRE(back.base_vertex == args.base_vertex);
            REQUIRE(back.num_vertices == args.num_vertices);
            REQUIRE(back.first_index == args.first_index);
        } else if (args.command != DrawCommand::None) {
            REQUIRE(back.indirect_buffer == args.indirect_buffer);
            REQUIRE(back.indirect_offset == args.indirect_offset);
            REQUIRE(back.max_draw_counts == args.max_draw_counts);
            REQUIRE(back.stride == args.stride);
            REQUIRE(back.count_buffer ==
                    (args.command == DrawCommand::IndirectCount ? args.count_buffer : 0));
            REQUIRE(back.count_offset ==
                    (args.command == DrawCommand::IndirectCount ? args.count_offset : 0));
        }
    }
}

TEST_CASE("Nx2StageDraw: the packet's own geometry is written without a copy", "[video_core]") {
    // SetGeometry: B's runtime's PacketGeometry, read by WriteTo alone.
    std::array<GeometrySlot, 3> slots{};
    for (u8 i = 0; i < slots.size(); ++i) {
        slots[i] = GeometrySlot{.slot = i, .index_format = 0, .reserved = 0, .stride = 16u * i,
                                .buffer = 0x100u + i, .offset = 8u * i, .size = 64u + i};
    }
    StageDrawWriter writer;
    writer.Begin();
    writer.SetGeometry(slots);
    std::array<u64, MaxDrawArgWords> packed;
    DrawArgs args{};
    args.command = DrawCommand::Direct;
    args.num_vertices = 3;
    writer.AddDrawWords(packed.data(), PackDrawArgs(args, packed.data()));
    REQUIRE(writer.Words() == DrawHeadWords + 3 * GeometrySlotWords + 3);
    slots[2].offset = 0x777; // read at WriteTo, not at SetGeometry
    const std::vector<u64> words = Write(writer);
    const StageDrawReader reader{words.data(), static_cast<u32>(words.size())};
    REQUIRE(reader.Head().geometry_slots == 3);
    REQUIRE(reader.Geometry().size() == 3);
    REQUIRE(reader.Geometry()[1].buffer == 0x101);
    REQUIRE(reader.Geometry()[2].offset == 0x777);
    REQUIRE(UnpackDrawArgs(reader.DrawWords()).num_vertices == 3);
    // Begin forgets the view.
    writer.Begin();
    REQUIRE(writer.Words() == DrawHeadWords);
}
