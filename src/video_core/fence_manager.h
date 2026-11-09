// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <queue>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/nx2_reactive.h"
#include "common/settings.h"
#include "common/thread.h"
#include "video_core/delayed_destruction_ring.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_reactive_flush.h"
#include "video_core/nx2_stages.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCommon {

class FenceBase {
public:
    explicit FenceBase(bool is_stubbed_) : is_stubbed{is_stubbed_} {}

    bool IsStubbed() const {
        return is_stubbed;
    }

protected:
    bool is_stubbed;
};

template <typename Traits>
class FenceManager {
    using TFence = typename Traits::FenceType;
    using TTextureCache = typename Traits::TextureCacheType;
    using TBufferCache = typename Traits::BufferCacheType;
    using TQueryCache = typename Traits::QueryCacheType;
    static constexpr bool can_async_check = Traits::HAS_ASYNC_CHECK;

public:
    /// Notify the fence manager about a new frame
    void TickFrame() {
        std::unique_lock lock(ring_guard);
        delayed_destruction_ring.Tick();
        // EDEN_NX2_RX_COALESCE: copy backs left uncommitted go with the frame's next fence.
        if (coalesce_fences > 1) [[unlikely]] {
            coalesce_force.store(true, std::memory_order_relaxed);
        }
    }

    // Unlike other fences, this one doesn't
    void SignalOrdering() {
        if constexpr (!can_async_check) {
            TryReleasePendingFences<false>();
        }
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.AccumulateFlushes();
    }

    void SignalReference() {
        std::function<void()> do_nothing([] {});
        SignalFence(std::move(do_nothing));
    }

    void SyncOperation(std::function<void()>&& func) {
        // Not only the GPU thread queues these (query cache work runs elsewhere too), while
        // SignalFence moves the list into pending_operations: unguarded, the list was corrupted
        // and the fencing thread crashed walking it. Its own lock, held for no callbacks.
        std::scoped_lock lock{uncommitted_guard};
        uncommitted_operations.emplace_back(std::move(func));
    }

    void SignalFence(std::function<void()>&& func) {
        Nx2Profile::Scope profile_scope{Nx2Profile::Zone::Fence};
        // EDEN_NX2_REACTIVE_STATS: the fence's count and time, by part (common/nx2_reactive.h).
        const bool stats = Common::Nx2Reactive::StatsEnabled();
        const s64 start_ns = stats ? Common::Nx2Reactive::NowNs() : 0;
        const bool delay_fence = Settings::IsGPUFenceBehaviorDefault() ? Settings::IsGPULevelHigh() : Settings::IsGPUFenceBehaviorBalanced() || Settings::IsGPUFenceBehaviorAccurate();
        if constexpr (!can_async_check) {
            TryReleasePendingFences<false>();
        }
        auto [should_flush, committed_flushes] = CommitCacheFlushes(delay_fence);
        if (stats) [[unlikely]] {
            CountCommit(start_ns, committed_flushes);
        }
        if (!delay_fence && committed_flushes == 0 && SkipEmptyFence()) {
            Nx2Profile::Scope skipped_scope{Nx2Profile::Zone::FenceSkipped};
            func();
            rasterizer.InvalidateGPUCache();
            if (stats) [[unlikely]] {
                Common::Nx2Reactive::Add(Common::Nx2Reactive::Counter::FencesSkipped, 1);
                CountFence(start_ns);
            }
            return;
        }
        skipped_fences = 0;
        should_flush |= query_cache.HasUncommittedFlushes();
        query_cache.CommitAsyncFlushes();
        TFence new_fence = CreateFence(!should_flush);
        if constexpr (can_async_check) {
            guard.lock();
        }
        {
            std::scoped_lock lock{uncommitted_guard};
            if (delay_fence) {
                uncommitted_operations.emplace_back(std::move(func));
            }
            pending_operations.emplace_back(std::move(uncommitted_operations));
            uncommitted_operations.clear(); // moved-from: make it empty explicitly
        }
        pending_flushes.push_back(committed_flushes);
        const s64 submit_ns = stats ? Common::Nx2Reactive::NowNs() : 0;
        const bool real_fence = !new_fence->IsStubbed();
        QueueFence(new_fence);
        if (!delay_fence) {
            func();
        }
        fences.push(std::move(new_fence));
        // A real fence's Queue submitted already (EDEN_NX2_RX_ONE_SUBMIT, nx2_reactive_flush.h).
        if (should_flush && !(real_fence && one_submit)) {
            rasterizer.FlushCommands();
            if (stats) [[unlikely]] {
                Common::Nx2Reactive::Add(Common::Nx2Reactive::Counter::FenceExtraFlushes,
                                         real_fence ? 1 : 0);
            }
        }
        if (stats) [[unlikely]] {
            Common::Nx2Reactive::Add(Common::Nx2Reactive::Counter::FencesReal, real_fence ? 1 : 0);
            Common::Nx2Reactive::Add(
                Common::Nx2Reactive::Counter::FenceSubmitNs,
                static_cast<u64>((std::max)(s64{0}, Common::Nx2Reactive::NowNs() - submit_ns)));
        }
        if constexpr (can_async_check) {
            guard.unlock();
            cv.notify_all();
        }
        rasterizer.InvalidateGPUCache();
        if (stats) [[unlikely]] {
            CountFence(start_ns);
        }
    }

