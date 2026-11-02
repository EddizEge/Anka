// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/nx2_record_list.h"

namespace {
using namespace Vulkan::Nx2Record;

struct Context {
    std::vector<int>* log;
};
using TestList = List<Context>;

constexpr int MARK = -1;

/// Replays `list` as the scheduler does, running each group at once as the worker would.
void ReplayNow(const TestList& list, Context& context, u32* groups = nullptr) {
    list.Replay([&] { context.log->push_back(MARK); },
                [&](const TestList::Group& group) {
                    group.Run(context);
                    if (groups != nullptr) {
                        ++*groups;
                    }
                });
}

struct Counted {
    std::atomic<int>* alive;
    explicit Counted(std::atomic<int>* alive_) : alive{alive_} {
        ++*alive;
    }
    Counted(const Counted& other) : alive{other.alive} {
        ++*alive;
    }
    ~Counted() {
        --*alive;
    }
};
} // Anonymous namespace

TEST_CASE("Nx2RecordList: commands replay in order, marks split them into groups", "[video_core]") {
    ChunkArena arena;
    TestList list{arena};
    list.Begin(7);
    for (int value : {1, 2}) {
        list.Record([value](Context& context) { context.log->push_back(value); });
    }
    list.MarkOutsideRenderPass();
    list.Record([](Context& context) { context.log->push_back(3); });
    list.MarkOutsideRenderPass();
    list.MarkOutsideRenderPass(); // nested: right after another, dropped
    list.Record([](Context& context) { context.log->push_back(4); });
    REQUIRE(list.Commands() == 4);
    REQUIRE(list.Marks() == 2);
    REQUIRE(list.Seq() == 7);

    std::vector<int> log;
    Context context{&log};
    u32 groups = 0;
    ReplayNow(list, context, &groups);
    REQUIRE(log == std::vector<int>{1, 2, MARK, 3, MARK, 4});
    REQUIRE(groups == 3);
    // The last group released the list.
    REQUIRE(arena.ReleasedList() != 0);
}

TEST_CASE("Nx2RecordList: leading and trailing marks, a list of marks alone", "[video_core]") {
    ChunkArena arena;
    TestList list{arena};
    list.Begin(1);
    list.MarkOutsideRenderPass();
    list.MarkOutsideRenderPass();
    list.Record([](Context& context) { context.log->push_back(5); });
    list.MarkOutsideRenderPass();
    REQUIRE(list.Marks() == 2);

    std::vector<int> log;
    Context context{&log};
    u32 groups = 0;
    ReplayNow(list, context, &groups);
    REQUIRE(log == std::vector<int>{MARK, 5, MARK});
    // The trailing mark leaves an empty group carrying the release.
    REQUIRE(groups == 2);
    const u64 first_id = arena.ReleasedList();
    REQUIRE(first_id != 0);

    list.Begin(2);
    list.MarkOutsideRenderPass();
    log.clear();
    groups = 0;
    ReplayNow(list, context, &groups);
    REQUIRE(log == std::vector<int>{MARK});
    REQUIRE(groups == 1);
    REQUIRE(arena.ReleasedList() > first_id);

    // An empty list replays nothing and releases nothing.
    list.Begin(3);
    REQUIRE(list.Empty());
    const u64 released = arena.ReleasedList();
    log.clear();
    groups = 0;
    ReplayNow(list, context, &groups);
    REQUIRE(log.empty());
    REQUIRE(groups == 0);
    REQUIRE(arena.ReleasedList() == released);
}

TEST_CASE("Nx2RecordList: commands are destroyed once, after running", "[video_core]") {
    std::atomic<int> alive{0};
    ChunkArena arena;
    TestList list{arena};
    list.Begin(1);
    for (int i = 0; i < 100; ++i) {
        list.Record([counted = Counted{&alive}, i](Context& context) {
            context.log->push_back(i);
        });
    }
    REQUIRE(alive.load() == 100);
    std::vector<int> log;
    Context context{&log};
    ReplayNow(list, context);
    REQUIRE(alive.load() == 0);
    REQUIRE(log.size() == 100);
    REQUIRE(log.front() == 0);
    REQUIRE(log.back() == 99);
}

