// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"
#include "video_core/nx2_gpu_pages.h"
#include "video_core/nx2_profile.h"

namespace {
using namespace VideoCommon::Nx2Stages;
namespace Profile = VideoCommon::Nx2Profile;

constexpr u64 Page = 0x1000;
/// Far from what the other test files mark around the caches (Tegra's bitmaps are global).
constexpr DAddr Base = 0x7'0000'0000;

/// The summary, started (the texture cache's constructor does it), on the heap: it is ~100 KiB.
std::unique_ptr<Nx2GpuPages> Started(bool check = false) {
    auto pages = std::make_unique<Nx2GpuPages>();
    pages->Start(check);
    return pages;
}

/// This thread's count of a profile counter (counted only with EDEN_NX2_GPU_PROFILE set, as
/// run.sh does; without it the counter tests check nothing but that nothing breaks).
u64 Count(Profile::Counter counter) {
    return Profile::t_data.counters[static_cast<size_t>(counter)];
}

/// Ends the test run when a test hangs instead of waiting forever: after NX2_TEST_WATCHDOG
/// seconds (default 120).
class Watchdog {
public:
    explicit Watchdog(const char* name_)
        : thread{[name = name_](std::stop_token token) {
              const char* value = std::getenv("NX2_TEST_WATCHDOG");
              const int seconds = value != nullptr ? std::atoi(value) : 120;
              std::mutex mutex;
              std::condition_variable_any condition;
              std::unique_lock lock{mutex};
              condition.wait_for(lock, token, std::chrono::seconds{seconds}, [] { return false; });
              if (!token.stop_requested()) {
                  std::printf("FAILED %s: no progress in %d s (deadlock)\n", name, seconds);
                  std::fflush(stdout);
                  std::_Exit(1);
              }
          }} {}

private:
    std::jthread thread;
};
} // Anonymous namespace

TEST_CASE("Nx2GpuPages: off, every page may hold data", "[video_core]") {
    Nx2GpuPages skeleton;
    REQUIRE(skeleton.Holds(0, 1));
    auto pages = Started();
    REQUIRE(!pages->Holds(Base, 4));
    pages->Stop();
    REQUIRE(pages->Holds(Base, 4));
    REQUIRE(!pages->Live());
}