    void SignalSyncPoint(u32 value) {
        // The id comes from a command list (the 3D engine's sync method, the puller's fence
        // action). Past the syncpoint table, the increment would walk memory after it as an
        // action list; command words read wrong carry any id (Splatoon Raiders on Linux).
        if (value >= Tegra::Host1x::SyncpointManager::NUM_MAX_SYNCPOINTS) {
            LOG_ERROR(HW_GPU, "Signal of syncpoint {} past the table, ignored", value);
            return;
        }
        syncpoint_manager.IncrementGuest(value);
        std::function<void()> func([this, value] { syncpoint_manager.IncrementHost(value); });
        SignalFence(std::move(func));
    }

    void WaitPendingFences([[maybe_unused]] bool force) {
        if constexpr (!can_async_check) {
            TryReleasePendingFences<true>();
        } else {
            if (!force) {
                return;
            }
            std::mutex wait_mutex;
            std::condition_variable wait_cv;
            std::atomic<bool> wait_finished{};
            std::function<void()> func([&] {
                std::scoped_lock lk(wait_mutex);
                wait_finished.store(true, std::memory_order_relaxed);
                wait_cv.notify_all();
            });
            SignalFence(std::move(func));
            std::unique_lock lk(wait_mutex);
            wait_cv.wait(
                lk, [&wait_finished] { return wait_finished.load(std::memory_order_relaxed); });
        }
    }

protected:
    explicit FenceManager(VideoCore::RasterizerInterface& rasterizer_, Tegra::GPU& gpu_,
                          TTextureCache& texture_cache_, TBufferCache& buffer_cache_,
                          TQueryCache& query_cache_)
        : rasterizer{rasterizer_}, gpu{gpu_}, syncpoint_manager{gpu.Host1x().GetSyncpointManager()},
          texture_cache{texture_cache_}, buffer_cache{buffer_cache_}, query_cache{query_cache_} {
        if constexpr (can_async_check) {
            fence_thread =
                std::jthread([this](std::stop_token token) { ReleaseThreadFunc(token); });
        }
    }

    virtual ~FenceManager() {
        if constexpr (can_async_check) {
            fence_thread.request_stop();
            cv.notify_all();
            fence_thread.join();
        }
    }

    /// Creates a Fence Interface, does not create a backend fence if 'is_stubbed' is
    /// true
    virtual TFence CreateFence(bool is_stubbed) = 0;
    /// Queues a fence into the backend if the fence isn't stubbed.
    virtual void QueueFence(TFence& fence) = 0;
    /// Notifies that the backend fence has been signaled/reached in host GPU.
    virtual bool IsFenceSignaled(TFence& fence) const = 0;
    /// Waits until a fence has been signalled by the host GPU.
    virtual void WaitFence(TFence& fence) = 0;

