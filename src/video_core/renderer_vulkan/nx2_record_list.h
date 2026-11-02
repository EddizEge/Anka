// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/common_types.h"

// EDEN_NX2_BACK_STAGES, cut c (Kesim-2, nx2_stages.h): C is the only stage recording Vulkan
// commands. B's buffer runtime writes its commands into a record list instead, which C replays
// into the scheduler where B's packet sits in the stream (Scheduler::Replay), and stamps what it
// hands the GPU (stream and staging ranges, buffers) with B's packet seq instead of a scheduler
// tick: C notes the tick each seq went into (SeqTicks). Free of Vulkan for the unit tests
// (tests/video_core/nx2_record_list.cpp); vk_scheduler.h instantiates it. Nothing here is made
// with the stages off. Owner: T3.
namespace Vulkan::Nx2Record {

/// The tick handover (§3.6/2). C notes, in seq order, the scheduler tick each of B's seqs went
/// into; B asks whether what it stamped with a seq is done: C recorded it and the GPU passed that
/// tick. Seqs are u64 everywhere (DrawToC::seq, K5): they never wrap, "C is done with `seq`" is a
/// plain DoneSeq() >= seq.
class SeqTicks {
public:
    static constexpr u64 WINDOW = u64{1} << 14;
    /// Set in the stamps made while staged (BufferCacheRuntime::CurrentTick) to tell them from
    /// scheduler ticks, which never reach it.
    static constexpr u64 STAMP_BIT = u64{1} << 63;

    [[nodiscard]] static constexpr u64 Stamp(u64 seq) noexcept {
        return STAMP_BIT | seq;
    }
    [[nodiscard]] static constexpr bool IsStamp(u64 value) noexcept {
        return (value & STAMP_BIT) != 0;
    }
    [[nodiscard]] static constexpr u64 SeqOf(u64 stamp) noexcept {
        return stamp & ~STAMP_BIT;
    }

    /// C: `seq` and every seq before it not noted yet went into `tick`. In seq order; noting the
    /// last seq again (a packet split by a join) keeps the later tick.
    void Note(u64 seq, u64 tick) noexcept;

    /// The last seq C noted (0: none). Any thread.
    [[nodiscard]] u64 DoneSeq() const noexcept {
        return done_seq.load(std::memory_order_acquire);
    }

    /// The tick `seq` (at most DoneSeq) went into; a later one once it left the window.
    [[nodiscard]] u64 TickOf(u64 seq) const noexcept;

    /// Whether what was stamped with `seq` is done, the GPU having reached `known_gpu_tick`.
    [[nodiscard]] bool IsFree(u64 seq, u64 known_gpu_tick) const noexcept;

private:
    std::atomic<u64> done_seq{};
    std::array<std::atomic<u64>, WINDOW> ticks{};
};

/// B's memory for its record lists: 32 KiB chunks in use in allocation order. A chunk is reused
/// once every list written into it ran on the Vulkan worker (the last command group of a list
/// releases its id; ids, not seqs: a packet split by a join has two lists of one seq) and is
/// never freed before the arena.
class ChunkArena {
public:
    static constexpr size_t CHUNK_BYTES = 32 * 1024;

    ChunkArena() = default;
    ChunkArena(const ChunkArena&) = delete;
    ChunkArena& operator=(const ChunkArena&) = delete;

    /// B: an id for a new list (ids only grow; lists are replayed in their order).
    [[nodiscard]] u64 NewList() noexcept {
        return ++last_list;
    }

    /// B: `size` bytes aligned to `align` for the list `list`.
    [[nodiscard]] void* Allocate(size_t size, size_t align, u64 list);

    /// The Vulkan worker: the lists up to `list` ran, their commands are destroyed.
    void Release(u64 list) noexcept {
        released_list.store(list, std::memory_order_release);
    }

    [[nodiscard]] u64 ReleasedList() const noexcept {
        return released_list.load(std::memory_order_acquire);
    }

    /// Chunks made so far (in use and free), for the log and the tests.
    [[nodiscard]] size_t Chunks() const noexcept {
        return in_use.size() + free_chunks.size();
    }

private:
    struct Chunk {
        u64 last_list{};
        size_t used{};
        alignas(std::max_align_t) std::array<std::byte, CHUNK_BYTES> data;
    };

    std::deque<std::unique_ptr<Chunk>> in_use; ///< Oldest first; the back one is being filled
    std::vector<std::unique_ptr<Chunk>> free_chunks;
    u64 last_list{};
    std::atomic<u64> released_list{};
};

/// One packet's commands from B, in order, with the outside render pass requests between them
/// (marks). `Context` is what a command runs with (vk_scheduler.h: the two command buffers).
template <typename Context>
class List {
public:
    struct Item {
        Item* next;
        /// Runs the command with the context and destroys it; null for a mark
        void (*run)(Item* item, Context& context);
    };

