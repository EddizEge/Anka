// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <type_traits>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class EngineInterface;
}

namespace VideoCommon {

// Splits the GPU thread in two (NX2_GPU_THREAD_SPLIT.md). The GPU thread (the front) takes the
// CPU's commands, parses command lists and runs the puller; engine method calls and everything
// behind them (register state, macros, draws, the rasterizer and its caches) run in order on a
// second thread (the back), fed through a single-producer single-consumer ring. Work the front
// would do on back-owned state is deferred into the same ring; the front waits for the back to
// catch up (Drain) only where it needs a result from that state, e.g. a semaphore it acquires.
// On for Switch 2 titles with the 3D and compute engines' macros on the front, against copies of
// the registers they read (Maxwell3D and KeplerCompute Front*), which sends the writes they make.
// EDEN_NX2_SPLIT=1 keeps the macros on the back; EDEN_NX2_SPLIT=0 runs everything on one thread.
class GpuSplit {
public:
    static GpuSplit& Get() {
        return instance;
    }

    /// Whether the front sends work to the back (front thread only).
    [[nodiscard]] bool Active() const {
        return active;
    }
    /// Whether the front runs the engines' macros (EDEN_NX2_SPLIT=2).
    [[nodiscard]] bool FrontMacros() const {
        return front_macros;
    }

    /// Starts the back thread once the split is wanted (EDEN_NX2_SPLIT=1, a Switch 2 title,
    /// commands read without cache flushes). Called by the front between commands.
    void MaybeStart(Core::System& system);
    void Stop();

    /// An engine method write, executed as DmaPusher would (method sink, ConsumeSink, call).
    /// `segment`: the engine's current_dma_segment for the call, when the engine reads it.
    void Method(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument, bool is_last,
                const GPUVAddr* segment) {
        SelectEngine(engine);
        const size_t words = segment != nullptr ? 2 : 1;
        u64* const entry = ReserveEntry(words);
        entry[0] = static_cast<u64>(Kind::Method) | (is_last ? IsLast : 0) |
                   (segment != nullptr ? HasSegment : 0) | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        if (segment != nullptr) {
            entry[1] = *segment;
        }
        Commit(words);
    }
    void MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                     u32 amount, u32 methods_pending, const GPUVAddr* segment);
    /// A write made by a macro: CallMethod without the method sink, as the engines' macros do.
    void MacroWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument) {
        if (batch_writes) {
            // EDEN_NX2_WRITE_BATCH: held, then sent with the macro writes after it to the same
            // engine as one entry (WritePending) before anything else enters the ring, and
            // published where its own entry would have been (Count).
            if (engine != front_engine || pending_count == MaxPending) [[unlikely]] {
                FlushPending();
                SelectEngine(engine);
            }
            pending[pending_count++] = (u64{argument} << 32) | (method & 0xffff);
            Count(1);
            return;
        }
        SelectEngine(engine);
        u64* const entry = Reserve(1);
        entry[0] = static_cast<u64>(Kind::Method) | IsLast | Direct | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        Commit(1);
    }

    /// Runs `func` on the back after everything sent before it. Must be trivially copyable.
    template <typename Func>
    void Defer(const Func& func) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        u64* const entry = ReserveEntry(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload);
    }
    void DeferFunction(std::function<void()>&& func);

    /// Defer, returning where the copy of `func` went. Until Published(start) the back cannot
    /// see it: the front may still change the copy, e.g. merge a following draw into it.
    template <typename Func>
    u64* DeferEditable(const Func& func, u64& start) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        u64* const entry = ReserveEntry(2 + payload);
        // After a skip to the ring's start (ReserveSlow) too, the entry is at write_position.
        start = write_position;
        entry[0] = static_cast<u64>(Kind::Call) | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload);
        return entry + 2;
    }
    /// Front: the position the next entry goes to (held macro writes are sent first).
    [[nodiscard]] u64 WritePosition() {
        FlushPending();
        return write_position;
    }
    /// Front: whether the entry starting at `position` may already be visible to the back.
    [[nodiscard]] bool Published(u64 position) const {
        return position < published_local;
    }
    /// Front: changes with every Stop (positions and the ring start over).
    [[nodiscard]] u64 Session() const {
        return session;
    }

    /// EDEN_NX2_SPLIT_CHECK=1 (debug): reports, with its stack, a thread other than the back
    /// using back-owned state (the Vulkan scheduler) while the back runs entries.
    static inline bool check_owner{};
    static void CheckOwner(const char* where);

    /// Makes what was sent visible to the back (macro writes held too).
    void Publish();
    /// Waits until the back executed everything sent so far. Returns whether it had to wait.
    bool Drain();