    VideoCore::RasterizerInterface& rasterizer;
    Tegra::GPU& gpu;
    Tegra::Host1x::SyncpointManager& syncpoint_manager;
    TTextureCache& texture_cache;
    TBufferCache& buffer_cache;
    TQueryCache& query_cache;

private:
    template <bool force_wait>
    void TryReleasePendingFences() {
        while (!fences.empty()) {
            TFence& current_fence = fences.front();
            if (ShouldWait() && !IsFenceSignaled(current_fence)) {
                if constexpr (force_wait) {
                    WaitFence(current_fence);
                } else {
                    return;
                }
            }
            PopAsyncFlushes(pending_flushes.front());
            pending_flushes.pop_front();
            auto operations = std::move(pending_operations.front());
            pending_operations.pop_front();
            for (auto& operation : operations) {
                operation();
            }
            {
                std::unique_lock lock(ring_guard);
                delayed_destruction_ring.Push(std::move(current_fence));
            }
            fences.pop();
        }
    }

    void ReleaseThreadFunc(std::stop_token stop_token) {
        Common::SetCurrentThreadName("GPUFencingThread");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
        Common::SetCurrentThreadToPerformanceCores();

        // Every fence the host GPU has already passed is released together: Switch 2 titles
        // signal thousands a second and the host GPU completes them in bursts, so each cache's
        // lock is taken once per burst instead of once per fence (the GPU thread, which holds
        // them for nearly every command, spun, slept and woke this thread for each one).
        // EDEN_NX2_FENCE_BATCH=0: one fence at a time (for comparisons).
        static const size_t MaxBatch = [] {
            const char* value = std::getenv("EDEN_NX2_FENCE_BATCH");
            return value != nullptr && value[0] == '0' ? size_t{1} : size_t{64};
        }();
        struct Released {
            TFence fence;
            std::deque<std::function<void()>> operations;
            u8 flushes;
        };
        std::vector<Released> batch;
        batch.reserve(MaxBatch);
        const auto take_front = [&] {
            batch.push_back({std::move(fences.front()), std::move(pending_operations.front()),
                             pending_flushes.front()});
            fences.pop();
            pending_operations.pop_front();
            pending_flushes.pop_front();
        };
        while (!stop_token.stop_requested()) {
            {
                std::unique_lock lock(guard);
                cv.wait(lock, [&] { return stop_token.stop_requested() || !fences.empty(); });
                if (stop_token.stop_requested()) [[unlikely]] {
                    return;
                }
                take_front();
            }
            if (!batch.front().fence->IsStubbed()) {
                WaitFence(batch.front().fence);
            }
            {
                std::unique_lock lock(guard);
                while (batch.size() < MaxBatch && !fences.empty() &&
                       (fences.front()->IsStubbed() || IsFenceSignaled(fences.front()))) {
                    take_front();
                }
            }
            size_t texture_pops = 0;
            size_t buffer_pops = 0;
            for (const Released& released : batch) {
                texture_pops += (released.flushes & TextureFlushes) != 0 ? 1 : 0;
                buffer_pops += (released.flushes & BufferFlushes) != 0 ? 1 : 0;
            }
            if (texture_pops != 0) {
                std::scoped_lock lock{texture_cache.mutex};
                for (size_t i = 0; i < texture_pops; ++i) {
                    texture_cache.PopAsyncFlushes();
                }
            }
            if (buffer_pops != 0) {
                const bool stats = Common::Nx2Reactive::StatsEnabled();
                const s64 wait_ns = stats ? Common::Nx2Reactive::NowNs() : 0;
                std::scoped_lock lock{buffer_cache.mutex};
                const s64 held_ns = stats ? Common::Nx2Reactive::NowNs() : 0;
                for (size_t i = 0; i < buffer_pops; ++i) {
                    buffer_cache.PopAsyncFlushes();
                }
                if (stats) [[unlikely]] {
                    using Common::Nx2Reactive::Counter;
                    Common::Nx2Reactive::Add(Counter::PopBufferWaitNs,
                                             static_cast<u64>((std::max)(s64{0}, held_ns - wait_ns)));
                    Common::Nx2Reactive::Add(
                        Counter::PopBufferNs,
                        static_cast<u64>((std::max)(s64{0}, Common::Nx2Reactive::NowNs() - held_ns)));
                }
            }
            // Queries and operations keep their per-fence order.
            for (Released& released : batch) {
                query_cache.PopAsyncFlushes();
                for (auto& operation : released.operations) {
                    operation();
                }
            }
            {
                std::unique_lock lock(ring_guard);
                for (Released& released : batch) {
                    delayed_destruction_ring.Push(std::move(released.fence));
                }
            }
            batch.clear();
        }
    }

