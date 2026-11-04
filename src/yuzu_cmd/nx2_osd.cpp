// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/nx2_osd.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

#include "common/logging.h"
#include "common/nx2_rtss_osd.h"
#include "common/thread.h"
#include "core/hle/service/nx2_osd_speed.h"
#include "core/perf_stats.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Nx2Osd {

namespace Osd = Common::Nx2RtssOsd;

#ifdef _WIN32

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr auto Period = 500ms; // a new text every ~0.5 s
constexpr auto Retry = 2s;     // RTSS missing or its memory unusable
constexpr size_t Window = 3;   // samples kept: the rates span the last ~1 s

struct Access {
    Osd::Status status;
    u32 version;
};

// RTSS's shared memory, opened for one access and closed after it, as the SDK sample does: RTSS
// started, closed or restarted between two accesses is seen at the next one, and no handle of
// ours keeps an old mapping alive. nullopt: no mapping (RTSS not running) or no view.
template <typename F>
std::optional<Access> WithSharedMemory(F&& access) {
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "RTSSSharedMemoryV2");
    if (mapping == nullptr) {
        return std::nullopt;
    }
    std::optional<Access> result;
    if (void* const view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) {
        // The view's size: its pages share one allocation and protection, so the region from
        // its base is the view (rounded up to pages). A smaller answer only fails LayoutFits.
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(view, &info, sizeof(info)) == sizeof(info)) {
            const std::span<u8> memory{static_cast<u8*>(view), info.RegionSize};
            const u32 version =
                memory.size() >= sizeof(Osd::Header)
                    ? reinterpret_cast<const Osd::Header*>(memory.data())->version
                    : 0;
            result = Access{access(memory), version};
        }
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    return result;
}

} // Anonymous namespace

class Writer {
public:
    explicit Writer(std::function<s64()> system_time_us_)
        : system_time_us{std::move(system_time_us_)},
          thread{[this](std::stop_token stop) { Run(stop); }} {}

    // jthread's destructor asks the thread to stop (waking the wait) and joins it; the thread
    // empties our slot on its way out.

private:
    Osd::Sample Take() const {
        const auto speed = Service::Nx2OsdSpeed::g_counter.Get();
        return Osd::Sample{
            .seconds = std::chrono::duration<double>(Clock::now().time_since_epoch()).count(),
            .game_frames = Core::g_game_frames.load(std::memory_order_relaxed),
            .output_frames = Core::g_output_frames.load(std::memory_order_relaxed),
            .speed_frame_us = speed.frame_us,
            .speed_game_us = speed.game_us,
            .system_us = system_time_us ? system_time_us() : 0,
        };
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("Nx2Osd");
        LOG_INFO(Frontend, "nx2 osd (EDEN_NX2_OSD): game FPS, display FPS and speed in "
                           "RivaTuner Statistics Server's OSD every 0.5 s (RTSS shows it with its "
                           "\"Show On-Screen Display\" on)");
        std::array<Osd::Sample, Window> samples{};
        size_t count = 0;
        Clock::time_point next_try{};
        bool connected = false;
        bool missing_logged = false;
        std::optional<Osd::Status> problem; // the last one logged
        while (!stop.stop_requested()) {
            {
                std::unique_lock lock{mutex};
                wake.wait_for(lock, stop, Period, [] { return false; });
            }
            if (stop.stop_requested()) {
                break;
            }
            if (count == Window) {
                std::move(samples.begin() + 1, samples.end(), samples.begin());
                --count;
            }
            samples[count++] = Take();
            const auto now = Clock::now();
            if (count < 2 || now < next_try) {
                continue;
            }
            const std::string text = Osd::Format(Osd::Compute(samples[0], samples[count - 1]));
            const auto access = WithSharedMemory(
                [&text](std::span<u8> memory) { return Osd::Update(memory, text); });
            if (!access) {
                if (connected) {
                    LOG_INFO(Frontend, "nx2 osd: RTSS's shared memory is gone (RTSS closed?); "
                                       "trying again every 2 s");
                } else if (!missing_logged) {
                    LOG_INFO(Frontend, "nx2 osd: no RTSS shared memory (RTSS not running?); "
                                       "trying again every 2 s");
                }
                missing_logged = true;
                connected = false;
                next_try = now + Retry;
                continue;
            }
            switch (access->status) {
            case Osd::Status::Written:
                if (!connected) {
                    LOG_INFO(Frontend, "nx2 osd: writing to RTSS's OSD (shared memory v{}.{}): {}",
                             access->version >> 16, access->version & 0xffff, text);
                }
                connected = true;
                problem.reset();
                break;
            case Osd::Status::Busy:
                break; // the lock was held: the next text in 0.5 s
            default:
                if (problem != access->status) {
                    LOG_WARNING(Frontend, "nx2 osd: RTSS shared memory v{}.{}: {}; trying again "
                                          "every 2 s",
                                access->version >> 16, access->version & 0xffff,
                                Osd::StatusName(access->status));
                }
                problem = access->status;
                connected = false;
                next_try = now + Retry;
                break;
            }
        }
        // Empty our slot: under the lock if RTSS lets go of it within ~20 ms, else without it,
        // as the sample does.
        const auto give_up = Clock::now() + 20ms;
        for (bool force = false;; force = Clock::now() >= give_up) {
            const auto access = WithSharedMemory(
                [force](std::span<u8> memory) { return Osd::Release(memory, force); });
            if (!access || access->status != Osd::Status::Busy) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    std::function<s64()> system_time_us;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::jthread thread; // last: started once the members above exist, joined first
};

void WriterDeleter::operator()(Writer* writer) const {
    delete writer;
}

WriterPtr StartFromEnvironment(std::function<s64()> system_time_us) {
    if (!Osd::Enabled(std::getenv("EDEN_NX2_OSD"))) {
        return nullptr;
    }
    return WriterPtr{new Writer(std::move(system_time_us))};
}

#else

class Writer {};

void WriterDeleter::operator()(Writer* writer) const {
    delete writer;
}

WriterPtr StartFromEnvironment(std::function<s64()>) {
    if (Osd::Enabled(std::getenv("EDEN_NX2_OSD"))) {
        LOG_WARNING(Frontend, "EDEN_NX2_OSD: RivaTuner Statistics Server's OSD is Windows only");
    }
    return nullptr;
}

#endif

} // namespace Nx2Osd