TEST_CASE("Nx2GpuPages: an image's marks, by page", "[video_core]") {
    auto pages = Started();
    REQUIRE(pages->Live());
    REQUIRE(!pages->Holds(Base, 0x10000));
    pages->MarkImage(Base + 0x1800, 0x1000); // pages 1 and 2
    REQUIRE(!pages->Holds(Base, Page));
    REQUIRE(pages->Holds(Base + Page, 4));
    REQUIRE(pages->Holds(Base + 2 * Page + 0xffc, 4));
    REQUIRE(!pages->Holds(Base + 3 * Page, 4));
    REQUIRE(pages->Holds(Base, 0x10000));
    // A size of 0 asks for its byte's page.
    REQUIRE(pages->Holds(Base + Page, 0));
    // Another block (16 MiB on): allocated apart.
    REQUIRE(!pages->Holds(Base + 0x100'0000, 4));
    pages->MarkImage(Base + 0x100'0000, 1);
    REQUIRE(pages->Holds(Base + 0x100'0000, 4));
    REQUIRE(!pages->Holds(Base + 0x100'1000, 4));
}

TEST_CASE("Nx2GpuPages: a clean-up waits for the safe point", "[video_core]") {
    auto pages = Started();
    pages->MarkImage(Base, 4 * Page);
    pages->UnmarkImage(Base + Page, 2 * Page);
    REQUIRE(pages->Holds(Base + Page, 4));
    pages->Settle();
    REQUIRE(pages->Holds(Base, 4));
    REQUIRE(!pages->Holds(Base + Page, 4));
    REQUIRE(!pages->Holds(Base + 2 * Page, 4));
    REQUIRE(pages->Holds(Base + 3 * Page, 4));
    // Ran publishes C's seq only (it takes no lock): the clean-up waits for Settle.
    pages->UnmarkImage(Base, Page);
    pages->Ran(1);
    REQUIRE(pages->RanSeq() == 1);
    REQUIRE(pages->Holds(Base, 4));
    pages->Settle();
    REQUIRE(!pages->Holds(Base, 4));
}

TEST_CASE("Nx2GpuPages: a page marked after its clean-up was queued stays", "[video_core]") {
    // JoinImages: the overlaps unregister (their pages queued), the new image registers.
    auto pages = Started();
    pages->MarkImage(Base, 2 * Page);
    pages->UnmarkImage(Base, 2 * Page);
    pages->MarkImage(Base + Page, Page);
    pages->Settle();
    REQUIRE(!pages->Holds(Base, 4));
    REQUIRE(pages->Holds(Base + Page, 4));
    // A clean-up queued after the mark clears it.
    pages->UnmarkImage(Base + Page, Page);
    pages->Settle();
    REQUIRE(!pages->Holds(Base + Page, 4));
}

TEST_CASE("Nx2GpuPages: B's set holds until C ran its seq", "[video_core]") {
    auto pages = Started();
    pages->PreSet(Base, 2 * Page, 1);
    REQUIRE(pages->Holds(Base + Page, 4));
    REQUIRE(!pages->Holds(Base + 2 * Page, 4));
    // C ran it and marked nothing there (the draw wrote elsewhere): free again.
    pages->Ran(1);
    REQUIRE(!pages->Holds(Base, 4));
    // C ran it and marked the image: C's mark holds.
    pages->PreSet(Base, Page, 2);
    pages->MarkImage(Base, Page);
    pages->Ran(2);
    REQUIRE(pages->Holds(Base, 4));
}

TEST_CASE("Nx2GpuPages: an older clean-up leaves a later set (S12)", "[video_core]") {
    auto pages = Started();
    pages->MarkImage(Base, Page);
    pages->Ran(1);
    // B sets the page for packet 5; C, at its seq 3, cleans up an older image there.
    pages->PreSet(Base, Page, 5);
    pages->UnmarkImage(Base, Page);
    pages->Clear(Base, Page, 3);
    pages->Settle();
    pages->Ran(3);
    REQUIRE(pages->Holds(Base, 4));
    pages->Ran(4);
    REQUIRE(pages->Holds(Base, 4));
    pages->Ran(5);
    REQUIRE(!pages->Holds(Base, 4));
}

TEST_CASE("Nx2GpuPages: Set and Clear (another source) apart from the images", "[video_core]") {
    auto pages = Started();
    pages->Set(Base, Page, 1);
    pages->MarkImage(Base + Page, Page);
    pages->Clear(Base, 2 * Page, 1);
    pages->Settle();
    REQUIRE(!pages->Holds(Base, 4));
    // Clear takes its own source only.
    REQUIRE(pages->Holds(Base + Page, 4));
    pages->Set(Base + Page, Page, 2);
    pages->UnmarkImage(Base + Page, Page);
    pages->Settle();
    // The image's clean-up was queued after the Set: the image source goes, Set's stays.
    REQUIRE(pages->Holds(Base + Page, 4));
    pages->Clear(Base + Page, Page, 3);
    pages->Settle();
    REQUIRE(!pages->Holds(Base + Page, 4));
}

TEST_CASE("Nx2GpuPages: B's ranges in flight", "[video_core]") {
    auto pages = Started();
    // The same render targets draw after draw: one range each.
    for (u64 seq = 1; seq <= 1000; ++seq) {
        pages->PreSet(Base, 8 * Page, seq);
        pages->PreSet(Base + 0x10'0000, 8 * Page, seq);
    }
    pages->Ran(999);
    REQUIRE(pages->Holds(Base + 7 * Page, 4));
    REQUIRE(pages->Holds(Base + 0x10'0000, 4));
    REQUIRE(!pages->Holds(Base + 8 * Page, 4));
    pages->Ran(1000);
    REQUIRE(!pages->Holds(Base, 4));
    REQUIRE(!pages->Holds(Base + 0x10'0000, 4));
    // A lower seq counts as B's last.
    pages->PreSet(Base, Page, 1002);
    pages->PreSet(Base + Page, Page, 1001);
    pages->Ran(1001);
    REQUIRE(pages->Holds(Base + Page, 4));
    pages->Ran(1002);
    REQUIRE(!pages->Holds(Base + Page, 4));
    // More distinct ranges than it keeps: every page holds until C ran them.
    for (u32 i = 0; i <= Nx2GpuPages::MaxPresets; ++i) {
        pages->PreSet(Base + u64{i} * 2 * Page, Page, 1003 + i);
    }
    REQUIRE(pages->Holds(Base + 0x4000'0000, 4));
    pages->Ran(1003 + Nx2GpuPages::MaxPresets - 1);
    REQUIRE(pages->Holds(Base + 0x4000'0000, 4));
    pages->Ran(1003 + Nx2GpuPages::MaxPresets);
    REQUIRE(!pages->Holds(Base + 0x4000'0000, 4));
    REQUIRE(!pages->Holds(Base, 4));
    // The ranges C ran are dropped: room again.
    for (u32 i = 0; i < Nx2GpuPages::MaxPresets; ++i) {
        pages->PreSet(Base + u64{i} * 2 * Page, Page, 2000 + i);
    }
    REQUIRE(!pages->Holds(Base + 0x4000'0000, 4));
    REQUIRE(pages->Holds(Base, 4));
}

TEST_CASE("Nx2GpuPages: work B cannot place, and GPU ranges", "[video_core]") {
    auto pages = Started();
    pages->PreSetAll(3);
    REQUIRE(pages->Holds(Base + 0x5000'0000, 4));
    pages->Ran(2);
    REQUIRE(pages->Holds(Base, 4));
    pages->Ran(3);
    REQUIRE(!pages->Holds(Base, 4));
    // A GPU range over scattered device pages: GPU pages 0-1 at Base, 2 unmapped, 3-4 at
    // Base + 1 MiB.
    struct Memory {
        std::optional<DAddr> GpuToCpuAddress(GPUVAddr address) const {
            const u64 page = address >> 12;
            if (page <= 1) {
                return Base + page * Page;
            }
            if (page >= 3 && page <= 4) {
                return Base + 0x10'0000 + (page - 3) * Page;
            }
            return std::nullopt;
        }
    };
    PreSetGpu(*pages, Memory{}, 0x800, 5 * Page - 0x1000, 4);
    REQUIRE(pages->Holds(Base, 4));
    REQUIRE(pages->Holds(Base + Page, 4));
    REQUIRE(!pages->Holds(Base + 2 * Page, 4));
    REQUIRE(pages->Holds(Base + 0x10'0000, 4));
    REQUIRE(pages->Holds(Base + 0x10'1000, 4));
    REQUIRE(!pages->Holds(Base + 0x10'2000, 4));
    pages->Ran(4);
    REQUIRE(!pages->Holds(Base, 0x20'0000));
}

TEST_CASE("Nx2GpuPages: writes around the caches and the address range", "[video_core]") {
    auto pages = Started();
    const DAddr address = Base + 0x2000'0000;
    REQUIRE(!pages->Holds(address, 4));
    Tegra::Nx2MarkUntrackedWrite(address, 16);
    REQUIRE(pages->Holds(address + 8, 4));
    REQUIRE(!pages->Holds(address + Page, 4));
    // Past the device addresses it covers: every page holds; marks there change nothing.
    const DAddr past = DAddr{1} << Nx2GpuPages::AddressBits;
    REQUIRE(pages->Holds(past, 4));
    REQUIRE(pages->Holds(past - 4, 8));
    pages->MarkImage(past, Page);
    pages->UnmarkImage(past, Page);
    pages->Settle();
    Tegra::Nx2ClearGpuWrittenPages();
    REQUIRE(!pages->Holds(address, 4));
}

TEST_CASE("Nx2GpuPages: queued clean-ups past the queue stay marked", "[video_core]") {
    auto pages = Started();
    pages->MarkImage(Base, 2 * Page);
    for (u32 i = 0; i < Nx2GpuPages::MaxClears; ++i) {
        pages->UnmarkImage(Base + 0x100'0000, Page);
    }
    pages->UnmarkImage(Base, 2 * Page);
    pages->Settle();
    REQUIRE(pages->Holds(Base, 4));
    pages->UnmarkImage(Base, 2 * Page);
    pages->Settle();
    REQUIRE(!pages->Holds(Base, 4));
}

TEST_CASE("Nx2GpuPages: Start forgets, Reset keeps the blocks", "[video_core]") {
    auto pages = Started();
    pages->MarkImage(Base, Page);
    pages->PreSet(Base + Page, Page, 9);
    pages->Ran(3);
    pages->Start(false);
    REQUIRE(pages->RanSeq() == 0);
    REQUIRE(!pages->Holds(Base, 2 * Page));
}

TEST_CASE("Nx2GpuPages: STAGE_CHECK hazards", "[video_core]") {
    const bool counted = Profile::Enabled();
    auto pages = Started(true);
    REQUIRE(pages->Checking());
    const u64 pages0 = Count(Profile::Counter::HazardPages);
    const u64 untracked0 = Count(Profile::Counter::HazardUntracked);
    const u64 epoch0 = Count(Profile::Counter::HazardEpoch);
    // The caches' answer at the same point of the stream: holding where the summary says no.
    REQUIRE(!pages->CheckHolds(Base, 4, false));
    REQUIRE(Count(Profile::Counter::HazardPages) == pages0);
    REQUIRE(!pages->CheckHolds(Base, 4, true));
    REQUIRE(Count(Profile::Counter::HazardPages) == pages0 + (counted ? 1 : 0));
    pages->MarkImage(Base, Page);
    REQUIRE(pages->CheckHolds(Base, 4, true));
    REQUIRE(Count(Profile::Counter::HazardPages) == pages0 + (counted ? 1 : 0));

    // B relied on pages holding nothing, about to send packet 5 (set pages for it already).
    pages->PreSet(Base + 0x10'0000, Page, 5);
    REQUIRE(!pages->Holds(Base + Page, 4));
    REQUIRE(!pages->Holds(Base + 2 * Page, 4));
    REQUIRE(!pages->Holds(Base + 3 * Page, 4));
    // C, within packet 3, marks one: C's work came first in the stream.
    pages->Ran(2);
    pages->MarkImage(Base + Page, Page);
    REQUIRE(Count(Profile::Counter::HazardPages) == pages0 + (counted ? 2 : 0));
    // ... and writes one around the caches.
    pages->NoteUntracked(Base + 2 * Page, 16);
    REQUIRE(Count(Profile::Counter::HazardUntracked) == untracked0 + (counted ? 1 : 0));
    // Within packet 5 itself the order is not known: not counted.
    pages->Ran(4);
    pages->MarkImage(Base + 3 * Page, Page);
    pages->NoteUntracked(Base + 3 * Page, 16);
    REQUIRE(Count(Profile::Counter::HazardPages) == pages0 + (counted ? 2 : 0));
    REQUIRE(Count(Profile::Counter::HazardUntracked) == untracked0 + (counted ? 1 : 0));
    // B's copy of the light epoch behind what C ran.
    pages->CheckEpoch(7, 7);
    REQUIRE(Count(Profile::Counter::HazardEpoch) == epoch0);
    pages->CheckEpoch(6, 7);
    REQUIRE(Count(Profile::Counter::HazardEpoch) == epoch0 + (counted ? 1 : 0));
    // Without STAGE_CHECK nothing is counted.
    auto quiet = Started(false);
    REQUIRE(!quiet->CheckHolds(Base, 4, true));
    quiet->CheckEpoch(0, 1);
    REQUIRE(Count(Profile::Counter::HazardPages) == pages0 + (counted ? 2 : 0));
    REQUIRE(Count(Profile::Counter::HazardEpoch) == epoch0 + (counted ? 1 : 0));
}

TEST_CASE("Nx2GpuPages: B and C race, B never sees a hole", "[video_core]") {
    // B (this thread) sends packets; each sets its render target (one of four pages, 16 MiB
    // apart: other blocks) before it is sent and asks about it right after: C may not have run
    // it, may run it now or ran it. C marks the target in the packet and, in every packet, also
    // deletes and registers again the image of another target (unmark, mark: JoinImages), and
    // cleans up a page no image ever covers. The targets hold data throughout: Holds must say so
    // whatever C does meanwhile; the free page must read free once C ran past its clean-up.
    Watchdog watchdog{"Nx2GpuPages race"};
    auto pages = Started();
    constexpr u64 Packets = 200'000;
    constexpr u64 Spacing = 0x100'0000;
    const DAddr free_page = Base + 0x30'0000;
    std::atomic<u64> sent{0};
    std::atomic<bool> failed{false};
    std::jthread c_thread{[&] {
        std::minstd_rand random{7};
        for (u64 seq = 1; seq <= Packets; ++seq) {
            while (sent.load(std::memory_order_acquire) < seq) {
                std::this_thread::yield();
            }
            pages->MarkImage(Base + (seq % 4) * Spacing, Page);
            const u64 other = random() % 4;
            if (seq > 4) {
                pages->UnmarkImage(Base + other * Spacing, Page);
                pages->MarkImage(Base + other * Spacing, Page);
            }
            pages->MarkImage(free_page, Page);
            pages->UnmarkImage(free_page, Page);
            // A safe point every packet (the texture cache's TickFrame makes one a frame).
            pages->Settle();
            pages->Ran(seq);
        }
    }};
    u64 free_seen = 0;
    for (u64 seq = 1; seq <= Packets; ++seq) {
        const DAddr target = Base + (seq % 4) * Spacing;
        pages->PreSet(target, Page, seq);
        sent.store(seq, std::memory_order_release);
        if (!pages->Holds(target, 4)) {
            failed = true;
        }
        if (seq > 4) {
            for (u64 i = 0; i < 4; ++i) {
                if (!pages->Holds(Base + i * Spacing, 4)) {
                    failed = true;
                }
            }
        }
        free_seen += pages->Holds(free_page, 4) ? 0 : 1;
    }
    c_thread.join();
    REQUIRE(!failed);
    REQUIRE(!pages->Holds(free_page, 4));
    std::printf("  race: %llu packets, free page read free %llu times\n",
                static_cast<unsigned long long>(Packets),
                static_cast<unsigned long long>(free_seen));
}