    bool ShouldWait() const {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        return texture_cache.ShouldWaitAsyncFlushes() || buffer_cache.ShouldWaitAsyncFlushes() ||
               query_cache.ShouldWaitAsyncFlushes();
    }

    static constexpr u8 TextureFlushes = 1;
    static constexpr u8 BufferFlushes = 2;

    /// committed: what CommitAsyncFlushes returned for this fence.
    void PopAsyncFlushes(u8 committed) {
        if (committed & TextureFlushes) {
            std::scoped_lock lock{texture_cache.mutex};
            texture_cache.PopAsyncFlushes();
        }
        if (committed & BufferFlushes) {
            std::scoped_lock lock{buffer_cache.mutex};
            buffer_cache.PopAsyncFlushes();
        }
        query_cache.PopAsyncFlushes();
    }

    struct CacheFlushes {
        bool should_flush;  ///< Downloads to wait for: the fence must reach the host GPU
        u8 committed;       ///< TextureFlushes, BufferFlushes: the caches that committed downloads
    };

    /// Commits the texture and buffer caches' downloads for a new fence (the query cache's are
    /// committed once a fence is made). Switch 2 titles signal a fence per compute dispatch
    /// (thousands a second), mostly with nothing or only buffer data to download: each cache's
    /// lock is taken once, alone, and the fencing thread later takes only the lock of the cache
    /// with work, instead of holding both while it copies (the GPU thread's draws and uploads
    /// waited for it).
    CacheFlushes CommitCacheFlushes(bool delay_fence) {
        CacheFlushes flushes{};
        {
            std::scoped_lock lock{texture_cache.mutex};
            flushes.should_flush = texture_cache.HasUncommittedFlushes();
            if (texture_cache.HasAsyncFlushWork()) {
                texture_cache.CommitAsyncFlushes();
                flushes.committed |= TextureFlushes;
            }
        }
        {
            // Back stages, cut c: B's mutex (K3); C may commit only inside a ParkB.
            Nx2Stages::CheckBufferLock("FenceManager::CommitCacheFlushes");
            std::scoped_lock lock{buffer_cache.mutex};
            if (buffer_cache.HasUncommittedFlushes()) {
                if (coalesce_fences > 1 && !CommitBufferNow(delay_fence, flushes)) [[unlikely]] {
                    return flushes;
                }
                buffer_cache.CommitAsyncFlushes();
                flushes.should_flush = true;
                flushes.committed |= BufferFlushes;
            }
        }
        return flushes;
    }

    /// EDEN_NX2_RX_COALESCE: whether this fence commits the buffer cache's copy backs, else it
    /// leaves them for a later one (nx2_reactive_flush.h). A fence made anyway takes them along:
    /// real for another cache's downloads or query flushes, or stubbed for query fence work,
    /// queued operations or the skip limit (not with EDEN_NX2_RX_COALESCE_LOOSE).
    bool CommitBufferNow(bool delay_fence, const CacheFlushes& flushes) {
        // A fence real without the copy back, or one made stubbed (asked only when it counts:
        // EDEN_NX2_RX_COALESCE_LOOSE leaves the copy back off the latter, nx2_reactive_flush.h).
        const bool real_work = flushes.should_flush || flushes.committed != 0 ||
                               query_cache.HasUncommittedFlushes();
        const bool made_anyway = Nx2Reactive::CoalesceMadeAnyway(
            coalesce_loose, real_work, [this] { return MadeStubbedAnyway(); });
        const bool force = coalesce_force.exchange(false, std::memory_order_relaxed);
        if (Nx2Reactive::CommitBufferCopyBack(coalesce_fences, delay_fence, made_anyway, force,
                                              deferred_fences)) {
            deferred_fences = 0;
            return true;
        }
        ++deferred_fences;
        if (Common::Nx2Reactive::StatsEnabled()) [[unlikely]] {
            Common::Nx2Reactive::Add(Common::Nx2Reactive::Counter::FencesBufferDeferred, 1);
        }
        return false;
    }