    /// A run of commands between marks, run by the worker in one scheduler command. The last
    /// group of a list releases the list in its arena after running.
    struct Group {
        Item* first;
        Item* end; ///< The item after the last (a mark, or null)
        ChunkArena* release; ///< Set for the list's last group
        u64 id;    ///< The list's

        void Run(Context& context) const;
    };

    explicit List(ChunkArena& arena_) noexcept : arena{&arena_} {}

    /// B: empties the list for (a part of) the packet of `seq`.
    void Begin(u64 seq_) noexcept {
        first = nullptr;
        last = nullptr;
        seq = seq_;
        id = arena->NewList();
        commands = 0;
        marks = 0;
    }

    /// B: appends a command invocable with `Context&`.
    template <typename T>
    void Record(T&& command);

    /// B: appends an outside render pass request; one right after another is dropped.
    void MarkOutsideRenderPass();

    /// C: in order, `mark()` for each mark and `group(Group)` for each run of commands; after a
    /// trailing mark an empty group carries the release.
    template <typename MarkFunc, typename GroupFunc>
    void Replay(MarkFunc&& mark, GroupFunc&& group) const;

    [[nodiscard]] bool Empty() const noexcept {
        return first == nullptr;
    }
    [[nodiscard]] u64 Seq() const noexcept {
        return seq;
    }
    /// The list's id in its arena (Begin makes a new one).
    [[nodiscard]] u64 Id() const noexcept {
        return id;
    }
    [[nodiscard]] u32 Commands() const noexcept {
        return commands;
    }
    [[nodiscard]] u32 Marks() const noexcept {
        return marks;
    }
    [[nodiscard]] ChunkArena& Arena() const noexcept {
        return *arena;
    }

private:
    template <typename T>
    struct TypedItem final : Item {
        T command;
    };

    void Append(Item* item) noexcept;

    ChunkArena* arena;
    Item* first{};
    Item* last{};
    u64 seq{};
    u64 id{};
    u32 commands{};
    u32 marks{};
};

/// Objects B let go of (a deleted buffer, a replaced LUT, an invalidated pipeline: §3.6/7) kept
/// until what was stamped with the seq they were buried at is done. Single thread (the one owning
/// the cache, or any with it parked).
template <typename T>
class SeqGraveyard {
public:
    void Bury(T&& object, u64 seq) {
        objects.emplace_back(seq, std::move(object));
    }

    /// Frees, oldest first, those whose seq `is_free(seq)` says is done.
    template <typename IsFree>
    void Collect(IsFree&& is_free) {
        while (!objects.empty() && is_free(objects.front().first)) {
            objects.pop_front();
        }
    }

    [[nodiscard]] size_t Size() const noexcept {
        return objects.size();
    }

private:
    std::deque<std::pair<u64, T>> objects; ///< In seq order
};

/// The cache locks B holds (K3). B takes them around its work through CacheLock, which lists them
/// per thread: Count says whether B holds one (T7's FlushStageCWork writes no ring 3 record while
/// it does). Unlocked lets go of every listed one and takes them again: the buffer runtime's joins
/// used it around the record owner hook (K3 option (b)); with option (a), decided after R2, the
/// joins keep the buffer cache mutex (BufferCacheRuntime::RecordOwnerHook) and nothing uses it.
/// A cache lock B takes another way is not listed. Only for the stages; nothing takes one with
/// them off.
class HeldLocks {
public:
    static constexpr size_t MAX_HELD = 4;

    struct Entry {
        void* mutex;
        void (*lock)(void* mutex);
        bool (*try_lock)(void* mutex);
        void (*unlock)(void* mutex);
    };

    /// The cache locks this thread took through CacheLock and holds now.
    [[nodiscard]] static size_t Count() noexcept;

    /// Releases every listed lock of this thread, the last taken first (a recursive one taken
    /// twice is listed twice), and takes them all again when it goes, without assuming an order
    /// between them (as std::lock: one blocking, the others tried). In between this thread holds
    /// none of them and the list is empty: what runs meanwhile may take them again through
    /// CacheLock, as long as it lets go before this goes.
    class Unlocked {
    public:
        Unlocked() noexcept;
        ~Unlocked();
        Unlocked(const Unlocked&) = delete;
        Unlocked& operator=(const Unlocked&) = delete;

        /// How many it released.
        [[nodiscard]] size_t Released() const noexcept {
            return count;
        }

