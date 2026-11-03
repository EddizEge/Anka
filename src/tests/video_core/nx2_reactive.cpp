// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_reactive.h"
#include "core/nx2_host_core.h"

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
