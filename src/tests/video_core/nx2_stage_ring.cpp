// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_stage_ring.h"

namespace {
using namespace VideoCommon::Nx2Stages;

/// Ends the test run when a test hangs (a deadlock), instead of waiting forever: after
/// NX2_TEST_WATCHDOG seconds (default 120).
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

/// Checks made on the stages' threads (the test's REQUIRE is for the main thread): counted,
/// the first few printed, required to be none at the end.
std::atomic<u32> g_failures{};
void Expect(bool ok, const char* what, u64 a = 0, u64 b = 0) {
    if (!ok && g_failures.fetch_add(1) < 8) {
        std::printf("  check failed: %s (%llu, %llu)\n", what, static_cast<unsigned long long>(a),
                    static_cast<unsigned long long>(b));
    }
}

void Spin(u32 count) {
    for (u32 i = 0; i < count; ++i) {
        RingPause();
    }
}

struct CbWrite {
    u64 address;
    std::vector<u32> data;
};

/// The stages' handler of the tests: logs what each stage got.
struct FakeHandler {
    using Scopes = RingWaitScopes;

    struct Log {
        std::vector<u64> deltas;
        u64 target{};
        std::vector<CbWrite> cb;
        std::vector<std::pair<u64, u64>> exec;
        u64 last_mark{};
        u32 marks{};
        u32 caught_up_marks{};
        u32 verifies{};
    };

    /// A's register capture (the main thread is A).
    std::vector<u64> capture;
    std::array<Log, 3> logs{};
    std::atomic<u32> threads{};
    std::atomic<u32> bad_sequences{};
    std::atomic<u32> parks{};
    /// Called by Mark (the last stage), if set.
    void (*on_mark)(u64 position){};
    /// Called by Verify, if set.
    void (*on_verify)(u8 stage){};

    u32 PendingDeltas() {
        return static_cast<u32>(capture.size());
    }
    void TakeDeltas(u64* out, u32 count) {
        Expect(count == capture.size(), "TakeDeltas takes everything", count, capture.size());
        std::memcpy(out, capture.data(), size_t{count} * sizeof(u64));
        capture.clear();
    }
    void Apply(u8 stage, const u64* words, u32 count) {
        logs[stage].deltas.insert(logs[stage].deltas.end(), words, words + count);
    }
    void Target(u8 stage, u64 primary) {
        logs[stage].target = primary;
    }
    void CbData(u8 stage, u64 address, const u32* data, u32 count) {
        logs[stage].cb.push_back({address, std::vector<u32>(data, data + count)});
    }
    void ExecA(u64 position, u64 words) {
        logs[StageIdB].exec.emplace_back(position, words);
    }
    void Mark(u8 stage, u64 position, bool caught_up) {
        Log& log = logs[stage];
        Expect(position > log.last_mark, "marks reach consumed in order", position, log.last_mark);
        log.last_mark = position;
        ++log.marks;
        log.caught_up_marks += caught_up ? 1 : 0;
        if (on_mark != nullptr) {
            on_mark(position);
        }
    }
    void Verify(u8 stage) {
        ++logs[stage].verifies;
        if (on_verify != nullptr) {
            on_verify(stage);
        }
    }
    void BadSequence(u8, u16, u16) {
        ++bad_sequences;
    }
    void ThreadStart(u8) {
        ++threads;
    }
    void Report(u8) {}
    void Busy(u8, bool) {}
    void Wrote(u8, size_t) {}
    void Parked() {
        ++parks;
    }
};

/// Small rings: records wrap and producers wait for room.
using TestPipeline = StagePipeline<FakeHandler, 4096, 2048>;

FakeHandler* g_handler{};
TestPipeline* g_pipe{};

/// What the Call records ran, in order, on B and on C (each list written by its stage only).
std::vector<u64> g_ran_b;
std::vector<u64> g_ran_c;
/// Deltas B had applied when each call ran.
std::vector<size_t> g_deltas_at_call;

void ResetGlobals(FakeHandler& handler, TestPipeline& pipe) {
    g_handler = &handler;
    g_pipe = &pipe;
    g_ran_b.clear();
    g_ran_c.clear();
    g_deltas_at_call.clear();
    g_failures = 0;
}

void RanOnB(const u64* payload, u32 words) {
    Expect(words == 1, "a call gets its payload's size", words, 1);
    g_ran_b.push_back(payload[0]);
    g_deltas_at_call.push_back(g_handler->logs[StageIdB].deltas.size());
}

void RanOnC(const u64* payload, u32 words) {
    Expect(words >= 1, "a call gets its payload's size", words, 1);
    g_ran_c.push_back(payload[0]);
}

void PostCallB(TestPipeline& pipe, RecordThunk thunk, u64 value) {
    u64* const body = pipe.BeginB(RecordKind::Call, 2);
    body[0] = reinterpret_cast<u64>(thunk);
    body[1] = value;
    pipe.EndB();
}

/// From B: a call record for C.
void PostCallC(TestPipeline& pipe, RecordThunk thunk, u64 value, u32 extra_words = 0) {
    u64* const body = pipe.BeginC(RecordKind::Call, 2 + extra_words);
    body[0] = reinterpret_cast<u64>(thunk);
    body[1] = value;
    for (u32 i = 0; i < extra_words; ++i) {
        body[2 + i] = value + i;
    }
    pipe.EndC();
}

void PostMarkB(TestPipeline& pipe, u64 position, bool caught_up) {
    u64* const body = pipe.BeginB(RecordKind::Mark, 1, caught_up ? MarkCaughtUp : 0);
    body[0] = position;
    pipe.EndB();
}

/// A: everything sent ran on the stages (a join with nothing to do).
void Drain(TestPipeline& pipe) {
    pipe.JoinAEnd(pipe.JoinABegin());
}

} // Anonymous namespace