private:
    enum class Kind : u64 {
        Wrap,   ///< The rest of the ring is unused: continue at its start
        Engine, ///< The engine the next methods go to: word 1
        Method, ///< Bits 8-23 method, 32-63 argument; flags IsLast, HasSegment (word 1)
        Multi,  ///< Bits 8-23 method, 32-63 amount; word 1 methods pending, [segment], words
        Call,   ///< Bits 8-31 payload size in words; word 1 thunk, then the payload
        Batch,  ///< Bits 32-63 count, then a word per macro write: bits 0-15 method, 32-63
                ///< argument (each run as a Method entry with IsLast and Direct)
    };
    static constexpr u64 IsLast = 1 << 4;
    static constexpr u64 HasSegment = 1 << 5;
    static constexpr u64 Direct = 1 << 6; ///< Method: call it even if not executable (macros)

    static constexpr size_t RingWords = size_t{1} << 20; // 8 MiB
    static constexpr size_t RingMask = RingWords - 1;
    static constexpr u32 MaxMultiWords = 8192;
    static constexpr size_t PublishEvery = 256;
    static constexpr u32 MaxPending = 64; ///< Macro writes in one Batch entry at most

    template <typename Func>
    static void CallThunk(const u64* payload) {
        (*reinterpret_cast<const Func*>(payload))();
    }

    /// Room for an entry of `words` at the write position (never across the ring's end).
    u64* Reserve(size_t words) {
        const size_t index = write_position & RingMask;
        if (index + words <= RingWords && write_position + words - consumed_cache <= RingWords)
            [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words);
    }
    u64* ReserveSlow(size_t words);
    void Commit(size_t words) {
        write_position += words;
        if (batch_writes) {
            Count(words);
        } else if (write_position - published_local >= publish_every) {
            Publish();
        }
    }
    /// EDEN_NX2_WRITE_BATCH: counts an entry's words as the ring would hold them without
    /// batching (each macro write an entry of one word; a skip to the ring's start where that
    /// ring would skip) and publishes where that ring would have been published: every write
    /// reaches the back no later than its own entry did (a batch's header word, and the ring's
    /// own skips, which come elsewhere, are not counted).
    void Count(size_t words) {
        const size_t index = unbatched_position & RingMask;
        if (index + words > RingWords) {
            unbatched_position += RingWords - index;
        }
        unbatched_position += words;
        if (unbatched_position - unbatched_published >= publish_every) {
            Publish();
        }
    }
    /// Publish without the held writes or the count (WaitForSpace: a full ring, which the
    /// ring without batching filled elsewhere).
    void PublishRing();
    /// Room for any entry but a batch: the macro writes held so far go before it.
    u64* ReserveEntry(size_t words) {
        FlushPending();
        return Reserve(words);
    }
    /// Sends the held macro writes (EDEN_NX2_WRITE_BATCH) as one Batch entry.
    void FlushPending() {
        if (pending_count != 0) {
            WritePending();
        }
    }
    void WritePending();
    void WaitForSpace(size_t words);
    void SelectEngine(Tegra::Engines::EngineInterface* engine) {
        if (engine != front_engine) [[unlikely]] {
            SwitchEngine(engine);
        }
    }
    void SwitchEngine(Tegra::Engines::EngineInterface* engine);

    void BackLoop(std::stop_token stop_token);
    /// Runs the entry; `published_left`: words published from it on. Returns its size.
    size_t Execute(const u64* entry, u64 published_left);

    Core::System* system{};
    bool active{};
    bool front_macros{};
    std::unique_ptr<u64[]> ring;

    // Front side.
    u64 write_position{};
    u64 published_local{};
    u64 consumed_cache{};
    u64 session{};
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    Tegra::Engines::EngineInterface* front_engine{};
    /// EDEN_NX2_WRITE_BATCH: macro writes to front_engine not in the ring yet (WritePending).
    std::array<u64, MaxPending> pending{};
    u32 pending_count{};
    bool batch_writes{};
    /// See Count: the write position and the last published one without batching.
    u64 unbatched_position{};
    u64 unbatched_published{};

    // Back side.
    Tegra::Engines::EngineInterface* back_engine{};

    static GpuSplit instance;

    alignas(64) std::atomic<u64> published{};
    alignas(64) std::atomic<u64> consumed{};
    alignas(64) std::atomic<bool> back_waiting{};
    std::atomic<u32> wake{};
    std::jthread back_thread;
};

} // namespace VideoCommon
