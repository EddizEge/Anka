// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
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
    writer.Head().instance_count = 3;
    writer.Head().is_indexed = true;
    writer.Head().any_upload = true;
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
    REQUIRE(head.instance_count == 3);
    REQUIRE(head.is_indexed);
    REQUIRE(head.any_upload);
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