    private:
        std::array<Entry, MAX_HELD> entries{};
        size_t count{};
    };

private:
    template <typename... Mutexes>
    friend class CacheLock;

    struct List {
        std::array<Entry, MAX_HELD> entries{};
        size_t count{};
    };

    static List& ThisThread() noexcept;
    static void Push(const Entry& entry) noexcept;
    static void Pop(void* mutex) noexcept;
    static void Relock(Entry* entries, size_t count);
};

/// B (K3): takes `mutexes` (std::lock's way when there are several) and lists them for
/// HeldLocks::Unlocked; lets go of them when it goes. As std::scoped_lock otherwise.
template <typename... Mutexes>
class CacheLock {
    static_assert(sizeof...(Mutexes) >= 1 && sizeof...(Mutexes) <= HeldLocks::MAX_HELD);

public:
    explicit CacheLock(Mutexes&... mutexes) : held{EntryOf(mutexes)...} {
        if constexpr (sizeof...(Mutexes) == 1) {
            (mutexes.lock(), ...);
        } else {
            std::lock(mutexes...);
        }
        for (const HeldLocks::Entry& entry : held) {
            HeldLocks::Push(entry);
        }
    }

    ~CacheLock() {
        for (auto it = held.rbegin(); it != held.rend(); ++it) {
            HeldLocks::Pop(it->mutex);
            it->unlock(it->mutex);
        }
    }

    CacheLock(const CacheLock&) = delete;
    CacheLock& operator=(const CacheLock&) = delete;

private:
    template <typename Mutex>
    static HeldLocks::Entry EntryOf(Mutex& mutex) noexcept {
        return HeldLocks::Entry{
            .mutex = &mutex,
            .lock = [](void* lockable) { static_cast<Mutex*>(lockable)->lock(); },
            .try_lock = [](void* lockable) { return static_cast<Mutex*>(lockable)->try_lock(); },
            .unlock = [](void* lockable) { static_cast<Mutex*>(lockable)->unlock(); },
        };
    }