TEST_CASE("Nx2StageRing: a record header keeps its fields apart", "[video_core]") {
    const u64 header = MakeHeader(RecordKind::Forward, 0xabcd, 0xd5b, 0xfedc, 0x5);
    REQUIRE(HeaderKind(header) == RecordKind::Forward);
    REQUIRE(HeaderFlags(header) == 0x5);
    REQUIRE(HeaderWords(header) == 0xabcd);
    REQUIRE(HeaderDeltaWords(header) == 0xd5b);
    REQUIRE(HeaderSeq(header) == 0xfedc);
}

TEST_CASE("Nx2StageRing: the largest record fits its header and a quarter ring", "[video_core]") {
    // MakeHeader stops on a field that does not fit (K9); the largest one keeps every field.
    constexpr u64 largest =
        MakeHeader(RecordKind::CbData, MaxRecordWords, MaxDeltaWords, 0xffff, 0xf);
    REQUIRE(HeaderKind(largest) == RecordKind::CbData);
    REQUIRE(HeaderWords(largest) == MaxRecordWords);
    REQUIRE(HeaderDeltaWords(largest) == MaxDeltaWords);
    REQUIRE(HeaderSeq(largest) == 0xffff);
    REQUIRE(HeaderFlags(largest) == 0xf);
    static_assert(MaxRecordWords <= 0xffffff && MaxDeltaWords <= 0xffff);
    static_assert(size_t{MaxRecordWords} * 4 <= RingToCWords && RingToCWords <= RingToBWords);
    static_assert(MaxDeltaWords + MaxBodyWords == MaxRecordWords);
}

TEST_CASE("Nx2StageRing: sequence numbers compare across a wrap", "[video_core]") {
    REQUIRE(SeqAtLeast<u16>(5, 5));
    REQUIRE(SeqAtLeast<u16>(6, 5));
    REQUIRE(!SeqAtLeast<u16>(5, 6));
    REQUIRE(SeqAtLeast<u16>(2, 0xfffe));
    REQUIRE(!SeqAtLeast<u16>(0xfffe, 2));
    REQUIRE(SeqAtLeast<u32>(3, 0xffffff00u));
    REQUIRE(!SeqAtLeast<u32>(0xffffff00u, 3));
    // Half the range apart or more: the older one reads as later (K5).
    REQUIRE(!SeqAtLeast<u32>(0x80000000u, 0));
    REQUIRE(SeqAtLeast<u64>(u64{1} << 40, 7));
    static_assert(SeqAtLeast<u16>(1, 0xffff) && !SeqAtLeast<u16>(0xffff, 1));
}