    /// Whether a fence with nothing to download is made anyway (stubbed), as SkipEmptyFence
    /// decides: the skip limit, query fence work or queued operations.
    bool MadeStubbedAnyway() {
        if (skipped_fences >= 64) {
            return true;
        }
        if constexpr (requires { query_cache.HasPendingFenceWork(); }) {
            if (query_cache.HasPendingFenceWork()) {
                return true;
            }
        }
        std::scoped_lock lock{uncommitted_guard};
        return !uncommitted_operations.empty();
    }

    /// EDEN_NX2_REACTIVE_STATS: CommitCacheFlushes' time and what it committed.
    static void CountCommit(s64 start_ns, u8 committed) {
        using Common::Nx2Reactive::Counter;
        Common::Nx2Reactive::Add(
            Counter::FenceCommitNs,
            static_cast<u64>((std::max)(s64{0}, Common::Nx2Reactive::NowNs() - start_ns)));
        Common::Nx2Reactive::Add(Counter::FencesBufferCommit, (committed & BufferFlushes) ? 1 : 0);
        Common::Nx2Reactive::Add(Counter::FencesTextureCommit,
                                 (committed & TextureFlushes) ? 1 : 0);
    }

    /// EDEN_NX2_REACTIVE_STATS: a fence signaled, its whole time.
    static void CountFence(s64 start_ns) {
        using Common::Nx2Reactive::Counter;
        Common::Nx2Reactive::Add(Counter::Fences, 1);
        Common::Nx2Reactive::Add(
            Counter::FenceNs,
            static_cast<u64>((std::max)(s64{0}, Common::Nx2Reactive::NowNs() - start_ns)));
    }

    /// Whether a fence with no downloads would do nothing, so need not be made: with fast GPU
    /// fences its callback runs at once anyway, and most of the per-dispatch fences of Switch 2
    /// titles have no queries or queued operations either. Making, queueing and retiring them
    /// cost the GPU thread ~5% and woke the fencing thread for each. One in 64 is still made, so
    /// the operations fences carry along (query slot cleanup) keep running.
    bool SkipEmptyFence() {
        if constexpr (requires { query_cache.HasPendingFenceWork(); }) {
            if (skipped_fences >= 64 || query_cache.HasPendingFenceWork()) {
                return false;
            }
            {
                std::scoped_lock lock{uncommitted_guard};
                if (!uncommitted_operations.empty()) {
                    return false;
                }
            }
            ++skipped_fences;
            return true;
        } else {
            return false;
        }
    }

    std::queue<TFence> fences;
    std::deque<std::function<void()>> uncommitted_operations;
    std::deque<std::deque<std::function<void()>>> pending_operations;
    std::deque<u8> pending_flushes; ///< Per fence: the caches that committed downloads
    u32 skipped_fences{}; ///< Empty fences skipped since the last one made (GPU thread only)
    /// EDEN_NX2_RX_ONE_SUBMIT: no FlushCommands after a real fence's own submission
    const bool one_submit = Nx2Reactive::OneSubmit();
    /// EDEN_NX2_RX_COALESCE (<= 1: off): fences that may leave the buffer copy backs uncommitted
    const u64 coalesce_fences = Nx2Reactive::CoalesceFences();
    /// EDEN_NX2_RX_COALESCE_LOOSE (diagnostic): only a fence real anyway takes the copy backs
    const bool coalesce_loose = Nx2Reactive::CoalesceLoose();
    u64 deferred_fences{}; ///< Fences that left them since the last commit (GPU thread only)
    std::atomic<bool> coalesce_force{}; ///< A frame ended since (TickFrame): the next one commits

    std::mutex guard;
    std::mutex ring_guard;
    std::mutex uncommitted_guard; ///< Guards uncommitted_operations
    std::condition_variable cv;

    std::jthread fence_thread;

    DelayedDestructionRing<TFence, 8> delayed_destruction_ring;
};

} // namespace VideoCommon
