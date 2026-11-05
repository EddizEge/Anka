// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_reactive.h"
#include "core/nx2_host_core.h"
#include "video_core/nx2_pitch_forced.h"
#include "video_core/nx2_reactive_flush.h"

namespace Reactive = Common::Nx2Reactive;

namespace {

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // Anonymous namespace

TEST_CASE("Reactive host core slot", "[video_core][nx2_reactive]") {
    constexpr std::size_t cores = Core::Hardware::NUM_CPU_CORES;
    for (std::size_t id = 0; id + 1 < cores; ++id) {
        REQUIRE(Core::HostCoreSlot(id) == id);
    }
    // The last core and every other thread (ids past the cores, up to 255) share the last slot.
    REQUIRE(Core::HostCoreSlot(cores - 1) == cores - 1);
    REQUIRE(Core::HostCoreSlot(cores) == cores - 1);
    REQUIRE(Core::HostCoreSlot(cores + 1) == cores - 1);
    REQUIRE(Core::HostCoreSlot(255) == cores - 1);
    REQUIRE(Core::HostCoreSlot(~std::size_t{0}) == cores - 1);
    static_assert(Core::HostCoreSlot(255) < cores);
}

TEST_CASE("Reactive switches", "[video_core][nx2_reactive]") {
    REQUIRE(!Reactive::ParseFlag(nullptr));
    REQUIRE(!Reactive::ParseFlag(""));
    REQUIRE(!Reactive::ParseFlag("0"));
    REQUIRE(Reactive::ParseFlag("1"));
    REQUIRE(Reactive::ParseFlag("2"));
    REQUIRE(Reactive::ParseFlag("00"));
    REQUIRE(Reactive::ParseFrames(nullptr) == 0);
    REQUIRE(Reactive::ParseFrames("") == 0);
    REQUIRE(Reactive::ParseFrames("0") == 0);
    REQUIRE(Reactive::ParseFrames("x") == 0);
    REQUIRE(Reactive::ParseFrames("12x") == 0);
    REQUIRE(Reactive::ParseFrames("1") == 1);
    REQUIRE(Reactive::ParseFrames("120") == 120);
}

TEST_CASE("Reactive counters and window", "[video_core][nx2_reactive]") {
    Reactive::State state;
    // The first call starts the first window and is never due.
    constexpr s64 t0 = 1'000'000'000;
    REQUIRE(!Reactive::TakeReportDue(state, t0, 0));
    REQUIRE(state.window_start_ns == t0);
    REQUIRE(!Reactive::TakeReportDue(state, t0 + Reactive::REPORT_PERIOD_NS - 1, 0));
    // A caller with slack waits longer than the frame's.
    REQUIRE(!Reactive::TakeReportDue(state, t0 + Reactive::REPORT_PERIOD_NS,
                                     Reactive::REPORT_PERIOD_NS));
    REQUIRE(Reactive::TakeReportDue(state, t0 + Reactive::REPORT_PERIOD_NS, 0));
    // Only one caller gets it; the next is a period later.
    REQUIRE(!Reactive::TakeReportDue(state, t0 + Reactive::REPORT_PERIOD_NS, 0));
    REQUIRE(!Reactive::TakeReportDue(state, t0 + 2 * Reactive::REPORT_PERIOD_NS - 1, 0));
    REQUIRE(Reactive::TakeReportDue(state, t0 + 3 * Reactive::REPORT_PERIOD_NS,
                                    Reactive::REPORT_PERIOD_NS));

    const auto add = [&state](Reactive::Counter counter, u64 value) {
        state.counters[static_cast<std::size_t>(counter)].fetch_add(value);
    };
    state.slow_reads[0].value = 5;
    state.slow_reads[3].value = 7;
    add(Reactive::Counter::CpuReads, 11);
    add(Reactive::Counter::SyncFlushes, 2);
    add(Reactive::Counter::SyncFlushUs, 300);
    add(Reactive::Counter::SyncFlushMaxUs, 250);
    add(Reactive::Counter::FlushAreaCalls, 12);
    add(Reactive::Counter::RtDownloadsLatched, 3);
    add(Reactive::Counter::RtDownloadBytesLatched, 3 * 1024 * 1024);
    add(Reactive::Counter::RtDownloadsForced, 1);
    add(Reactive::Counter::RtDownloadBytesForced, 1);
    add(Reactive::Counter::BufferDownloads, 4);
    add(Reactive::Counter::BufferDownloadBytes, 8192);
    add(Reactive::Counter::Frames, 600);
    state.gauges[static_cast<std::size_t>(Reactive::Gauge::ReadPages)] = 42;
    state.gauges[static_cast<std::size_t>(Reactive::Gauge::StagingDownloadBytes)] = 3 << 20;
    state.fastmem_fallbacks = 9;

    const s64 t1 = t0 + 10'500'000'000;
    state.window_start_ns = t0;
    const Reactive::Window window = Reactive::TakeWindow(state, t1);
    REQUIRE(window.seconds > 10.49);
    REQUIRE(window.seconds < 10.51);
    REQUIRE(state.window_start_ns == t1);
    REQUIRE(window.slow_reads[0] == 5);
    REQUIRE(window.slow_reads[3] == 7);
    REQUIRE(window.Get(Reactive::Counter::CpuReads) == 11);
    REQUIRE(window.Get(Reactive::Counter::Frames) == 600);
    REQUIRE(window.Get(Reactive::Gauge::ReadPages) == 42);
    REQUIRE(window.fastmem_fallbacks == 9);
    REQUIRE(window.fastmem_fallbacks_total == 9);
    // Counters start over; gauges stay as last set; the fastmem count shows the change.
    REQUIRE(state.counters[static_cast<std::size_t>(Reactive::Counter::CpuReads)] == 0);
    REQUIRE(state.slow_reads[3].value == 0);
    REQUIRE(state.gauges[static_cast<std::size_t>(Reactive::Gauge::ReadPages)] == 42);

    const std::string line = Reactive::FormatWindow(window);
    REQUIRE(Contains(line, "nx2 reactive: 10.5 s, 600 frames;"));
    REQUIRE(Contains(line, "slow reads 12 (cores 5/0/0/7);"));
    REQUIRE(Contains(line, "cpu reads 11;"));
    REQUIRE(Contains(line, "sync flushes 2 wait 300 us max 250 us;"));
    REQUIRE(Contains(line, "flush area 12 calls"));
    REQUIRE(Contains(line, "rt downloads forced 1 (1 KiB) latched 3 (3072 KiB);"));
    REQUIRE(Contains(line, "buffer copy back 4 (8 KiB);"));
    REQUIRE(Contains(line, "read pages 42;"));
    REQUIRE(Contains(line, "download 3.0 MiB;"));
    REQUIRE(Contains(line, "fastmem fallbacks 9 (total 9);"));

    state.fastmem_fallbacks += 4;
    const Reactive::Window next = Reactive::TakeWindow(state, t1 + 1'000'000'000);
    REQUIRE(next.fastmem_fallbacks == 4);
    REQUIRE(next.fastmem_fallbacks_total == 13);
    REQUIRE(next.Get(Reactive::Counter::CpuReads) == 0);
    REQUIRE(Contains(Reactive::FormatWindow(next), "slow reads 0 (cores 0);"));
}

TEST_CASE("Reactive global counters", "[video_core][nx2_reactive]") {
    Reactive::State& state = Reactive::GetState();
    const Reactive::Window before = Reactive::TakeWindow(state, 1);
    (void)before;
    // Slots past the last go to it.
    Reactive::CountSlowRead(0);
    Reactive::CountSlowRead(Reactive::SLOW_READ_SLOTS - 1);
    Reactive::CountSlowRead(Reactive::SLOW_READ_SLOTS);
    Reactive::CountSlowRead(255);
    Reactive::Add(Reactive::Counter::SyncFlushUs, 10);
    Reactive::Add(Reactive::Counter::SyncFlushUs, 15);
    Reactive::Max(Reactive::Counter::SyncFlushMaxUs, 7);
    Reactive::Max(Reactive::Counter::SyncFlushMaxUs, 30);
    Reactive::Max(Reactive::Counter::SyncFlushMaxUs, 20);
    Reactive::SetGauge(Reactive::Gauge::LatchedImages, 3);
    Reactive::CountFastmemFallback();
    // Threads adding at once lose nothing.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < 10000; ++i) {
                Reactive::Add(Reactive::Counter::CpuReads, 1);
                Reactive::CountSlowRead(static_cast<std::size_t>(t));
                Reactive::Max(Reactive::Counter::FlushAreaUs, static_cast<u64>(i));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const Reactive::Window window = Reactive::TakeWindow(state, 2);
    REQUIRE(window.slow_reads[0] == 1 + 10000);
    REQUIRE(window.slow_reads[1] == 10000);
    REQUIRE(window.slow_reads[Reactive::SLOW_READ_SLOTS - 1] == 3);
    REQUIRE(window.Get(Reactive::Counter::SyncFlushUs) == 25);
    REQUIRE(window.Get(Reactive::Counter::SyncFlushMaxUs) == 30);
    REQUIRE(window.Get(Reactive::Counter::CpuReads) == 40000);
    REQUIRE(window.Get(Reactive::Counter::FlushAreaUs) == 9999);
    REQUIRE(window.Get(Reactive::Gauge::LatchedImages) == 3);
    REQUIRE(window.fastmem_fallbacks >= 1);

    // At most MAX_IMAGE_LOGS images get their line.
    state.image_logs = 0;
    u32 taken = 0;
    for (u32 i = 0; i < 40; ++i) {
        taken += Reactive::TakeImageLog() ? 1 : 0;
    }
    REQUIRE(taken == Reactive::MAX_IMAGE_LOGS);

    REQUIRE(Reactive::MicrosecondsSince(Reactive::NowNs() + 1'000'000'000) == 0);
    REQUIRE(Reactive::MicrosecondsSince(Reactive::NowNs() - 2'000'000) >= 2000);
}

namespace {

constexpr DAddr PAGE = 4096;

bool Same(const VideoCore::RasterizerDownloadArea& a, const VideoCore::RasterizerDownloadArea& b) {
    return a.start_address == b.start_address && a.end_address == b.end_address &&
           a.preemtive == b.preemtive;
}

/// RasterizerVulkan::GetFlushArea's answer before EDEN_NX2_FLUSH_FIRST_READ.
VideoCore::RasterizerDownloadArea OldFlushArea(
    const std::optional<VideoCore::RasterizerDownloadArea>& texture_area, DAddr addr, u64 size) {
    if (texture_area) {
        return *texture_area;
    }
    return VideoCore::RasterizerDownloadArea{
        .start_address = addr / PAGE * PAGE,
        .end_address = (addr + size + PAGE - 1) / PAGE * PAGE,
        .preemtive = true,
    };
}

} // Anonymous namespace

TEST_CASE("Reactive first read decision", "[video_core][nx2_reactive]") {
    namespace Flush = VideoCommon::Nx2Reactive;
    for (int bits = 0; bits < 8; ++bits) {
        const bool enabled = (bits & 1) != 0;
        const bool newly = (bits & 2) != 0;
        const bool modified = (bits & 4) != 0;
        REQUIRE(Flush::FirstReadFlushes(enabled, newly, modified) == (bits == 7));
    }
    static_assert(Flush::FirstReadFlushes(true, true, true));
    static_assert(!Flush::FirstReadFlushes(false, true, true));
}

TEST_CASE("Reactive first read area", "[video_core][nx2_reactive]") {
    namespace Flush = VideoCommon::Nx2Reactive;
    // Hand cases.
    {
        const auto area = Flush::CombineFlushArea(std::nullopt, 0x10000, 0x12000, false);
        REQUIRE(area.start_address == 0x10000);
        REQUIRE(area.end_address == 0x12000);
        REQUIRE(area.preemtive);
    }
    {
        const auto area = Flush::CombineFlushArea(std::nullopt, 0x10000, 0x12000, true);
        REQUIRE(area.start_address == 0x10000);
        REQUIRE(area.end_address == 0x12000);
        REQUIRE(!area.preemtive);
    }
    {
        // A preemptive texture area (its images forced already) wider than the pages
        const VideoCore::RasterizerDownloadArea texture{0x8000, 0x40000, true};
        REQUIRE(Same(Flush::CombineFlushArea(texture, 0x10000, 0x12000, false), texture));
        const auto area = Flush::CombineFlushArea(texture, 0x10000, 0x12000, true);
        REQUIRE(area.start_address == 0x8000);
        REQUIRE(area.end_address == 0x40000);
        REQUIRE(!area.preemtive);
    }
    {
        // A texture area next to the pages: the flush covers both
        const VideoCore::RasterizerDownloadArea texture{0x20000, 0x30000, true};
        const auto area = Flush::CombineFlushArea(texture, 0x10000, 0x12000, true);
        REQUIRE(area.start_address == 0x10000);
        REQUIRE(area.end_address == 0x30000);
        REQUIRE(!area.preemtive);
        const VideoCore::RasterizerDownloadArea low{0x1000, 0x2000, true};
        const auto high = Flush::CombineFlushArea(low, 0x10000, 0x12000, true);
        REQUIRE(high.start_address == 0x1000);
        REQUIRE(high.end_address == 0x12000);
    }
    {
        // A texture area flushing anyway stays as it is
        const VideoCore::RasterizerDownloadArea texture{0x10800, 0x11000, false};
        REQUIRE(Same(Flush::CombineFlushArea(texture, 0x10000, 0x12000, false), texture));
        const auto area = Flush::CombineFlushArea(texture, 0x10000, 0x12000, true);
        REQUIRE(area.start_address == 0x10000);
        REQUIRE(area.end_address == 0x12000);
        REQUIRE(!area.preemtive);
    }
    // Off (no buffer flush): the old answer for any read; on: never preemptive, covers the read
    // and the texture area.
    std::mt19937_64 rng{11};
    for (int i = 0; i < 200000; ++i) {
        const DAddr addr = rng() % (1ULL << 36);
        const u64 size = 1 + rng() % (3 * PAGE);
        const DAddr page_start = addr / PAGE * PAGE;
        const DAddr page_end = (addr + size + PAGE - 1) / PAGE * PAGE;
        std::optional<VideoCore::RasterizerDownloadArea> texture;
        if (rng() % 2 == 0) {
            const DAddr start = addr - rng() % (64 * PAGE);
            texture = VideoCore::RasterizerDownloadArea{start, start + 1 + rng() % (128 * PAGE),
                                                        rng() % 2 == 0};
        }
        REQUIRE(Same(Flush::CombineFlushArea(texture, page_start, page_end, false),
                     OldFlushArea(texture, addr, size)));
        const auto flushed = Flush::CombineFlushArea(texture, page_start, page_end, true);
        REQUIRE(!flushed.preemtive);
        REQUIRE(flushed.start_address <= addr);
        REQUIRE(flushed.end_address >= addr + size);
        REQUIRE(flushed.start_address <= page_start);
        REQUIRE(flushed.end_address >= page_end);
        if (texture) {
            REQUIRE(flushed.start_address == (std::min)(texture->start_address, page_start));
            REQUIRE(flushed.end_address == (std::max)(texture->end_address, page_end));
        } else {
            REQUIRE(flushed.start_address == page_start);
            REQUIRE(flushed.end_address == page_end);
        }
    }
}

TEST_CASE("Reactive latch age step", "[video_core][nx2_reactive]") {
    namespace Flush = VideoCommon::Nx2Reactive;
    REQUIRE(Flush::AgeStep(1) == 1);
    REQUIRE(Flush::AgeStep(3) == 1);
    REQUIRE(Flush::AgeStep(4) == 1);
    REQUIRE(Flush::AgeStep(8) == 2);
    REQUIRE(Flush::AgeStep(120) == 30);
    REQUIRE(!Flush::Expired(10, 10, 0));
    REQUIRE(Flush::Expired(11, 10, 0));
    REQUIRE(!Flush::Expired(70, 10, 60));
    REQUIRE(Flush::Expired(71, 10, 60));
    // A read "later" than the frame (another cache's counter) is never expired.
    REQUIRE(!Flush::Expired(5, 10, 1));

    const u32 epoch = Reactive::ReadAreaEpoch();
    Reactive::BumpReadAreaEpoch();
    REQUIRE(Reactive::ReadAreaEpoch() == epoch + 1);
}

TEST_CASE("Reactive read page ages", "[video_core][nx2_reactive]") {
    namespace Flush = VideoCommon::Nx2Reactive;
    Flush::ReadPageAges ages;
    ages.Note(0x10000, 0, 1);
    REQUIRE(ages.Size() == 0);
    ages.Note(0x10000, 0x3000, 1); // pages 0x10..0x12
    ages.Note(0x12fff, 2, 5);      // pages 0x12, 0x13
    ages.Note(0x20000, 1, 9);      // page 0x20
    REQUIRE(ages.Size() == 5);
    std::vector<u64> dropped;
    const auto drop = [&dropped](u64 page) { dropped.push_back(page); };
    REQUIRE(ages.Expire(4, 3, drop) == 0);
    REQUIRE(ages.Expire(5, 3, drop) == 2);
    std::sort(dropped.begin(), dropped.end());
    REQUIRE(dropped == std::vector<u64>{0x10000, 0x11000});
    REQUIRE(ages.Size() == 3);
    dropped.clear();
    REQUIRE(ages.Expire(9, 3, drop) == 2);
    std::sort(dropped.begin(), dropped.end());
    REQUIRE(dropped == std::vector<u64>{0x12000, 0x13000});
    ages.Note(0x20000, 0x1000, 20);
    dropped.clear();
    REQUIRE(ages.Expire(23, 3, drop) == 0);
    REQUIRE(ages.Expire(24, 3, drop) == 1);
    REQUIRE(dropped == std::vector<u64>{0x20000});
    REQUIRE(ages.Size() == 0);
}

TEST_CASE("Reactive read latch list", "[video_core][nx2_reactive]") {
    namespace Flush = VideoCommon::Nx2Reactive;
    Flush::ReadLatchList<u32> list;
    for (u32 id = 1; id <= 6; ++id) {
        list.Add(id);
    }
    list.Remove(3);
    list.Remove(42);
    REQUIRE(list.Size() == 5);
    std::set<u32> seen;
    const u64 cleared = list.Age([&seen](u32 id) {
        seen.insert(id);
        return id % 2 == 0;
    });
    REQUIRE(cleared == 3);
    REQUIRE(seen == std::set<u32>{1, 2, 4, 5, 6});
    REQUIRE(list.Size() == 2);
    seen.clear();
    REQUIRE(list.Age([&seen](u32 id) {
                seen.insert(id);
                return false;
            }) == 0);
    REQUIRE(seen == std::set<u32>{1, 5});
    list.Remove(1);
    list.Remove(5);
    REQUIRE(list.Size() == 0);
    REQUIRE(list.Age([](u32) { return true; }) == 0);
}

namespace {

/// The buffer cache's CPU-read pages with EDEN_NX2_READ_LATCH_AGE (NoteCpuRead, TickFrame), a
/// core's cached read area (Memory) and the read epoch, per page.
struct LatchModel {
    explicit LatchModel(u64 age_) : age{age_} {}

    /// A core's read of `page` at this frame: through its cached area unless the epoch changed.
    void Read(u64 page) {
        if (epoch != core_epoch) {
            core_epoch = epoch;
            cached_page.reset();
        }
        if (cached_page == page) {
            return;
        }
        cached_page = page;
        // GetFlushArea -> NoteCpuRead
        if (age != 0) {
            ages.Note(page << 12, 0x1000, frame);
        }
        noted.insert(page);
    }

    void TickFrame() {
        if (age != 0 && frame % VideoCommon::Nx2Reactive::AgeStep(age) == 0) {
            ages.Expire(frame, age, [this](u64 addr) { noted.erase(addr >> 12); });
            ++epoch;
        }
        ++frame;
    }

    u64 age;
    u64 frame = 0;
    u32 epoch = 0;
    u32 core_epoch = 0;
    std::optional<u64> cached_page;
    std::set<u64> noted;
    VideoCommon::Nx2Reactive::ReadPageAges ages;
};

} // Anonymous namespace

TEST_CASE("Reactive latch age model", "[video_core][nx2_reactive]") {
    // Off: pages stay noted for good, as before.
    {
        LatchModel model{0};
        model.Read(1);
        model.Read(2);
        for (int i = 0; i < 1000; ++i) {
            model.TickFrame();
        }
        REQUIRE(model.noted == std::set<u64>{1, 2});
        REQUIRE(model.epoch == 0);
    }
    std::mt19937 rng{3};
    for (const u64 age : {1ULL, 2ULL, 5ULL, 8ULL, 60ULL, 120ULL}) {
        LatchModel model{age};
        const u64 step = VideoCommon::Nx2Reactive::AgeStep(age);
        std::map<u64, u64> last_read;
        for (int frame = 0; frame < 3000; ++frame) {
            // Page 1: read every frame, from the cached area mostly. Page 2: read every `step`
            // frames. Pages 10..19: read now and then.
            model.Read(1);
            last_read[1] = model.frame;
            if (model.frame % step == 0) {
                model.Read(2);
                last_read[2] = model.frame;
            }
            if (rng() % 7 == 0) {
                const u64 page = 10 + rng() % 10;
                model.Read(page);
                last_read[page] = model.frame;
            }
            model.Read(1);
            model.TickFrame();
            // Read at least once an aging step: never dropped.
            REQUIRE(model.noted.count(1) == 1);
            REQUIRE(model.noted.count(2) == 1);
            for (const auto& [page, last] : last_read) {
                const u64 idle = model.frame - last;
                if (idle <= age) {
                    REQUIRE(model.noted.count(page) == 1);
                } else if (idle > age + step) {
                    REQUIRE(model.noted.count(page) == 0);
                }
            }
        }
    }
}

TEST_CASE("Reactive fence coalescing decision", "[video_core][nx2_reactive]") {
    namespace Flush = VideoCommon::Nx2Reactive;
    // Off (0 or 1), delayed fences, a fence made anyway or a frame end commit at once.
    for (const u64 limit : {u64{0}, u64{1}, u64{2}, u64{3}, u64{16}, u64{64}}) {
        for (u64 deferred = 0; deferred < 100; ++deferred) {
            for (int bits = 0; bits < 8; ++bits) {
                const bool delay = (bits & 1) != 0;
                const bool made = (bits & 2) != 0;
                const bool force = (bits & 4) != 0;
                const bool expected = limit <= 1 || delay || made || force || deferred + 1 >= limit;
                REQUIRE(Flush::CommitBufferCopyBack(limit, delay, made, force, deferred) ==
                        expected);
            }
        }
    }
    REQUIRE(Flush::CommitBufferCopyBack(0, false, false, false, 0));
    REQUIRE(Flush::CommitBufferCopyBack(1, false, false, false, 0));
    REQUIRE(!Flush::CommitBufferCopyBack(2, false, false, false, 0));
    REQUIRE(Flush::CommitBufferCopyBack(2, false, false, false, 1));
    REQUIRE(!Flush::CommitBufferCopyBack(64, false, false, false, 62));
    REQUIRE(Flush::CommitBufferCopyBack(64, false, false, false, 63));
    REQUIRE(Flush::CommitBufferCopyBack(64, true, false, false, 0));
    REQUIRE(Flush::CommitBufferCopyBack(64, false, true, false, 0));
    REQUIRE(Flush::CommitBufferCopyBack(64, false, false, true, 0));
    REQUIRE(Flush::ReadCount("EDEN_NX2_RX_TEST_UNSET_SWITCH") == 0);
}

TEST_CASE("Reactive fence coalescing model", "[video_core][nx2_reactive]") {
    namespace Flush = VideoCommon::Nx2Reactive;
    // FenceManager::CommitBufferNow's bookkeeping around the decision: per fence with copy backs
    // pending, a commit resets the deferred count; a frame end forces the next one. Every copy
    // back reaches a commit within `limit` fences and by the first fence after a frame end; a
    // made fence always takes it; off, every fence commits.
    std::mt19937_64 rng{0x5eed};
    for (const u64 limit : {u64{0}, u64{1}, u64{2}, u64{8}, u64{64}}) {
        u64 deferred = 0;
        bool force = false;
        u64 since_commit = 0;   // fences with pending copy backs since the last commit
        bool frame_ended = false;
        u64 commits = 0;
        u64 fences = 0;
        for (int step = 0; step < 200'000; ++step) {
            if (rng() % 50 == 0) {
                force = limit > 1;
                frame_ended = true;
                continue;
            }
            const bool made = rng() % 64 == 0;
            ++fences;
            const bool taken = force;
            force = false;
            const bool commit = Flush::CommitBufferCopyBack(limit, false, made, taken, deferred);
            if (commit) {
                deferred = 0;
                ++commits;
                since_commit = 0;
                frame_ended = false;
            } else {
                ++deferred;
                ++since_commit;
                REQUIRE(!made);
                REQUIRE(!frame_ended);
                REQUIRE(since_commit < limit);
            }
            if (limit <= 1) {
                REQUIRE(commit);
            }
        }
        if (limit <= 1) {
            REQUIRE(commits == fences);
        } else {
            // At least one commit per `limit` fences, and far fewer than one per fence.
            REQUIRE(commits * limit >= fences);
            REQUIRE(commits < fences);
        }
    }
}

TEST_CASE("Reactive fence line", "[video_core][nx2_reactive]") {
    Reactive::Window window;
    const auto set = [&](Reactive::Counter counter, u64 value) {
        window.counters[static_cast<std::size_t>(counter)] = value;
    };
    set(Reactive::Counter::Fences, 101);
    set(Reactive::Counter::FencesSkipped, 2);
    set(Reactive::Counter::FencesReal, 3);
    set(Reactive::Counter::FencesBufferCommit, 4);
    set(Reactive::Counter::FencesTextureCommit, 5);
    set(Reactive::Counter::FencesBufferDeferred, 6);
    set(Reactive::Counter::FenceExtraFlushes, 7);
    set(Reactive::Counter::FenceNs, 8'999);
    set(Reactive::Counter::FenceCommitNs, 9'000);
    set(Reactive::Counter::FenceSubmitNs, 10'500);
    set(Reactive::Counter::Submits, 11);
    set(Reactive::Counter::PopBufferNs, 12'000);
    set(Reactive::Counter::PopBufferWaitNs, 13'000);
    set(Reactive::Counter::RepeatFeedback, 14);
    set(Reactive::Counter::RepeatRtCpuModified, 15);
    set(Reactive::Counter::RepeatRtSparse, 16);
    set(Reactive::Counter::RepeatRtAlias, 17);
    set(Reactive::Counter::RepeatViewCpuModified, 18);
    set(Reactive::Counter::RepeatViewAlias, 19);
    set(Reactive::Counter::MaskedColorTargets, 20);
    set(Reactive::Counter::MaskedDepthTargets, 21);
    set(Reactive::Counter::Draws, 22);
    set(Reactive::Counter::FitInPassBarriers, 23);
    set(Reactive::Counter::FitInPassLightBarriers, 24);
    const std::string line = Reactive::FormatWindow(window);
    REQUIRE(Contains(line, "; fences 101 (skipped 2, real 3, buffer commits 4, texture commits 5, "
                           "buffer deferred 6, extra flushes 7) 8 us (commit 9 us, submit 10 us); "
                           "submits 11; copy back pops 12 us (lock wait 13 us); light refusals: "
                           "feedback 14, target cpu modified 15 sparse 16 alias 17, image cpu "
                           "modified 18 alias 19; masked targets color 20 depth 21; draws 22; "
                           "rt fit in pass barriers 23 (light 24)"));
    // The line ends with them (appended last: older parsers read the rest as before).
    REQUIRE(line.ends_with("; draws 22; rt fit in pass barriers 23 (light 24)"));
    // Appended: the older part ends as before.
    REQUIRE(Contains(line, "latched images 0; fences 101"));
    // The first MAX_IMAGE_LOGS images keeping draws off the light way get a line, no more.
    Reactive::GetState().repeat_logs = 0;
    for (u32 i = 0; i < Reactive::MAX_IMAGE_LOGS; ++i) {
        REQUIRE(Reactive::TakeRepeatLog());
    }
    REQUIRE(!Reactive::TakeRepeatLog());
    REQUIRE(!Reactive::TakeRepeatLog());
}

TEST_CASE("Reactive pitch linear forced", "[video_core][nx2_reactive]") {
    namespace Flush = VideoCommon::Nx2Reactive;
    // Reactive flushing off: pitch linear images forced, switch or not (as before); on: only with
    // EDEN_NX2_RX_PITCH_FORCED.
    static_assert(Flush::PitchLinearForced(false, false));
    static_assert(Flush::PitchLinearForced(false, true));
    static_assert(!Flush::PitchLinearForced(true, false));
    static_assert(Flush::PitchLinearForced(true, true));
    for (int bits = 0; bits < 4; ++bits) {
        const bool reactive = (bits & 1) != 0;
        const bool forced = (bits & 2) != 0;
        REQUIRE(Flush::PitchLinearForced(reactive, forced) == (!reactive || forced));
    }
}

TEST_CASE("Reactive masked targets", "[video_core][nx2_reactive]") {
    namespace Flush = VideoCommon::Nx2Reactive;
    // A color target is written when any channel of its mask is on.
    for (u32 bits = 0; bits < 16; ++bits) {
        REQUIRE(Flush::ColorMaskWrites(bits & 1, (bits >> 1) & 1, (bits >> 2) & 1,
                                       (bits >> 3) & 1) == (bits != 0));
    }
    static_assert(!Flush::ColorMaskWrites(0, 0, 0, 0));
    static_assert(Flush::ColorMaskWrites(0, 0, 0, 1));
    // Off: every target marked (as before); on: clears and writable targets only.
    for (int bits = 0; bits < 8; ++bits) {
        const bool enabled = (bits & 1) != 0;
        const bool clear = (bits & 2) != 0;
        const bool writes = (bits & 4) != 0;
        REQUIRE(Flush::MarksTarget(enabled, clear, writes) == (!enabled || clear || writes));
    }
    static_assert(Flush::MarksTarget(false, false, false));
    static_assert(!Flush::MarksTarget(true, false, false));
    static_assert(Flush::MarksTarget(true, true, false));
}

TEST_CASE("Reactive render target fit", "[video_core][nx2_reactive]") {
    namespace Flush = VideoCommon::Nx2Reactive;
    // Donkey Kong Bananza: a 2112x1179 target over a 2095x1179 texture, clip 2095x1179.
    static_assert(Flush::FitsTarget(0, 2095, 0, 1179, 2095, 1179, 2112, 1179));
    // A clip wider than the image (the guest may render there), offset, or smaller (the render
    // area would not cover the image a full clear invalidates): no.
    static_assert(!Flush::FitsTarget(0, 2112, 0, 1179, 2095, 1179, 2112, 1179));
    static_assert(!Flush::FitsTarget(1, 2095, 0, 1179, 2095, 1179, 2112, 1179));
    static_assert(!Flush::FitsTarget(0, 2095, 1, 1179, 2095, 1179, 2112, 1179));
    static_assert(!Flush::FitsTarget(1, 2094, 0, 1179, 2095, 1179, 2112, 1179));
    static_assert(!Flush::FitsTarget(0, 2080, 0, 1179, 2095, 1179, 2112, 1179));
    static_assert(!Flush::FitsTarget(0, 2095, 0, 1178, 2095, 1179, 2112, 1179));
    // Not narrower (the target's own image, or one taken already), or another height: no.
    static_assert(!Flush::FitsTarget(0, 2095, 0, 1179, 2112, 1179, 2112, 1179));
    static_assert(!Flush::FitsTarget(0, 2000, 0, 1179, 2095, 1179, 2095, 1179));
    static_assert(!Flush::FitsTarget(0, 2095, 0, 1000, 2095, 1000, 2112, 1179));
    static_assert(!Flush::FitsTarget(0, 2095, 0, 1179, 2095, 1180, 2112, 1179));
    // Exhaustive over small sizes against the definition.
    for (u32 clip_x = 0; clip_x < 3; ++clip_x) {
        for (u32 clip_w = 0; clip_w < 6; ++clip_w) {
            for (u32 clip_y = 0; clip_y < 3; ++clip_y) {
                for (u32 clip_h = 0; clip_h < 6; ++clip_h) {
                    for (u32 w = 0; w < 6; ++w) {
                        for (u32 h = 0; h < 6; ++h) {
                            for (u32 tw = 0; tw < 6; ++tw) {
                                for (u32 th = 0; th < 6; ++th) {
                                    const bool expected = w < tw && h == th && clip_x == 0 &&
                                                          clip_y == 0 && clip_w == w &&
                                                          clip_h == h;
                                    REQUIRE(Flush::FitsTarget(clip_x, clip_w, clip_y, clip_h, w,
                                                              h, tw, th) == expected);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    // The barrier only in mode 1 (2 is the diagnostic without it, 0 off).
    static_assert(!Flush::FitFeedbackBarrier(0));
    static_assert(Flush::FitFeedbackBarrier(1));
    static_assert(!Flush::FitFeedbackBarrier(2));
    static_assert(!Flush::FitFeedbackBarrier(3));
}

TEST_CASE("Reactive render target fit modes", "[video_core][nx2_reactive]") {
    namespace Flush = VideoCommon::Nx2Reactive;
    // The switch's values: 0-3, anything else off.
    for (u64 value = 0; value < 4; ++value) {
        REQUIRE(Flush::RtFitMode(value) == value);
    }
    REQUIRE(Flush::RtFitMode(4) == 0);
    REQUIRE(Flush::RtFitMode(5) == 0);
    REQUIRE(Flush::RtFitMode(~u64{0}) == 0);
    REQUIRE(Flush::RtFitMode(u64{1} << 32 | 3) == 0);
    static_assert(Flush::RtFitMode(3) == 3);
    // Mode 3: the barrier inside the render pass, never its end; 1 the end, never inside.
    static_assert(!Flush::FitInPassBarrier(0));
    static_assert(!Flush::FitInPassBarrier(1));
    static_assert(!Flush::FitInPassBarrier(2));
    static_assert(Flush::FitInPassBarrier(3));
    // Ordered (barrier_feedback_loops needed, checked every full draw): 1 and 3, not 0 or 2.
    static_assert(!Flush::FitOrdersFeedback(0));
    static_assert(Flush::FitOrdersFeedback(1));
    static_assert(!Flush::FitOrdersFeedback(2));
    static_assert(Flush::FitOrdersFeedback(3));
    // Lookups of every target: never off, always in 1 and 2 (as before), in 3 on a clip change.
    static_assert(!Flush::FitForcesLookup(0, false));
    static_assert(!Flush::FitForcesLookup(0, true));
    static_assert(Flush::FitForcesLookup(1, false));
    static_assert(Flush::FitForcesLookup(2, false));
    static_assert(!Flush::FitForcesLookup(3, false));
    static_assert(Flush::FitForcesLookup(3, true));
    for (u32 mode = 0; mode < 8; ++mode) {
        REQUIRE(Flush::FitInPassBarrier(mode) == (mode == 3));
        REQUIRE(Flush::FitOrdersFeedback(mode) == (mode == 1 || mode == 3));
        // At most one kind of barrier per mode.
        REQUIRE(!(Flush::FitFeedbackBarrier(mode) && Flush::FitInPassBarrier(mode)));
        for (const bool changed : {false, true}) {
            const bool before = mode == 1 || mode == 2; // the old rule (rt_fit != 0, modes <= 2)
            REQUIRE(Flush::FitForcesLookup(mode, changed) ==
                    (before || (mode == 3 && changed)));
        }
    }
    // TextureCache::CheckFeedbackLoop's answer per mode, for a draw sampling a color target's
    // image (`color`) and / or the depth target's (`depth`): the render pass end (and the light
    // way refused), or the barrier inside it. Off and 2: the fork's policy (depth only).
    struct Answer {
        bool end;
        bool in_pass;
    };
    const auto check = [](u32 mode, bool color, bool depth) {
        Answer answer{};
        if (color && Flush::FitOrdersFeedback(mode)) {
            if (Flush::FitFeedbackBarrier(mode)) {
                return Answer{true, false};
            }
            answer.in_pass = true;
        }
        if (depth) {
            answer.end = true;
        }
        if (answer.end) {
            answer.in_pass = false; // the render pass end's barrier covers the color targets
        }
        return answer;
    };
    for (u32 mode = 0; mode < 4; ++mode) {
        for (int bits = 0; bits < 4; ++bits) {
            const bool color = (bits & 1) != 0;
            const bool depth = (bits & 2) != 0;
            const Answer answer = check(mode, color, depth);
            REQUIRE(answer.end == (depth || (color && mode == 1)));
            REQUIRE(answer.in_pass == (color && !depth && mode == 3));
            REQUIRE(!(answer.end && answer.in_pass));
        }
    }
}