TEST_CASE("Nx2RecordList: chunks wrap and are reused only once their lists ran", "[video_core]") {
    ChunkArena arena;
    TestList list{arena};
    std::array<u8, 1000> payload{};
    std::vector<int> log;
    Context context{&log};

    // 100 commands of ~1 KiB: four chunks, none reused while the list did not run.
    list.Begin(1);
    for (int i = 0; i < 100; ++i) {
        payload[0] = static_cast<u8>(i);
        list.Record([payload, i](Context& ctx) {
            ctx.log->push_back(payload[0] == static_cast<u8>(i) ? i : -1000);
        });
    }
    const size_t chunks = arena.Chunks();
    REQUIRE(chunks >= 4);
    std::vector<TestList> held;
    held.push_back(list);

    // A second list not released either: its chunks are new ones.
    list.Begin(2);
    for (int i = 0; i < 100; ++i) {
        list.Record([payload, i](Context& ctx) { ctx.log->push_back(payload[1] + i); });
    }
    REQUIRE(arena.Chunks() >= 2 * chunks - 1);
    held.push_back(list);

    // Both run: from then on the chunks wrap around without growing.
    for (const TestList& done : held) {
        ReplayNow(done, context);
    }
    REQUIRE(log.size() == 200);
    for (int i = 0; i < 100; ++i) {
        REQUIRE(log[i] == i);
    }
    const size_t grown = arena.Chunks();
    for (u64 seq = 3; seq < 200; ++seq) {
        list.Begin(seq);
        for (int i = 0; i < 40; ++i) {
            list.Record([payload, i](Context& ctx) { ctx.log->push_back(payload[2] + i); });
        }
        log.clear();
        ReplayNow(list, context);
        REQUIRE(log.size() == 40);
        REQUIRE(log[39] == 39);
    }
    REQUIRE(arena.Chunks() == grown);
}

TEST_CASE("Nx2RecordList: a packet split by a join keeps its second list's chunks", "[video_core]") {
    // The first part runs (releasing its id) while the second part, of the same seq, is still
    // being recorded: the second part's chunks must stay.
    ChunkArena arena;
    TestList list{arena};
    std::array<u8, 3000> payload{};
    std::vector<int> log;
    Context context{&log};

    list.Begin(5);
    list.Record([](Context& ctx) { ctx.log->push_back(1); });
    const TestList first = list;
    list.Begin(5); // the continuation after the join
    for (int i = 0; i < 30; ++i) {
        payload[0] = static_cast<u8>(i);
        list.Record([payload, i](Context& ctx) {
            ctx.log->push_back(payload[0] == static_cast<u8>(i) ? 100 + i : -1);
        });
        if (i == 15) {
            ReplayNow(first, context); // the worker runs the first part meanwhile
        }
    }
    ReplayNow(list, context);
    REQUIRE(log.size() == 31);
    REQUIRE(log[0] == 1);
    for (int i = 0; i < 30; ++i) {
        REQUIRE(log[1 + i] == 100 + i);
    }
}

TEST_CASE("Nx2RecordList: B records while the worker runs, chunks recycled", "[video_core]") {
    // B (this thread) fills lists; a second thread replays and runs them in order, as C and the
    // Vulkan worker would; every command checks its payload survived. B stays at most 64 lists
    // ahead, as ring 3 would keep it.
    ChunkArena arena;
    constexpr u64 LISTS = 20000;
    constexpr u64 AHEAD = 64;
    std::vector<TestList> lists(LISTS, TestList{arena});
    std::atomic<u64> published{0};
    std::atomic<u64> consumed{0};
    std::atomic<int> bad{0};
    std::atomic<u64> ran{0};

    std::jthread worker{[&] {
        std::vector<int> log;
        Context context{&log};
        for (u64 index = 0; index < LISTS; ++index) {
            while (published.load(std::memory_order_acquire) <= index) {
                std::this_thread::yield();
            }
            log.clear();
            ReplayNow(lists[index], context);
            for (size_t i = 0; i < log.size(); ++i) {
                if (log[i] != MARK && log[i] != static_cast<int>((index * 31 + i) & 0xffff)) {
                    ++bad;
                }
            }
            ran.fetch_add(log.size(), std::memory_order_relaxed);
            consumed.store(index + 1, std::memory_order_release);
        }
    }};
    u64 expected = 0;
    for (u64 index = 0; index < LISTS; ++index) {
        while (index - consumed.load(std::memory_order_acquire) >= AHEAD) {
            std::this_thread::yield();
        }
        TestList& list = lists[index];
        list.Begin(index + 1);
        const int commands = static_cast<int>(index % 7) + 1;
        size_t position = 0;
        for (int i = 0; i < commands; ++i) {
            if (i == 3) {
                list.MarkOutsideRenderPass();
                ++position;
            }
            std::array<u32, 64> payload;
            payload.fill(static_cast<u32>((index * 31 + position) & 0xffff));
            list.Record([payload](Context& ctx) {
                const u32 value = payload[0];
                for (u32 word : payload) {
                    if (word != value) {
                        ctx.log->push_back(-2);
                        return;
                    }
                }
                ctx.log->push_back(static_cast<int>(value));
            });
            ++position;
        }
        expected += position;
        published.store(index + 1, std::memory_order_release);
    }
    worker.join();
    REQUIRE(bad.load() == 0);
    REQUIRE(ran.load() == expected);
    // ~20 MiB of commands went through a few chunks (64 lists of at most ~2 KiB in flight).
    REQUIRE(arena.Chunks() <= 8);
}

