// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <deque>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/range_sets.h"
#include "common/range_sets.inc"
#include "video_core/buffer_cache/held_cpu_writes.h"

namespace {
using Held = VideoCommon::HeldCpuWrites<0x1000>;
using Ranges = std::deque<Common::RangeSet<DAddr>>;

std::vector<std::pair<DAddr, DAddr>> Collect(const Common::RangeSet<DAddr>& set) {
    std::vector<std::pair<DAddr, DAddr>> out;
    set.ForEach([&](DAddr begin, DAddr end) { out.emplace_back(begin, end); });
    return out;
}

using V = std::vector<std::pair<DAddr, DAddr>>;
} // namespace

TEST_CASE("HeldCpuWrites: a held write after the download keeps its page from the pop",
          "[video_core]") {
    Held held;
    Ranges pending(1);
    Ranges superseded(1);
    pending[0].Add(0x10000, 0x3000); // download taken: pages 0x10000-0x12fff
    superseded[0].Add(0x11000, 0x1000);
    held.OnHeldWrite(0x11010, 4, pending, superseded); // CPU writes 4 bytes in page 0x11000
    REQUIRE(Collect(pending[0]) == V{{0x10000, 0x11000}, {0x12000, 0x13000}});
    REQUIRE(superseded[0].Empty());
}

TEST_CASE("HeldCpuWrites: a download taken before the upload skips the held pages",
          "[video_core]") {
    Held held;
    Ranges pending;
    Ranges superseded;
    held.OnHeldWrite(0x20ff0, 0x20, pending, superseded); // spans pages 0x20000 and 0x21000
    Common::RangeSet<DAddr> commit;
    commit.Add(0x1f000, 0x4000);
    held.OnCommit(commit, 0x1f000, 0x4000);
    REQUIRE(Collect(commit) == V{{0x1f000, 0x20000}, {0x22000, 0x23000}});
}

TEST_CASE("HeldCpuWrites: after the upload downloads write the page again and the epoch moves",
          "[video_core]") {
    Held held;
    Ranges pending;
    Ranges superseded;
    held.OnHeldWrite(0x30100, 8, pending, superseded);
    const u32 before = VideoCommon::g_held_cpu_write_epoch.load();
    held.OnUploaded(0x30100, 8, [](DAddr, u64) { return true; });
    REQUIRE(VideoCommon::g_held_cpu_write_epoch.load() != before);
    REQUIRE(held.pages.Empty());
    Common::RangeSet<DAddr> commit;
    commit.Add(0x30000, 0x1000);
    held.OnCommit(commit, 0x30000, 0x1000);
    REQUIRE(Collect(commit) == V{{0x30000, 0x31000}});
    // An upload of an unrelated range does not move the epoch (no short circuit lost).
    held.OnHeldWrite(0x40000, 4, pending, superseded);
    const u32 mid = VideoCommon::g_held_cpu_write_epoch.load();
    held.OnUploaded(0x50000, 4, [](DAddr, u64) { return true; });
    REQUIRE(VideoCommon::g_held_cpu_write_epoch.load() == mid);
    // Uploaded but not downloadable: the page leaves the set, the epoch stays.
    held.OnUploaded(0x40000, 4, [](DAddr, u64) { return false; });
    REQUIRE(held.pages.Empty());
    REQUIRE(VideoCommon::g_held_cpu_write_epoch.load() == mid);
}

TEST_CASE("HeldCpuWrites: held write vs pop order, either way the CPU bytes win",
          "[video_core]") {
    // Guest memory model: one page; GPU download carries 0xAA, the CPU writes 0x55 at offset 16.
    for (const bool pop_first : {true, false}) {
        std::vector<u8> memory(0x1000, 0);
        Held held;
        Ranges pending(1);
        Ranges superseded(1);
        pending[0].Add(0x60000, 0x1000);
        const auto pop = [&] {
            pending[0].ForEach([&](DAddr begin, DAddr end) {
                for (DAddr a = begin; a < end; ++a) {
                    memory[a - 0x60000] = 0xAA;
                }
            });
            pending.pop_front();
            superseded.pop_front();
        };
        const auto cpu_write = [&] {
            held.OnHeldWrite(0x60010, 1, pending, superseded); // under the cache lock
            memory[0x10] = 0x55;                               // then the bytes land
        };
        if (pop_first) {
            pop();
            cpu_write();
        } else {
            cpu_write();
            pop();
        }
        REQUIRE(memory[0x10] == 0x55);
    }
}
