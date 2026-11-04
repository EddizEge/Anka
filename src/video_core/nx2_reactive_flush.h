// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/nx2_reactive.h"
#include "video_core/rasterizer_download_area.h"

/// Experimental reactive flushing switches (off by default; they matter with use_reactive_flushing
/// on, where guest CPU reads of GPU-cached pages reach RasterizerVulkan::GetFlushArea).
namespace VideoCommon::Nx2Reactive {

/// EDEN_NX2_FLUSH_FIRST_READ=1: a guest CPU read that notes a buffer page (NoteCpuRead) the GPU
/// wrote (IsRegionGpuModified) flushes it once, synchronously (OnCPURead's FlushRegion). Without
/// it buffer reads are always preemptive: data the GPU wrote before the page was noted reaches the
/// guest only if the buffer is bound as written again and a fence copies it back, never with the
/// GPU idle (the first "clear, GPU writes, poll" round is lost). Later reads are left to the
/// fence copy back, as before.
[[nodiscard]] inline bool FlushFirstRead() noexcept {
    static const bool enabled =
        Common::Nx2Reactive::ParseFlag(Common::Nx2Reactive::GetEnv("EDEN_NX2_FLUSH_FIRST_READ"));
    return enabled;
}

/// Whether a GetFlushArea flushes its buffer pages (FlushFirstRead).
[[nodiscard]] constexpr bool FirstReadFlushes(bool enabled, bool newly_noted,
                                              bool gpu_modified) noexcept {
    return enabled && newly_noted && gpu_modified;
}

/// GetFlushArea's answer from the texture cache's (if it has GPU-modified images there) and the
/// read's buffer pages [page_start, page_end). Without a buffer flush it is the texture cache's,
/// else those pages, preemptive (as without FlushFirstRead); with one it covers both and is not
/// preemptive.
[[nodiscard]] inline VideoCore::RasterizerDownloadArea CombineFlushArea(
    const std::optional<VideoCore::RasterizerDownloadArea>& texture_area, DAddr page_start,
    DAddr page_end, bool flush_buffer) noexcept {
    if (texture_area) {
        VideoCore::RasterizerDownloadArea area = *texture_area;
        if (flush_buffer) {
            area.start_address = (std::min)(area.start_address, page_start);
            area.end_address = (std::max)(area.end_address, page_end);
            area.preemtive = false;
        }
        return area;
    }
    return VideoCore::RasterizerDownloadArea{
        .start_address = page_start,
        .end_address = page_end,
        .preemtive = !flush_buffer,
    };
}

/// EDEN_NX2_READ_LATCH_AGE=<frames> (0 / unset: off). What a guest CPU read latches is kept
/// while the guest keeps reading it: buffer pages it read (copied back at every fence) not read
/// again for more than that many frames are dropped from the CPU-read ranges, and images a read
/// made forced_flushed (downloaded at every render target bind) lose it the same way. Images
/// forced_flushed from their creation (pitch linear, reactive flushing off) keep it. Every
/// AgeStep frames each core's cached read area is forgotten (State::read_area_epoch) so its next
/// read of GPU-cached memory reaches GetFlushArea again and refreshes the age. A page or image
/// read again after it was dropped is latched again; until then the first-read gap is back
/// (EDEN_NX2_FLUSH_FIRST_READ closes it for buffers; a dropped image's next read downloads it).
[[nodiscard]] inline u64 ReadLatchAge() noexcept {
    static const u64 frames =
        Common::Nx2Reactive::ParseFrames(Common::Nx2Reactive::GetEnv("EDEN_NX2_READ_LATCH_AGE"));
    return frames;
}

/// Frames between aging steps: a quarter of the age, so a latch read at least that often stays.
[[nodiscard]] constexpr u64 AgeStep(u64 age) noexcept {
    return age / 4 != 0 ? age / 4 : 1;
}

/// Whether a latch last read at frame `last` is older than `age` at frame `frame`.
[[nodiscard]] constexpr bool Expired(u64 frame, u64 last, u64 age) noexcept {
    return frame > last && frame - last > age;
}

/// The frame each CPU-read buffer page was last read in.
class ReadPageAges {
public:
    static constexpr u64 PAGE_BITS = 12;

    /// The guest read [addr, addr + size) at `frame`.
    void Note(u64 addr, u64 size, u64 frame) {
        if (size == 0) {
            return;
        }
        const u64 last = (addr + size - 1) >> PAGE_BITS;
        for (u64 page = addr >> PAGE_BITS; page <= last; ++page) {
            frames[page] = frame;
        }
    }