    std::array<HeldLocks::Entry, sizeof...(Mutexes)> held;
};

inline void SeqTicks::Note(u64 seq, u64 tick) noexcept {
    const u64 done = done_seq.load(std::memory_order_relaxed);
    if (seq < done) {
        return;
    }
    if (seq == done) {
        std::atomic<u64>& entry = ticks[seq & (WINDOW - 1)];
        if (entry.load(std::memory_order_relaxed) < tick) {
            entry.store(tick, std::memory_order_relaxed);
        }
        return;
    }
    // Seqs C never saw (B sent nothing for them) get this tick too: a later tick than theirs is
    // only later to free. Past the window, every entry is this tick.
    const u64 begin = seq - done > WINDOW ? seq - WINDOW + 1 : done + 1;
    for (u64 next = begin; next <= seq; ++next) {
        ticks[next & (WINDOW - 1)].store(tick, std::memory_order_relaxed);
    }
    done_seq.store(seq, std::memory_order_release);
}

inline u64 SeqTicks::TickOf(u64 seq) const noexcept {
    // Entries are overwritten only with later ticks (seqs WINDOW ahead), so a racing read is at
    // worst later than the seq's tick; past the window the oldest entry is later than it too.
    const u64 done = DoneSeq();
    if (done - seq >= WINDOW) {
        seq = done - WINDOW + 1;
    }
    return ticks[seq & (WINDOW - 1)].load(std::memory_order_relaxed);
}

inline bool SeqTicks::IsFree(u64 seq, u64 known_gpu_tick) const noexcept {
    return seq <= DoneSeq() && known_gpu_tick >= TickOf(seq);
}

inline void* ChunkArena::Allocate(size_t size, size_t align, u64 list) {
    if (!in_use.empty()) {
        Chunk& chunk = *in_use.back();
        const size_t offset = (chunk.used + align - 1) & ~(align - 1);
        if (offset + size <= CHUNK_BYTES) {
            chunk.used = offset + size;
            chunk.last_list = list;
            return chunk.data.data() + offset;
        }
    }
    // The back chunk is full: the oldest ones whose lists all ran become free (the back one,
    // being filled, never: it holds the newest list).
    const u64 released = ReleasedList();
    while (in_use.size() > 1 && in_use.front()->last_list <= released) {
        free_chunks.push_back(std::move(in_use.front()));
        in_use.pop_front();
    }
    if (free_chunks.empty()) {
        in_use.push_back(std::make_unique<Chunk>());
    } else {
        in_use.push_back(std::move(free_chunks.back()));
        free_chunks.pop_back();
    }
    Chunk& chunk = *in_use.back();
    chunk.used = size;
    chunk.last_list = list;
    return chunk.data.data();
}

template <typename Context>
void List<Context>::Group::Run(Context& context) const {
    for (Item* item = first; item != end;) {
        Item* const next = item->next;
        item->run(item, context);
        item = next;
    }
    if (release != nullptr) {
        release->Release(id);
    }
}

template <typename Context>
void List<Context>::Append(Item* item) noexcept {
    item->next = nullptr;
    if (last != nullptr) {
        last->next = item;
    } else {
        first = item;
    }
    last = item;
}

template <typename Context>
template <typename T>
void List<Context>::Record(T&& command) {
    using Command = std::remove_cvref_t<T>;
    using Typed = TypedItem<Command>;
    static_assert(sizeof(Typed) <= ChunkArena::CHUNK_BYTES, "Command is too large");
    static_assert(alignof(Typed) <= alignof(std::max_align_t));
    void* const memory = arena->Allocate(sizeof(Typed), alignof(Typed), id);
    Typed* const typed = new (memory) Typed{{nullptr, nullptr}, std::forward<T>(command)};
    typed->run = [](Item* item, Context& context) {
        Typed* const self = static_cast<Typed*>(item);
        self->command(context);
        self->~Typed();
    };
    Append(typed);
    ++commands;
}

template <typename Context>
void List<Context>::MarkOutsideRenderPass() {
    if (last != nullptr && last->run == nullptr) {
        return;
    }
    void* const memory = arena->Allocate(sizeof(Item), alignof(Item), id);
    Append(new (memory) Item{nullptr, nullptr});
    ++marks;
}

template <typename Context>
template <typename MarkFunc, typename GroupFunc>
void List<Context>::Replay(MarkFunc&& mark, GroupFunc&& group) const {
    // Each group's items are read here before the group is handed over, never after: the worker
    // may run and destroy them while the next ones are walked.
    Item* item = first;
    while (item != nullptr) {
        if (item->run == nullptr) {
            mark();
            item = item->next;
            continue;
        }
        Item* const begin = item;
        while (item != nullptr && item->run != nullptr) {
            item = item->next;
        }
        group(Group{begin, item, item == nullptr ? arena : nullptr, id});
    }
    if (last != nullptr && last->run == nullptr) {
        group(Group{nullptr, nullptr, arena, id});
    }
}

inline HeldLocks::List& HeldLocks::ThisThread() noexcept {
    static thread_local List list;
    return list;
}

inline size_t HeldLocks::Count() noexcept {
    return ThisThread().count;
}

inline void HeldLocks::Push(const Entry& entry) noexcept {
    List& list = ThisThread();
    if (list.count == MAX_HELD) [[unlikely]] {
        std::abort();
    }
    list.entries[list.count++] = entry;
}

inline void HeldLocks::Pop(void* mutex) noexcept {
    // Guards go in reverse order; the last listed entry of `mutex` is the guard's own.
    List& list = ThisThread();
    for (size_t index = list.count; index-- > 0;) {
        if (list.entries[index].mutex == mutex) {
            for (size_t next = index + 1; next < list.count; ++next) {
                list.entries[next - 1] = list.entries[next];
            }
            --list.count;
            return;
        }
    }
    std::abort();
}

inline void HeldLocks::Relock(Entry* entries, size_t count) {
    // std::lock's way: block on one, try the others; on a miss let all go and block on the one
    // that missed first next time.
    size_t first = 0;
    for (;;) {
        entries[first].lock(entries[first].mutex);
        size_t missed = count;
        for (size_t step = 1; step < count; ++step) {
            const size_t index = (first + step) % count;
            if (!entries[index].try_lock(entries[index].mutex)) {
                missed = index;
                for (size_t back = step; back-- > 0;) {
                    const size_t taken = (first + back) % count;
                    entries[taken].unlock(entries[taken].mutex);
                }
                break;
            }
        }
        if (missed == count) {
            return;
        }
        first = missed;
    }
}

inline HeldLocks::Unlocked::Unlocked() noexcept {
    List& list = ThisThread();
    count = list.count;
    entries = list.entries;
    list.count = 0;
    for (size_t index = count; index-- > 0;) {
        entries[index].unlock(entries[index].mutex);
    }
}

inline HeldLocks::Unlocked::~Unlocked() {
    List& list = ThisThread();
    if (list.count != 0) [[unlikely]] {
        // What ran meanwhile kept a CacheLock past this.
        std::abort();
    }
    if (count != 0) {
        Relock(entries.data(), count);
    }
    list.entries = entries;
    list.count = count;
}

} // namespace Vulkan::Nx2Record
