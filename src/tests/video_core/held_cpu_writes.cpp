// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/range_sets.h"
#include "common/range_sets.inc"
#include "core/gpu_dirty_memory_manager.h"
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

constexpr auto NoRequeue = [](DAddr, u64) {};

/// WriteAroundPending's pieces and the bytes it left out.
struct Around {
    V written;
    u64 skipped;
};
Around WriteAround(DAddr addr, u64 size, const V& pending) {
    Around out{};
    out.skipped = VideoCommon::WriteAroundPending(
        addr, size,
        [&](DAddr, u64, auto&& visit) {
            for (const auto& [begin, end] : pending) {
                visit(begin, end - begin);
            }
        },
        [&](DAddr begin, DAddr end) { out.written.emplace_back(begin, end); });
    return out;
}

/// The collector's pending writes touching [begin, end), as 64-byte runs.
V Pending(Core::GPUDirtyMemoryManager& collector, DAddr begin, DAddr end) {
    V out;
    collector.ForEachPending(begin, end,
                             [&](DAddr addr, size_t size) { out.emplace_back(addr, addr + size); });
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace

TEST_CASE("HeldCpuWrites: a held write after the download keeps its page from the pop",
          "[video_core]") {
    Held held;
    Ranges pending(1);
    Ranges superseded(1);
    pending[0].Add(0x10000, 0x3000); // download taken: pages 0x10000-0x12fff
    superseded[0].Add(0x11000, 0x1000);
    held.OnHeldWrite(0x11010, 4, pending, superseded, NoRequeue); // CPU writes 4 bytes in 0x11000
    REQUIRE(Collect(pending[0]) == V{{0x10000, 0x11000}, {0x12000, 0x13000}});
    REQUIRE(superseded[0].Empty());
}

TEST_CASE("HeldCpuWrites: a download taken before the upload skips the held pages",
          "[video_core]") {
    Held held;
    Ranges pending;
    Ranges superseded;
    held.OnHeldWrite(0x20ff0, 0x20, pending, superseded, NoRequeue); // pages 0x20000 and 0x21000
    Common::RangeSet<DAddr> commit;
    commit.Add(0x1f000, 0x4000);
    held.OnCommit(commit, 0x1f000, 0x4000, NoRequeue);
    REQUIRE(Collect(commit) == V{{0x1f000, 0x20000}, {0x22000, 0x23000}});
}

TEST_CASE("HeldCpuWrites: after the upload downloads write the page again and the epoch moves",
          "[video_core]") {
    Held held;
    Ranges pending;
    Ranges superseded;
    held.OnHeldWrite(0x30100, 8, pending, superseded, NoRequeue);
    const u32 before = VideoCommon::g_held_cpu_write_epoch.load();
    held.OnUploaded(0x30100, 8, [](DAddr, u64) { return true; });
    REQUIRE(VideoCommon::g_held_cpu_write_epoch.load() != before);
    REQUIRE(held.pages.Empty());
    Common::RangeSet<DAddr> commit;
    commit.Add(0x30000, 0x1000);
    held.OnCommit(commit, 0x30000, 0x1000, NoRequeue);
    REQUIRE(Collect(commit) == V{{0x30000, 0x31000}});
    // An upload of an unrelated range does not move the epoch (no short circuit lost).
    held.OnHeldWrite(0x40000, 4, pending, superseded, NoRequeue);
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
            held.OnHeldWrite(0x60010, 1, pending, superseded, NoRequeue); // under the cache lock
            memory[0x10] = 0x55;                                          // then the bytes land
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

TEST_CASE("HeldCpuWrites: what a held write or a commit leaves out is queued again (B2)",
          "[video_core]") {
    Held held;
    Ranges pending(2);
    Ranges superseded(2);
    pending[0].Add(0x70800, 0x1000); // the second half of page 0x70000, the first of 0x71000
    pending[1].Add(0x70000, 0x400);
    V requeued;
    const auto requeue = [&](DAddr begin, u64 length) {
        requeued.emplace_back(begin, begin + length);
    };
    held.OnHeldWrite(0x70020, 4, pending, superseded, requeue);
    std::sort(requeued.begin(), requeued.end());
    REQUIRE(requeued == V{{0x70000, 0x70400}, {0x70800, 0x71000}});
    REQUIRE(Collect(pending[0]) == V{{0x71000, 0x71800}});
    REQUIRE(pending[1].Empty());
    requeued.clear();
    Common::RangeSet<DAddr> commit;
    commit.Add(0x6f000, 0x3000);
    held.OnCommit(commit, 0x6f000, 0x3000, requeue);
    REQUIRE(requeued == V{{0x70000, 0x71000}});
    REQUIRE(Collect(commit) == V{{0x6f000, 0x70000}, {0x71000, 0x72000}});
}

TEST_CASE("WriteAroundPending: writes all but the pending bytes, in order", "[video_core]") {
    // Nothing pending: one piece.
    auto r = WriteAround(0x1000, 0x100, {});
    REQUIRE(r.written == V{{0x1000, 0x1100}});
    REQUIRE(r.skipped == 0);
    // Pending outside the range, or touching only its ends.
    r = WriteAround(0x1000, 0x100, {{0x800, 0x1000}, {0x1100, 0x1200}});
    REQUIRE(r.written == V{{0x1000, 0x1100}});
    REQUIRE(r.skipped == 0);
    // Unsorted, overlapping, clipped at the start.
    r = WriteAround(0x1000, 0x100, {{0x1080, 0x10c0}, {0xfc0, 0x1040}, {0x10a0, 0x10e0}});
    REQUIRE(r.written == V{{0x1040, 0x1080}, {0x10e0, 0x1100}});
    REQUIRE(r.skipped == 0x40 + 0x60);
    // Clipped at the end.
    r = WriteAround(0x1000, 0x100, {{0x10c0, 0x1200}});
    REQUIRE(r.written == V{{0x1000, 0x10c0}});
    REQUIRE(r.skipped == 0x40);
    // All of it pending: nothing written.
    r = WriteAround(0x1000, 0x100, {{0x1000, 0x1100}});
    REQUIRE(r.written.empty());
    REQUIRE(r.skipped == 0x100);
}

TEST_CASE("GPUDirtyMemoryManager::ForEachPending sees collected writes without consuming them",
          "[video_core]") {
    Core::GPUDirtyMemoryManager collector;
    REQUIRE(Pending(collector, 0, ~DAddr{0}).empty());
    collector.Collect(0x80010, 4);    // 64-byte run 0x80000
    collector.Collect(0x80050, 0x40); // runs 0x80040 and 0x80080
    collector.Collect(0x90800, 8);    // another page: the first goes to the back buffer
    REQUIRE(Pending(collector, 0x80000, 0x81000) == V{{0x80000, 0x800c0}});
    REQUIRE(Pending(collector, 0x80040, 0x80080) == V{{0x80000, 0x800c0}});
    REQUIRE(Pending(collector, 0x90000, 0x91000) == V{{0x90800, 0x90840}});
    REQUIRE(Pending(collector, 0xa0000, 0xa1000).empty());
    // Gather hands out the same writes; while it does, they still count as pending.
    V gathered;
    V pending_during;
    std::function<void(PAddr, size_t)> callback = [&](PAddr addr, size_t size) {
        gathered.emplace_back(addr, addr + size);
        if (pending_during.empty()) {
            pending_during = Pending(collector, 0x80000, 0x91000);
        }
    };
    collector.Gather(callback);
    std::sort(gathered.begin(), gathered.end());
    REQUIRE(gathered == V{{0x80000, 0x800c0}, {0x90800, 0x90840}});
    REQUIRE(pending_during == V{{0x80000, 0x800c0}, {0x90800, 0x90840}});
    // Handed out: no longer pending.
    REQUIRE(Pending(collector, 0, ~DAddr{0}).empty());
}

namespace {
/// Guest memory, a buffer of the GPU and one core's collector, for the synchronous download
/// orders (K1b). One page at 0xb0000: the GPU wrote 0xAA over all of it (not downloaded yet).
struct SyncModel {
    static constexpr DAddr base = 0xb0000;
    std::vector<u8> guest = std::vector<u8>(0x1000, 0x00);
    std::vector<u8> gpu = std::vector<u8>(0x1000, 0xAA);
    Core::GPUDirtyMemoryManager collector;

    /// A held CPU write: collected, then its bytes land in guest memory (HandleRasterizerWrite,
    /// then the copy).
    void CpuWrite(DAddr addr, u64 size, u8 value) {
        collector.Collect(addr, size);
        std::fill_n(guest.begin() + (addr - base), size, value);
    }
    /// The synchronous download's write of `copy` (the buffer read before) to guest memory.
    void WriteBack(const std::vector<u8>& copy) {
        VideoCommon::WriteAroundPending(
            base, copy.size(),
            [&](DAddr begin, u64 length, auto&& visit) {
                collector.ForEachPending(begin, begin + length, visit);
            },
            [&](DAddr begin, DAddr end) {
                std::copy(copy.begin() + (begin - base), copy.begin() + (end - base),
                          guest.begin() + (begin - base));
            });
    }
    /// InvalidateGPUCache: the collected writes' 64-byte runs from guest memory to the buffer.
    void Upload() {
        std::function<void(PAddr, size_t)> callback = [&](PAddr addr, size_t size) {
            std::copy_n(guest.begin() + (addr - base), size, gpu.begin() + (addr - base));
        };
        collector.Gather(callback);
    }
};
} // namespace

TEST_CASE("K1b: a held write, then a synchronous download: the CPU bytes stay, the rest arrive",
          "[video_core]") {
    SyncModel m;
    m.CpuWrite(SyncModel::base + 0x104, 4, 0x55); // run 0x100-0x13f
    m.WriteBack(m.gpu);
    REQUIRE(m.guest[0x104] == 0x55);
    REQUIRE(m.guest[0x107] == 0x55);
    REQUIRE(m.guest[0x0ff] == 0xAA); // the GPU's bytes next to it: the caller reads them now
    REQUIRE(m.guest[0x140] == 0xAA);
    // The 64-byte run's other bytes keep guest memory's: the upload copies the whole run, so
    // after it the buffer holds them too (as when the upload came first).
    REQUIRE(m.guest[0x100] == 0x00);
    m.Upload();
    REQUIRE(m.gpu == m.guest);
}

TEST_CASE("K1b: a write collected while the download waits (Finish) stays too", "[video_core]") {
    SyncModel m;
    const std::vector<u8> copy = m.gpu;           // the copy is recorded and runs on the GPU
    m.CpuWrite(SyncModel::base + 0x800, 8, 0x77); // a short-circuited write meanwhile
    m.WriteBack(copy);                            // after Finish
    REQUIRE(m.guest[0x800] == 0x77);
    REQUIRE(m.guest[0x7ff] == 0xAA);
    m.Upload();
    REQUIRE(m.gpu == m.guest);
}

TEST_CASE("K1b: a write uploaded before the download is in the copy", "[video_core]") {
    SyncModel m;
    m.CpuWrite(SyncModel::base + 0x200, 4, 0x33);
    m.Upload(); // InvalidateGPUCache first: nothing pending, the copy carries the CPU bytes
    m.WriteBack(m.gpu);
    REQUIRE(m.guest[0x200] == 0x33);
    REQUIRE(m.guest[0x1ff] == 0xAA);
}