    /// Forgets the pages older than `age` at `frame`, calling drop(page address) for each;
    /// returns how many.
    template <typename Func>
    u64 Expire(u64 frame, u64 age, Func&& drop) {
        u64 dropped = 0;
        for (auto it = frames.begin(); it != frames.end();) {
            if (Expired(frame, it->second, age)) {
                drop(it->first << PAGE_BITS);
                it = frames.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
        return dropped;
    }

    [[nodiscard]] std::size_t Size() const noexcept {
        return frames.size();
    }

private:
    std::unordered_map<u64, u64> frames;
};

/// The images a CPU read latched (by id), for aging.
template <typename Id>
class ReadLatchList {
public:
    void Add(Id id) {
        ids.push_back(id);
    }

    void Remove(Id id) {
        const auto it = std::find(ids.begin(), ids.end(), id);
        if (it != ids.end()) {
            *it = ids.back();
            ids.pop_back();
        }
    }

    /// Calls expire(id) for each; drops the ids it returns true for (it cleared their latch).
    /// Returns how many.
    template <typename Func>
    u64 Age(Func&& expire) {
        u64 cleared = 0;
        for (std::size_t i = 0; i < ids.size();) {
            if (expire(ids[i])) {
                ids[i] = ids.back();
                ids.pop_back();
                ++cleared;
            } else {
                ++i;
            }
        }
        return cleared;
    }

    [[nodiscard]] std::size_t Size() const noexcept {
        return ids.size();
    }

private:
    std::vector<Id> ids;
};

/// A count switch; 0 (off) when unset or not a number.
[[nodiscard]] inline u64 ReadCount(const char* name) noexcept {
    return Common::Nx2Reactive::ParseFrames(Common::Nx2Reactive::GetEnv(name));
}

/// EDEN_NX2_RX_COALESCE=<fences> (0 / 1 / unset: off). With reactive flushing the buffer cache
/// copies back, at every fence, what the GPU wrote to pages the guest CPU read. Switch 2 titles
/// signal a fence per compute dispatch, which the fence manager skips when it has nothing to
/// download (SkipEmptyFence): a copy back turns each into a real fence, that is a submission (the
/// fence's own Queue) with the render pass ended and the command buffer state invalidated, plus a
/// pipeline barrier pair around the copies. Here a fence that would be skipped but for the buffer
/// copy back leaves it uncommitted (the GPU-written ranges keep accumulating) for at most
/// <fences> - 1 fences; the copy back rides along with the next fence made anyway (texture or
/// query downloads, queued operations, the skip limit), the <fences>th one, or the first one
/// after a frame ended. Only without delayed fences (GPU accuracy not high, fence behavior
/// default / immediate): there a fence's operations (syncpoint, semaphore) run when it is signaled,
/// before the GPU even ran, so the copy back is not ordered with them anyway; with delayed fences
/// it is (data before the syncpoint), and every fence commits as before.
[[nodiscard]] inline u64 CoalesceFences() noexcept {
    static const u64 fences = ReadCount("EDEN_NX2_RX_COALESCE");
    return fences;
}

/// Whether a fence commits the buffer cache's pending copy backs now (CoalesceFences): `deferred`
/// fences already left them, `made_anyway` when the fence is made with or without them, `force`
/// after a frame ended.
[[nodiscard]] constexpr bool CommitBufferCopyBack(u64 limit, bool delay_fence, bool made_anyway,
                                                  bool force, u64 deferred) noexcept {
    return limit <= 1 || delay_fence || made_anyway || force || deferred + 1 >= limit;
}

/// EDEN_NX2_RX_ONE_SUBMIT=1: a fence with downloads to wait for is made real, and a real fence's
/// Queue submits the command buffer (InnerFence::Queue: scheduler.Flush); SignalFence then called
/// FlushCommands, which submits again whenever the rasterizer drew since its last own flush: an
/// empty submission (and state invalidation) per such fence. With the switch the second one is
/// left out; nothing was recorded between the two.
[[nodiscard]] inline bool OneSubmit() noexcept {
    static const bool enabled =
        Common::Nx2Reactive::ParseFlag(Common::Nx2Reactive::GetEnv("EDEN_NX2_RX_ONE_SUBMIT"));
    return enabled;
}

/// EDEN_NX2_RX_NO_COPYBACK=1 (DIAGNOSTIC ONLY, breaks reactive flushing's point): guest CPU reads
/// note no buffer page (NoteCpuRead), so nothing the GPU writes to buffers is copied back at
/// fences; reads still trap and images still latch.
[[nodiscard]] inline bool NoCopyBack() noexcept {
    static const bool enabled =
        Common::Nx2Reactive::ParseFlag(Common::Nx2Reactive::GetEnv("EDEN_NX2_RX_NO_COPYBACK"));
    return enabled;
}

/// EDEN_NX2_RX_NO_RT_LATCH=1 (DIAGNOSTIC ONLY, breaks reactive flushing's point for images): a
/// guest CPU read of a GPU-modified image latches nothing (no forced_flushed, no
/// PreemtiveDownload on its views) and is answered preemptive: the image is never downloaded for
/// it, the guest reads what its memory last held.
[[nodiscard]] inline bool NoRtLatch() noexcept {
    static const bool enabled =
        Common::Nx2Reactive::ParseFlag(Common::Nx2Reactive::GetEnv("EDEN_NX2_RX_NO_RT_LATCH"));
    return enabled;
}

} // namespace VideoCommon::Nx2Reactive