TEST_CASE("Nx2StageRing: 10^6 records keep their order across wraps, publishes and wakes",
          "[video_core]") {
    Watchdog watchdog{"ring order"};
    using Ring = StageRing<4096>;
    Ring ring;
    constexpr u32 Records = 1'000'000;
    const auto size_of = [](u32 n) { return (n * 7919U) % 300U + 1U; };
    const auto word_of = [](u32 n, u32 i) { return (u64{n} << 32) | (i * 2654435761U); };
    std::atomic<u32> errors{};
    std::atomic<u32> consumer_sleeps{};
    std::jthread consumer{[&] {
        u64 read = 0;
        u64 available = 0;
        u64 handed = 0;
        u32 expected = 0;
        while (expected < Records) {
            if (read == available) {
                available = ring.WaitForWork(read, [] { return false; });
                continue;
            }
            const u64* const record = ring.Read(read);
            if (HeaderKind(record[0]) == RecordKind::Wrap) {
                read = Ring::AfterWrap(read);
                continue;
            }
            const u32 words = HeaderWords(record[0]);
            if (HeaderKind(record[0]) != RecordKind::Call || words != size_of(expected) ||
                HeaderSeq(record[0]) != static_cast<u16>(expected + 1)) {
                ++errors;
            }
            for (u32 i = 0; i < words; ++i) {
                if (record[1 + i] != word_of(expected, i)) {
                    ++errors;
                }
            }
            read += 1 + words;
            ++expected;
            if (read - handed >= 256) {
                ring.Release(read);
                handed = read;
            }
            if (expected % 200'000 == 0 && expected < Records) {
                // Let the producer fill the ring and sleep waiting for room.
                ++consumer_sleeps;
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
        }
        ring.Release(read);
    }};
    for (u32 n = 0; n < Records; ++n) {
        const u32 words = size_of(n);
        u64* const record = ring.Reserve(1 + words);
        record[0] = MakeHeader(RecordKind::Call, words, 0, static_cast<u16>(n + 1));
        for (u32 i = 0; i < words; ++i) {
            record[1 + i] = word_of(n, i);
        }
        ring.Commit(1 + words);
        if (n % 1000 == 999) {
            ring.Publish();
        }
        if (n % 250'000 == 0) {
            // Let the consumer catch up and sleep waiting for records.
            ring.Publish();
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }
    ring.Publish();
    consumer.join();
    REQUIRE(errors == 0);
    REQUIRE(consumer_sleeps == 4);
}

TEST_CASE("Nx2StagePipeline: nothing starts with EDEN_NX2_BACK_STAGES=0", "[video_core]") {
    REQUIRE(ParseStageOptions(nullptr, nullptr, nullptr, nullptr).mode == 0);
    REQUIRE(ParseStageOptions("0", "ac", "1", "4").mode == 0);
    REQUIRE(ParseStageOptions("x", nullptr, nullptr, nullptr).mode == 0);
    REQUIRE(ParseStageOptions("4", nullptr, nullptr, nullptr).mode == 0);
    const StageOptions on = ParseStageOptions("1", nullptr, nullptr, nullptr);
    REQUIRE((on.mode == 1 && !on.cut_c && !on.check && on.verify_every == 1));
    const StageOptions verify = ParseStageOptions("2", "ac", "1", "16");
    REQUIRE((verify.mode == 2 && verify.cut_c && verify.check && verify.verify_every == 16));
    // =3 runs everything on B: never cut c.
    REQUIRE(!ParseStageOptions("3", "ac", nullptr, nullptr).cut_c);

    FakeHandler handler;
    TestPipeline pipe{handler};
    REQUIRE(!pipe.Start(ParseStageOptions("0", "ac", nullptr, nullptr)));
    REQUIRE(!pipe.Running());
    pipe.Stop();
    REQUIRE(handler.threads == 0);
    // On: B (and C with cut c) start, and stop.
    REQUIRE(pipe.Start(ParseStageOptions("1", "ac", nullptr, nullptr)));
    pipe.Stop();
    REQUIRE(handler.threads == 2);
}

TEST_CASE("Nx2StagePipeline: A's deltas ride in the next record, before it, in order",
          "[video_core]") {
    Watchdog watchdog{"embedded deltas"};
    FakeHandler handler;
    TestPipeline pipe{handler};
    ResetGlobals(handler, pipe);
    REQUIRE(pipe.Start(StageOptions{.mode = 1, .check = true}));
    std::vector<u64> sent;
    std::vector<size_t> sent_before_call;
    constexpr u32 Calls = 20'000;
    for (u32 n = 0; n < Calls; ++n) {
        // 0 to 6 delta words, sometimes more than a record holds after it (wraps).
        const u32 count = n % 7;
        for (u32 i = 0; i < count; ++i) {
            const u64 word = (u64{n} << 32) | (i << 16) | (n & 0xff);
            handler.capture.push_back(word);
            sent.push_back(word);
        }
        sent_before_call.push_back(sent.size());
        PostCallB(pipe, &RanOnB, n);
        if (n % 97 == 0) {
            // Deltas ride in marks too.
            handler.capture.push_back(u64{0xdead} << 32 | n);
            sent.push_back(u64{0xdead} << 32 | n);
            PostMarkB(pipe, n + 1, false);
        }
    }
    Drain(pipe);
    pipe.Stop();
    REQUIRE(g_failures == 0);
    REQUIRE(handler.bad_sequences == 0);
    REQUIRE(g_ran_b.size() == Calls);
    for (u32 n = 0; n < Calls; ++n) {
        REQUIRE(g_ran_b[n] == n);
        REQUIRE(g_deltas_at_call[n] == sent_before_call[n]);
    }
    REQUIRE(handler.logs[StageIdB].deltas == sent);
}

TEST_CASE("Nx2StagePipeline: CbData joins the record before it only while it may", "[video_core]") {
    Watchdog watchdog{"cb data"};
    FakeHandler handler;
    TestPipeline pipe{handler};
    ResetGlobals(handler, pipe);
    REQUIRE(pipe.Start(StageOptions{.mode = 1}));
    std::array<u32, 64> data{};
    for (u32 i = 0; i < data.size(); ++i) {
        data[i] = 0x1000 + i;
    }
    // 3 words, then 2, then 1 after them (odd sizes): one record. A notes const_buffer.offset
    // after each write (one pending delta): still joined.
    pipe.PostCbData(0x1000, &data[0], 3);
    handler.capture = {0x111};
    pipe.PostCbData(0x100C, &data[3], 2);
    pipe.PostCbData(0x1014, &data[5], 1);
    // Elsewhere: a new record (taking the pending delta).
    pipe.PostCbData(0x2000, &data[6], 1);
    // Two pending deltas (another register written in between): a new record.
    handler.capture = {0x222, 0x333};
    pipe.PostCbData(0x2004, &data[7], 2);
    // Published: a new record.
    pipe.PublishB();
    pipe.PostCbData(0x200C, &data[9], 4);
    // Anything else in between: a new record.
    PostCallB(pipe, &RanOnB, 7);
    pipe.PostCbData(0x201C, &data[13], 1);
    Drain(pipe);
    pipe.Stop();
    REQUIRE(g_failures == 0);
    const auto& cb = handler.logs[StageIdB].cb;
    REQUIRE(cb.size() == 5);
    REQUIRE(cb[0].address == 0x1000);
    REQUIRE(cb[0].data == std::vector<u32>(&data[0], &data[6]));
    REQUIRE(cb[1].address == 0x2000);
    REQUIRE(cb[1].data == std::vector<u32>(&data[6], &data[7]));
    REQUIRE(cb[2].address == 0x2004);
    REQUIRE(cb[2].data == std::vector<u32>(&data[7], &data[9]));
    REQUIRE(cb[3].address == 0x200C);
    REQUIRE(cb[3].data == std::vector<u32>(&data[9], &data[13]));
    REQUIRE(cb[4].address == 0x201C);
    REQUIRE(handler.logs[StageIdB].deltas == std::vector<u64>({0x111, 0x222, 0x333}));
    REQUIRE(g_ran_b == std::vector<u64>({7}));
}

namespace {
std::atomic<u32> g_b_ran_count{};
void CountOnB(const u64* payload, u32) {
    g_b_ran_count.store(static_cast<u32>(payload[0]) + 1, std::memory_order_relaxed);
}
/// On B: sends C a call record.
void SendToC(const u64* payload, u32) {
    PostCallC(*g_pipe, &RanOnC, payload[0]);
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: a join from A parks B (and C) after everything before it",
          "[video_core]") {
    Watchdog watchdog{"join from A"};
    for (const bool cut_c : {false, true}) {
        FakeHandler handler;
        TestPipeline pipe{handler};
        ResetGlobals(handler, pipe);
        g_b_ran_count = 0;
        REQUIRE(pipe.Start(StageOptions{.mode = 1, .cut_c = cut_c}));
        for (u32 round = 0; round < 2000; ++round) {
            PostCallB(pipe, &CountOnB, round);
            if (cut_c) {
                PostCallB(pipe, &SendToC, round);
            }
            const u32 join = pipe.JoinABegin();
            // Everything before the join ran, B's records for C too.
            REQUIRE(g_b_ran_count.load(std::memory_order_relaxed) == round + 1);
            if (cut_c) {
                REQUIRE(g_ran_c.size() == round + 1);
            }
            if (round % 500 == 0) {
                // Records sent during the join wait for its end.
                PostCallB(pipe, &CountOnB, round + 1000);
                pipe.PublishB();
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
                REQUIRE(g_b_ran_count.load(std::memory_order_relaxed) == round + 1);
                pipe.JoinAEnd(join);
                Drain(pipe);
                REQUIRE(g_b_ran_count.load(std::memory_order_relaxed) == round + 1001);
                continue;
            }
            pipe.JoinAEnd(join);
        }
        Drain(pipe);
        pipe.Stop();
        REQUIRE(g_failures == 0);
        REQUIRE(handler.bad_sequences == 0);
    }
}

namespace {
u32 g_committed{};
std::atomic<u32> g_lockstep_ran{};
void LockstepCall(const u64* payload, u32) {
    // A waits for this record: nothing after it was sent yet.
    Expect(payload[0] == g_committed, "lockstep: A waits for the record", payload[0], g_committed);
    g_lockstep_ran.store(static_cast<u32>(payload[0]) + 1, std::memory_order_relaxed);
}
void LockstepVerify(u8) {
    // A is still blocked in its EndB while the stage verifies.
    Expect(g_lockstep_ran.load(std::memory_order_relaxed) == g_committed + 1,
           "lockstep: verify while A waits", g_lockstep_ran.load(), g_committed);
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: =2 runs each record before A goes on, a join too (S8)",
          "[video_core]") {
    Watchdog watchdog{"lockstep"};
    for (const bool cut_c : {false, true}) {
        FakeHandler handler;
        TestPipeline pipe{handler};
        ResetGlobals(handler, pipe);
        handler.on_verify = &LockstepVerify;
        g_lockstep_ran = 0;
        REQUIRE(pipe.Start(StageOptions{.mode = 2, .cut_c = cut_c, .verify_every = 3}));
        constexpr u32 Calls = 3000;
        for (u32 n = 0; n < Calls; ++n) {
            g_committed = n;
            PostCallB(pipe, &LockstepCall, n);
            // Depth 1: B ran it before EndB returned.
            REQUIRE(g_lockstep_ran.load(std::memory_order_relaxed) == n + 1);
            if (n % 10 == 9) {
                // A join in lockstep waits for the park ack, not the record's end (S8).
                const u32 join = pipe.JoinABegin();
                REQUIRE(g_lockstep_ran.load(std::memory_order_relaxed) == n + 1);
                pipe.JoinAEnd(join);
            }
        }
        Drain(pipe);
        pipe.Stop();
        REQUIRE(g_failures == 0);
        REQUIRE(handler.bad_sequences == 0);
        // Every third record but the joins: calls and the joins' own are counted apart.
        REQUIRE(handler.logs[StageIdB].verifies == Calls / 3);
    }
}

TEST_CASE("Nx2StagePipeline: marks reach the last stage in order", "[video_core]") {
    Watchdog watchdog{"marks"};
    for (const bool cut_c : {false, true}) {
        FakeHandler handler;
        TestPipeline pipe{handler};
        ResetGlobals(handler, pipe);
        REQUIRE(pipe.Start(StageOptions{.mode = 1, .cut_c = cut_c, .check = true}));
        u64 position = 0;
        u32 caught_up = 0;
        for (u32 n = 0; n < 50'000; ++n) {
            position += 1 + n % 13;
            const bool batch_end = n % 5 == 0;
            caught_up += batch_end ? 1 : 0;
            PostMarkB(pipe, position, batch_end);
            if (n % 3 == 0) {
                PostCallB(pipe, &RanOnB, n);
            }
        }
        Drain(pipe);
        pipe.Stop();
        const auto& log = handler.logs[cut_c ? StageIdC : StageIdB];
        REQUIRE(g_failures == 0);
        REQUIRE(handler.bad_sequences == 0);
        REQUIRE(log.marks == 50'000);
        REQUIRE(log.caught_up_marks == caught_up);
        REQUIRE(log.last_mark == position);
        REQUIRE(handler.logs[cut_c ? StageIdB : StageIdC].marks == 0);
    }
}

namespace {
// JoinFromB and ParkB at once (S13).
std::atomic<bool> g_c_in_tick{};
std::atomic<bool> g_tick_done{};
std::atomic<u32> g_spin_b{};
std::atomic<u32> g_spin_c{};
std::vector<char> g_order;
std::mutex g_order_mutex;
u32 g_parks_before{};
u32 g_extra_records{};
u8 g_mode{};

void Order(char what) {
    std::scoped_lock lock{g_order_mutex};
    g_order.push_back(what);
}

void TickOnC(const u64*, u32) {
    g_c_in_tick.store(true, std::memory_order_seq_cst);
    Spin(g_spin_c.load(std::memory_order_relaxed));
    g_pipe->ParkB([] {
        // B parked for C.
        Expect(g_handler->parks.load() == g_parks_before + 1, "ParkB: B parked",
               g_handler->parks.load(), g_parks_before);
        Order('c');
    });
    g_tick_done.store(true, std::memory_order_seq_cst);
}

void MarkAfterTick(u64) {
    // A mark C met inside the tick (running up to B's join) is stored once the tick ended.
    Expect(g_tick_done.load(std::memory_order_seq_cst), "a mark met inside a record waits for it");
}

/// On B: C gets a tick (which asks B to park), a mark, maybe more than ring 3 holds; then B joins
/// C (a JoinFromB) while C waits for B to park.
void TickAndJoinOnB(const u64* payload, u32) {
    PostCallC(*g_pipe, &TickOnC, payload[0]);
    u64* const body = g_pipe->BeginC(RecordKind::Mark, 1);
    body[0] = payload[0] + 1;
    g_pipe->EndC();
    g_pipe->PublishC();
    for (u32 i = 0; i < g_extra_records; ++i) {
        // Ring 3 fills while C waits in ParkB: B parks for it where it waits for room.
        PostCallC(*g_pipe, &RanOnC, i, 200);
    }
    if (g_extra_records != 0) {
        return;
    }
    Spin(g_spin_b.load(std::memory_order_relaxed));
    const u32 join = g_pipe->JoinBBegin();
    if (g_mode == 1) {
        // C parked at the join: inside its tick, which waits for B.
        Expect(g_c_in_tick.load() && !g_tick_done.load(), "JoinFromB: C parked inside its tick");
    }
    Order('b');
    g_pipe->JoinBEnd(join);
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: JoinFromB and ParkB asked at once (S13)", "[video_core]") {
    Watchdog watchdog{"JoinFromB and ParkB"};
    std::mt19937 random{1234};
    for (const u8 mode : {u8{1}, u8{2}}) {
        for (u32 round = 0; round < 300; ++round) {
            FakeHandler handler;
            TestPipeline pipe{handler};
            ResetGlobals(handler, pipe);
            handler.on_mark = &MarkAfterTick;
            g_order.clear();
            g_c_in_tick = false;
            g_tick_done = false;
            g_parks_before = 0;
            // Who asks first varies: B joining before or after C's tick asks B to park.
            g_spin_b = random() % 20000;
            g_spin_c = random() % 20000;
            g_extra_records = round % 3 == 2 ? 40 : 0;
            g_mode = mode;
            REQUIRE(pipe.Start(StageOptions{.mode = mode, .cut_c = true}));
            PostCallB(pipe, &TickAndJoinOnB, 100);
            PostMarkB(pipe, 1000, true);
            Drain(pipe);
            pipe.Stop();
            REQUIRE(g_failures == 0);
            REQUIRE(g_tick_done);
            if (g_extra_records == 0 && mode == 1) {
                // B's join first (C ran up to it inside its tick), then C's park.
                REQUIRE(g_order == std::vector<char>({'b', 'c'}));
            } else if (g_extra_records == 0) {
                // =2: B waits for C to run the tick, parking for it meanwhile; then it joins.
                REQUIRE(g_order == std::vector<char>({'c', 'b'}));
            } else {
                REQUIRE(g_order == std::vector<char>({'c'}));
                REQUIRE(g_ran_c.size() == g_extra_records);
            }
            REQUIRE(handler.parks == 1);
            REQUIRE(handler.logs[StageIdC].last_mark == 1000);
        }
    }
}

namespace {
void TickOnly(const u64*, u32) {
    g_pipe->ParkB([] { Order('c'); });
    g_tick_done.store(true, std::memory_order_seq_cst);
}
void SendTick(const u64*, u32) {
    PostCallC(*g_pipe, &TickOnly, 0);
    g_pipe->PublishC();
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: ParkB wakes a B waiting for records", "[video_core]") {
    Watchdog watchdog{"ParkB wakes B"};
    FakeHandler handler;
    TestPipeline pipe{handler};
    ResetGlobals(handler, pipe);
    g_order.clear();
    g_tick_done = false;
    REQUIRE(pipe.Start(StageOptions{.mode = 1, .cut_c = true}));
    PostCallB(pipe, &SendTick, 0);
    pipe.PublishB();
    // B has nothing more and sleeps; C's park request wakes it.
    while (!g_tick_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(handler.parks == 1);
    Drain(pipe);
    pipe.Stop();
    REQUIRE(g_failures == 0);
    REQUIRE(g_order == std::vector<char>({'c'}));
}

namespace {
std::atomic<bool> g_hold_entered{};
std::atomic<bool> g_hold_release{};
void HoldOnB(const u64*, u32) {
    g_hold_entered = true;
    while (!g_hold_release.load()) {
        std::this_thread::yield();
    }
    g_ran_b.push_back(0);
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: Stop ends at a record boundary without what is held", "[video_core]") {
    Watchdog watchdog{"stop"};
    FakeHandler handler;
    TestPipeline pipe{handler};
    ResetGlobals(handler, pipe);
    g_hold_entered = false;
    g_hold_release = false;
    REQUIRE(pipe.Start(StageOptions{.mode = 1}));
    PostCallB(pipe, &HoldOnB, 0);
    for (u32 n = 1; n < 100; ++n) {
        PostCallB(pipe, &RanOnB, n);
    }
    pipe.PublishB();
    while (!g_hold_entered.load()) {
        std::this_thread::yield();
    }
    // Shutdown: A stopped without a drain; B stops after the record it runs.
    std::jthread stopper{[&] { pipe.Stop(); }};
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    g_hold_release = true;
    stopper.join();
    REQUIRE(!pipe.Running());
    REQUIRE(g_ran_b.size() == 1);
}