TEST_CASE("Nx2RecordList: seq ticks note in order, fill gaps and free conservatively", "[video_core]") {
    SeqTicks ticks;
    REQUIRE(ticks.DoneSeq() == 0);
    REQUIRE(!ticks.IsFree(1, 1000));
    ticks.Note(1, 10);
    ticks.Note(4, 12); // 2 and 3 never reached C: they get 12
    REQUIRE(ticks.DoneSeq() == 4);
    REQUIRE(ticks.TickOf(1) == 10);
    REQUIRE(ticks.TickOf(2) == 12);
    REQUIRE(ticks.TickOf(4) == 12);
    REQUIRE(ticks.IsFree(1, 10));
    REQUIRE(!ticks.IsFree(2, 11));
    REQUIRE(ticks.IsFree(4, 12));
    REQUIRE(!ticks.IsFree(5, 1000)); // not recorded yet
    // Noting the last seq again (a split packet) keeps the later tick; an older one is ignored.
    ticks.Note(4, 15);
    REQUIRE(ticks.TickOf(4) == 15);
    ticks.Note(4, 13);
    REQUIRE(ticks.TickOf(4) == 15);
    ticks.Note(3, 99);
    REQUIRE(ticks.TickOf(3) == 12);
    // Past the window a seq's tick reads as the oldest kept one: later, never earlier.
    for (u64 seq = 5; seq < 5 + SeqTicks::WINDOW + 10; ++seq) {
        ticks.Note(seq, 100 + seq);
    }
    REQUIRE(ticks.TickOf(1) >= 10);
    REQUIRE(ticks.TickOf(1) <= 100 + ticks.DoneSeq());
    REQUIRE(ticks.IsFree(1, 100 + ticks.DoneSeq()));
    REQUIRE(!ticks.IsFree(1, 50));
    // A jump of more than the window fills it all.
    const u64 done = ticks.DoneSeq();
    ticks.Note(done + 3 * SeqTicks::WINDOW, 9000);
    REQUIRE(ticks.TickOf(done + 3 * SeqTicks::WINDOW - 1) == 9000);
    REQUIRE(ticks.TickOf(done + 1) == 9000);
}

TEST_CASE("Nx2RecordList: stamps and u64 seqs", "[video_core]") {
    REQUIRE(SeqTicks::IsStamp(SeqTicks::Stamp(5)));
    REQUIRE(!SeqTicks::IsStamp(5));
    REQUIRE(SeqTicks::SeqOf(SeqTicks::Stamp(0x1234'5678'9abcULL)) == 0x1234'5678'9abcULL);
    // Seqs past 32 bits are plain u64s (K5): no wrap anywhere.
    SeqTicks ticks;
    const u64 low = 0xffff'fff0ULL;
    ticks.Note(low, 7);
    ticks.Note(0x1'0000'0002ULL, 8);
    REQUIRE(ticks.DoneSeq() == 0x1'0000'0002ULL);
    REQUIRE(ticks.TickOf(low) == 7);
    REQUIRE(ticks.TickOf(0x1'0000'0000ULL) == 8);
    REQUIRE(ticks.IsFree(0x1'0000'0002ULL, 8));
    REQUIRE(!ticks.IsFree(0x1'0000'0003ULL, 1000));
    REQUIRE(!ticks.IsFree(0x2'0000'0002ULL, 1000));
}

TEST_CASE("Nx2RecordList: the graveyard frees in seq order", "[video_core]") {
    std::atomic<int> alive{0};
    SeqGraveyard<Counted> graveyard;
    graveyard.Bury(Counted{&alive}, 3);
    graveyard.Bury(Counted{&alive}, 5);
    graveyard.Bury(Counted{&alive}, 9);
    REQUIRE(alive.load() == 3);
    graveyard.Collect([](u64 seq) { return seq <= 5; });
    REQUIRE(graveyard.Size() == 1);
    REQUIRE(alive.load() == 1);
    graveyard.Collect([](u64 seq) { return seq <= 8; });
    REQUIRE(graveyard.Size() == 1);
    graveyard.Collect([](u64) { return true; });
    REQUIRE(alive.load() == 0);
}
