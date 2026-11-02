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
#include <utility>
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
    /// Bytes written to each stage's ring (by its one producer).
    std::array<size_t, 3> wrote{};
    /// Called by Mark (the last stage), if set.
    void (*on_mark)(u64 position){};
    /// Called by Verify, if set.
    void (*on_verify)(u8 stage){};
    /// Called by BoundaryWork (C at a record boundary, asked), if set.
    void (*on_boundary)(){};
    /// Hazard reports by kind (StageHazard).
    std::array<std::atomic<u32>, 2> hazards{};
    /// STAGE_CHECK's reports of a join from B that lasts (JoinStall), the last one's wait.
    std::atomic<u32> join_stalls{};
    std::atomic<u64> join_stall_ms{};

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
    void Wrote(u8 stage, size_t bytes) {
        wrote[stage] += bytes;
    }
    void Parked() {
        ++parks;
    }
    void Hazard(u8, StageHazard what, u64, u64) {
        ++hazards[static_cast<size_t>(what)];
    }
    void BoundaryWork() {
        if (on_boundary != nullptr) {
            on_boundary();
        }
    }
    void JoinStall(u64, u64, u64 ms) {
        join_stall_ms = ms;
        ++join_stalls;
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
    // 3 words, then 2, then 1 after them (odd sizes): one record.
    pipe.PostCbData(0x1000, &data[0], 3);
    pipe.PostCbData(0x100C, &data[3], 2);
    pipe.PostCbData(0x1014, &data[5], 1);
    // A delta pending (a register written between the writes, K6): a new record, taking it.
    handler.capture = {0x111};
    pipe.PostCbData(0x1018, &data[6], 1);
    // Elsewhere: a new record.
    pipe.PostCbData(0x2000, &data[7], 1);
    // Published: a new record.
    pipe.PublishB();
    pipe.PostCbData(0x2004, &data[8], 4);
    // Anything else in between: a new record.
    PostCallB(pipe, &RanOnB, 7);
    pipe.PostCbData(0x2014, &data[12], 1);
    pipe.PostCbData(0x2018, &data[13], 2);
    Drain(pipe);
    pipe.Stop();
    REQUIRE(g_failures == 0);
    const auto& cb = handler.logs[StageIdB].cb;
    REQUIRE(cb.size() == 5);
    REQUIRE(cb[0].address == 0x1000);
    REQUIRE(cb[0].data == std::vector<u32>(&data[0], &data[6]));
    REQUIRE(cb[1].address == 0x1018);
    REQUIRE(cb[1].data == std::vector<u32>(&data[6], &data[7]));
    REQUIRE(cb[2].address == 0x2000);
    REQUIRE(cb[2].data == std::vector<u32>(&data[7], &data[8]));
    REQUIRE(cb[3].address == 0x2004);
    REQUIRE(cb[3].data == std::vector<u32>(&data[8], &data[12]));
    REQUIRE(cb[4].address == 0x2014);
    REQUIRE(cb[4].data == std::vector<u32>(&data[12], &data[15]));
    REQUIRE(handler.logs[StageIdB].deltas == std::vector<u64>({0x111}));
    REQUIRE(g_ran_b == std::vector<u64>({7}));
}

TEST_CASE("Nx2StagePipeline: CbData longer than a record goes in several, in order (K9)",
          "[video_core]") {
    Watchdog watchdog{"cb data split"};
    FakeHandler handler;
    // Rings of the real size: a record of MaxBodyWords fits.
    StagePipeline<FakeHandler> pipe{handler};
    g_failures = 0;
    REQUIRE(pipe.Start(StageOptions{.mode = 1, .check = true}));
    constexpr u32 Max = StagePipeline<FakeHandler>::MaxCbDataCount;
    static_assert(2 + Max / 2 == MaxBodyWords);
    std::vector<u32> data(2 * Max + 5);
    for (u32 i = 0; i < data.size(); ++i) {
        data[i] = i * 2654435761U;
    }
    // One write of 2 records and a bit: three records.
    pipe.PostCbData(0x10000, data.data(), static_cast<u32>(data.size()));
    // Just after it: joins the last one (5 words) while the sum fits...
    pipe.PostCbData(0x10000 + data.size() * 4, data.data(), 7);
    // ...but not past MaxBodyWords: a new record.
    pipe.PostCbData(0x10000 + (data.size() + 7) * 4, data.data(), Max - 11);
    pipe.JoinAEnd(pipe.JoinABegin());
    pipe.Stop();
    REQUIRE(g_failures == 0);
    REQUIRE(handler.bad_sequences == 0);
    const auto& cb = handler.logs[StageIdB].cb;
    REQUIRE(cb.size() == 4);
    REQUIRE(cb[0].address == 0x10000);
    REQUIRE(cb[0].data == std::vector<u32>(data.begin(), data.begin() + Max));
    REQUIRE(cb[1].address == 0x10000 + u64{Max} * 4);
    REQUIRE(cb[1].data == std::vector<u32>(data.begin() + Max, data.begin() + 2 * Max));
    REQUIRE(cb[2].address == 0x10000 + u64{Max} * 8);
    std::vector<u32> joined(data.begin() + 2 * Max, data.end());
    joined.insert(joined.end(), data.begin(), data.begin() + 7);
    REQUIRE(cb[2].data == joined);
    REQUIRE(cb[3].data.size() == Max - 11);
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
                // A sends nothing inside a join (K4); B, parked, runs nothing; after it, B goes on.
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
                REQUIRE(g_b_ran_count.load(std::memory_order_relaxed) == round + 1);
                pipe.JoinAEnd(join);
                PostCallB(pipe, &CountOnB, round + 1000);
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
        // Every third record, the joins too (K8): 3000 calls, 300 joins and the drain's.
        REQUIRE(handler.logs[StageIdB].verifies == (Calls + Calls / 10 + 1) / 3);
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
// Joins and parks between B and C (K3 as decided after R2, nx2_stages.h): B parks only at a
// record boundary of ring 2; C asks ParkB at a record boundary of its own and runs ring 3 on
// while it waits; C answers a join only once it ran up to its target.
std::atomic<bool> g_tick_done{};
std::atomic<u32> g_spin_b{};
std::atomic<u32> g_spin_c{};
std::vector<char> g_order;
std::mutex g_order_mutex;
u32 g_extra_records{};

void Order(char what) {
    std::scoped_lock lock{g_order_mutex};
    g_order.push_back(what);
}

/// The op of the ParkB a record of C asked for at its next record boundary (C's own).
void (*g_park_at_boundary)(){};
/// FakeHandler::on_boundary: C at a record boundary runs the ParkB a record asked for.
void ParkAtBoundary() {
    if (void (*const op)() = std::exchange(g_park_at_boundary, nullptr)) {
        g_pipe->ParkB(op);
    }
}
/// On C, inside a record: a ParkB with `op` at the record boundary after it (StageTickWork's
/// way).
void AskPark(void (*op)()) {
    g_park_at_boundary = op;
    g_pipe->RequestBoundaryWork();
}

/// Counted records for C: B numbers them (g_sent, B's own), C keeps the last one's number + 1.
u64 g_sent{};
std::atomic<u64> g_c_count{};
void CountOnC(const u64* payload, u32) {
    g_c_count.store(payload[0] + 1, std::memory_order_relaxed);
}
/// B inside a record of ring 2 (maybe a packet: no park point).
std::atomic<bool> g_b_in_record{};
std::atomic<u32> g_b_progress{};
std::atomic<u32> g_park_ops{};
/// B's g_sent once it ended its record (C reads it in a ParkB op, B parked).
u64 g_expected_c{};

/// A ParkB op: B parked at a record boundary of ring 2, C ran everything B sent before it
/// parked and took B's deltas (R2 F2); B runs nothing meanwhile.
void CheckParked() {
    Expect(!g_b_in_record.load(), "ParkB: B parks only at a record boundary of ring 2");
    Expect(g_c_count.load() == g_expected_c, "ParkB: C ran ring 3 up to where B parked",
           g_c_count.load(), g_expected_c);
    Expect(g_handler->logs[StageIdC].deltas == g_handler->logs[StageIdB].deltas,
           "ParkB: C's replica where B's is", g_handler->logs[StageIdC].deltas.size(),
           g_handler->logs[StageIdB].deltas.size());
    const u32 before = g_b_progress.load();
    Spin(20000);
    Expect(g_b_progress.load() == before, "ParkB: B runs nothing while parked",
           g_b_progress.load(), before);
    ++g_park_ops;
    Order('c');
}

void AskCheckedPark(const u64*, u32) {
    Spin(g_spin_c.load(std::memory_order_relaxed));
    AskPark(&CheckParked);
}

void ResetK3(FakeHandler& handler, TestPipeline& pipe) {
    ResetGlobals(handler, pipe);
    handler.on_boundary = &ParkAtBoundary;
    g_order.clear();
    g_park_at_boundary = nullptr;
    g_sent = 0;
    g_c_count = 0;
    g_expected_c = 0;
    g_b_in_record = false;
    g_b_progress = 0;
    g_park_ops = 0;
    g_tick_done = false;
}

void TickOnC(const u64*, u32) {
    Spin(g_spin_c.load(std::memory_order_relaxed));
    AskPark([] {
        Expect(g_handler->parks.load() == 1, "ParkB: B parked", g_handler->parks.load(), 1);
        Expect(g_ran_c.size() == g_extra_records, "ParkB: C ran ring 3 up to where B parked",
               g_ran_c.size(), g_extra_records);
        Order('c');
    });
    g_tick_done.store(true, std::memory_order_seq_cst);
}

/// On B: C gets a tick (which asks B to park at C's next boundary), a mark, maybe more than ring
/// 3 holds; then, without those, B joins C while C waits for B to park, or before.
void TickAndJoinOnB(const u64* payload, u32) {
    g_b_in_record = true;
    PostCallC(*g_pipe, &TickOnC, payload[0]);
    u64* const body = g_pipe->BeginC(RecordKind::Mark, 1);
    body[0] = payload[0] + 1;
    g_pipe->EndC();
    g_pipe->PublishC();
    for (u32 i = 0; i < g_extra_records; ++i) {
        // Ring 3 fills while C waits in ParkB: C runs it on, B waits for room, parking nowhere.
        PostCallC(*g_pipe, &RanOnC, i, 200);
    }
    if (g_extra_records == 0) {
        Spin(g_spin_b.load(std::memory_order_relaxed));
        g_pipe->JoinFromB([] {
            Expect(g_tick_done.load(), "JoinFromB: C ran the tick B sent before");
            Order('b');
        });
    }
    g_b_in_record = false;
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: JoinFromB and ParkB asked at once (K3)", "[video_core]") {
    Watchdog watchdog{"JoinFromB and ParkB"};
    std::mt19937 random{1234};
    for (const u8 mode : {u8{1}, u8{2}}) {
        for (u32 round = 0; round < 300; ++round) {
            FakeHandler handler;
            TestPipeline pipe{handler};
            ResetK3(handler, pipe);
            // Who asks first varies: B joining before or after C asks B to park.
            g_spin_b = random() % 20000;
            g_spin_c = random() % 20000;
            g_extra_records = round % 3 == 2 ? 40 : 0;
            REQUIRE(pipe.Start(StageOptions{.mode = mode, .cut_c = true}));
            PostCallB(pipe, &TickAndJoinOnB, 100);
            PostMarkB(pipe, 1000, true);
            Drain(pipe);
            pipe.Stop();
            REQUIRE(g_failures == 0);
            REQUIRE(g_tick_done);
            if (g_extra_records == 0) {
                // B's join first (C answers it once reached, in its ParkB wait too), then
                // C's park at B's next record boundary of ring 2.
                REQUIRE(g_order == std::vector<char>({'b', 'c'}));
            } else {
                REQUIRE(g_order == std::vector<char>({'c'}));
                REQUIRE(g_ran_c.size() == g_extra_records);
            }
            REQUIRE(handler.parks == 1);
            REQUIRE(handler.hazards[0] == 0);
            REQUIRE(handler.hazards[1] == 0);
            REQUIRE(handler.logs[StageIdC].last_mark == 1000);
        }
    }
}

namespace {
void TickOnly(const u64*, u32) {
    AskPark([] {
        ++g_park_ops;
        Order('c');
    });
}
void SendTick(const u64*, u32) {
    PostCallC(*g_pipe, &TickOnly, 0);
    g_pipe->PublishC();
}
std::atomic<u32> g_boundary_runs{};
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: ParkB wakes a B waiting for records", "[video_core]") {
    Watchdog watchdog{"ParkB wakes B"};
    FakeHandler handler;
    TestPipeline pipe{handler};
    ResetK3(handler, pipe);
    REQUIRE(pipe.Start(StageOptions{.mode = 1, .cut_c = true}));
    PostCallB(pipe, &SendTick, 0);
    pipe.PublishB();
    // B has nothing more and sleeps; C's park request wakes it.
    while (g_park_ops.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(handler.parks == 1);
    Drain(pipe);
    pipe.Stop();
    REQUIRE(g_failures == 0);
    REQUIRE(g_order == std::vector<char>({'c'}));
}

TEST_CASE("Nx2StagePipeline: C's boundary work runs when asked while C waits", "[video_core]") {
    Watchdog watchdog{"boundary work"};
    FakeHandler handler;
    TestPipeline pipe{handler};
    ResetK3(handler, pipe);
    g_boundary_runs = 0;
    handler.on_boundary = [] { ++g_boundary_runs; };
    REQUIRE(pipe.Start(StageOptions{.mode = 1, .cut_c = true}));
    // C sleeps waiting for ring 3: an ask from another thread wakes it.
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    pipe.RequestBoundaryWork();
    while (g_boundary_runs.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    Drain(pipe);
    pipe.Stop();
    REQUIRE(g_boundary_runs == 1);
    REQUIRE(g_failures == 0);
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


namespace {
/// Stands for the buffer cache's mutex (recursive, B's).
std::recursive_mutex g_buffer_mutex;

/// A ParkB op takes the buffer mutex (K3: C only here, B parked holding none).
void ParkTakesLock() {
    const bool locked = g_buffer_mutex.try_lock();
    Expect(locked, "ParkB: B holds the buffer mutex no more");
    if (locked) {
        g_buffer_mutex.unlock();
    }
    CheckParked();
}
void AskLockedPark(const u64*, u32) {
    Spin(g_spin_c.load(std::memory_order_relaxed));
    AskPark(&ParkTakesLock);
}

/// On B (K3 (a), R2 F6): C asks to park; B sends counted records (with `payload[0]` > 0, more
/// than ring 3 holds), then joins holding the buffer mutex, as the buffer runtime's hook does.
void LockedJoinOnB(const u64* payload, u32) {
    g_b_in_record = true;
    PostCallC(*g_pipe, &AskLockedPark, 0);
    for (u32 i = 0; i < 6; ++i) {
        PostCallC(*g_pipe, &CountOnC, g_sent++);
    }
    g_pipe->PublishC();
    for (u64 i = 0; i < payload[0]; ++i) {
        // Ring 3 full: B waits for room inside its record, serving no park; C runs ring 3 on.
        PostCallC(*g_pipe, &CountOnC, g_sent++, 200);
        ++g_b_progress;
    }
    Spin(g_spin_b.load(std::memory_order_relaxed));
    {
        std::scoped_lock lock{g_buffer_mutex};
        const size_t wrote = g_handler->wrote[StageIdC];
        const u64 expected = g_sent;
        g_pipe->JoinFromBLocked([wrote, expected] {
            Expect(g_c_count.load() == expected, "JoinFromBLocked: C ran everything B sent before",
                   g_c_count.load(), expected);
            Expect(g_handler->wrote[StageIdC] == wrote, "JoinFromBLocked: no ring 3 record",
                   g_handler->wrote[StageIdC], wrote);
            // The op locks it again: B's own, the same thread.
            std::scoped_lock again{g_buffer_mutex};
            Order('b');
        });
        ++g_b_progress;
    }
    g_expected_c = g_sent;
    g_b_in_record = false;
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: a locked join while C asks ParkB, ring 3 full or not (K3 a)",
          "[video_core]") {
    Watchdog watchdog{"locked join"};
    std::mt19937 random{4321};
    for (const u8 mode : {u8{1}, u8{2}}) {
        for (u32 round = 0; round < 200; ++round) {
            FakeHandler handler;
            TestPipeline pipe{handler};
            ResetK3(handler, pipe);
            g_spin_b = random() % 20000;
            g_spin_c = random() % 20000;
            REQUIRE(pipe.Start(StageOptions{.mode = mode, .cut_c = true}));
            handler.capture = {u64{round} << 8 | 3};
            // Every other round more than ring 3 holds (40 records of 203 words, 2048 words).
            PostCallB(pipe, &LockedJoinOnB, round % 2 == 0 ? 0 : 40);
            handler.capture = {u64{round} << 8 | 4};
            PostCallB(pipe, &RanOnB, round);
            Drain(pipe);
            pipe.Stop();
            REQUIRE(g_failures == 0);
            REQUIRE(g_order == std::vector<char>({'b', 'c'}));
            REQUIRE(g_park_ops == 1);
            REQUIRE(handler.hazards[0] == 0);
            REQUIRE(handler.hazards[1] == 0);
            REQUIRE(handler.logs[StageIdC].deltas == handler.logs[StageIdB].deltas);
        }
    }
}

namespace {
/// On B: C asks to park, B sends counted records (more than ring 3 holds with `payload[0]`).
void AskParkThenSendOnB(const u64* payload, u32) {
    g_b_in_record = true;
    PostCallC(*g_pipe, &AskLockedPark, 0);
    for (u64 i = 0; i < payload[0]; ++i) {
        PostCallC(*g_pipe, &CountOnC, g_sent++, 200);
        ++g_b_progress;
    }
    g_expected_c = g_sent;
    g_b_in_record = false;
}
/// On B, the record after (with a delta): it sends nothing, takes the buffer mutex and joins at
/// once, as the buffer runtime's hook inside a packet whose draw has not gone to C yet.
void LockedJoinPendingOnB(const u64*, u32) {
    g_b_in_record = true;
    {
        std::scoped_lock lock{g_buffer_mutex};
        const size_t wrote = g_handler->wrote[StageIdC];
        g_pipe->JoinFromBLocked([wrote] {
            Expect(g_handler->wrote[StageIdC] == wrote, "JoinFromBLocked: no ring 3 record",
                   g_handler->wrote[StageIdC], wrote);
            Expect(g_handler->logs[StageIdC].deltas.size() + 1 ==
                       g_handler->logs[StageIdB].deltas.size(),
                   "JoinFromBLocked: the record's delta stays pending",
                   g_handler->logs[StageIdC].deltas.size(),
                   g_handler->logs[StageIdB].deltas.size());
            Order('b');
        });
        ++g_b_progress;
    }
    g_b_in_record = false;
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: a locked join leaves B's pending deltas pending (sinama-1)",
          "[video_core]") {
    Watchdog watchdog{"locked join, deltas pending"};
    std::mt19937 random{1357};
    for (const u8 mode : {u8{1}, u8{2}}) {
        for (u32 round = 0; round < 120; ++round) {
            FakeHandler handler;
            TestPipeline pipe{handler};
            ResetK3(handler, pipe);
            // Wide: C's ParkB served before the join's record or waiting through it.
            g_spin_c = random() % 60000;
            REQUIRE(pipe.Start(StageOptions{.mode = mode, .cut_c = true}));
            handler.capture = {u64{round} << 8 | 3};
            PostCallB(pipe, &AskParkThenSendOnB, round % 2 == 0 ? 0 : 40);
            handler.capture = {u64{round} << 8 | 4};
            PostCallB(pipe, &LockedJoinPendingOnB, 0);
            pipe.PublishB();
            while (g_park_ops.load() == 0) {
                std::this_thread::yield();
            }
            Drain(pipe);
            pipe.Stop();
            REQUIRE(g_failures == 0);
            REQUIRE(g_order.size() == 2);
            REQUIRE(g_park_ops == 1);
            REQUIRE(handler.hazards[1] == 0);
            // The pending delta reached C afterwards (ServePark, A's join).
            REQUIRE(handler.logs[StageIdC].deltas == handler.logs[StageIdB].deltas);
            REQUIRE(handler.logs[StageIdB].deltas.size() == 2);
        }
    }
}

namespace {
std::atomic<bool> g_b_waits_record{};

/// On C, inside a record: =2 runs the op at once (B waits for this record's end); =1 is a
/// hazard, served at B's next record boundary of ring 2.
void ParkInsideOnC(const u64*, u32) {
    if (g_pipe->Options().mode == 2) {
        g_pipe->ParkB([] {
            Expect(g_b_waits_record.load(), "=2: B waits for the record C asked inside");
            ++g_park_ops;
            Order('c');
        });
    } else {
        g_pipe->ParkB([] {
            Expect(!g_b_in_record.load(), "=1: B parks at a record boundary of ring 2");
            ++g_park_ops;
            Order('c');
        });
    }
}
/// On B: a record for C that asks ParkB inside it; at =2 then a JoinFromB.
void ParkInsideThenJoinOnB(const u64*, u32) {
    g_b_in_record = true;
    g_b_waits_record = true;
    PostCallC(*g_pipe, &ParkInsideOnC, 0);
    g_b_waits_record = false;
    if (g_pipe->Options().mode == 2) {
        Expect(g_park_ops.load() == 1, "=2: the op ran while B waited for the record's end",
               g_park_ops.load(), 1);
        g_pipe->JoinFromB([] { Order('b'); });
    } else {
        g_pipe->PublishC();
    }
    ++g_b_progress;
    g_b_in_record = false;
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: ParkB inside a record, =2 lockstep and =1 hazard", "[video_core]") {
    Watchdog watchdog{"ParkB inside a record"};
    for (const u8 mode : {u8{2}, u8{1}}) {
        for (u32 round = 0; round < 50; ++round) {
            FakeHandler handler;
            TestPipeline pipe{handler};
            ResetK3(handler, pipe);
            REQUIRE(pipe.Start(StageOptions{.mode = mode, .cut_c = true}));
            PostCallB(pipe, &ParkInsideThenJoinOnB, 0);
            pipe.PublishB();
            if (mode == 1) {
                // The hazard's limit: C, not running ring 3 on, would hang on a B blocked on C
                // (a join from A passed on before C's request): B parks idle first.
                while (g_park_ops.load() == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{1});
                }
            }
            Drain(pipe);
            pipe.Stop();
            REQUIRE(g_failures == 0);
            REQUIRE(g_park_ops == 1);
            REQUIRE(handler.parks == 1);
            if (mode == 2) {
                REQUIRE(g_order == std::vector<char>({'c', 'b'}));
                REQUIRE(handler.hazards[0] == 0);
            } else {
                REQUIRE(g_order == std::vector<char>({'c'}));
                REQUIRE(handler.hazards[0] == 1);
            }
            REQUIRE(handler.hazards[1] == 0);
        }
    }
}

namespace {
/// On B: maybe C asks to park, B sends `count` counted records, then joins (locked or not):
/// the op runs after every record B sent before (R2 F1), with or without a ParkB waiting. Bare
/// (locked, nothing sent first): the record's deltas still pending at the join, the buffer
/// runtime's case (T9b review sinama-1).
void SendThenJoinB(const u64* payload, u32) {
    const u32 count = static_cast<u32>(payload[0] & 0xff);
    const bool locked = ((payload[0] >> 8) & 1) != 0;
    const bool ask_park = ((payload[0] >> 9) & 1) != 0;
    const bool bare = ((payload[0] >> 10) & 1) != 0;
    g_b_in_record = true;
    if (ask_park) {
        PostCallC(*g_pipe, &AskCheckedPark, 0);
    }
    for (u32 i = 0; i < count; ++i) {
        PostCallC(*g_pipe, &CountOnC, g_sent++);
    }
    const u64 expected = g_sent;
    const size_t wrote = g_handler->wrote[StageIdC];
    const auto op = [locked, bare, expected, wrote] {
        Expect(g_c_count.load() == expected, "a join from B: C ran everything B sent before",
               g_c_count.load(), expected);
        if (locked) {
            Expect(g_handler->wrote[StageIdC] == wrote, "JoinFromBLocked: no ring 3 record",
                   g_handler->wrote[StageIdC], wrote);
            if (bare) {
                // C parked: its replica behind B's by this record's two deltas at least.
                Expect(g_handler->logs[StageIdC].deltas.size() + 2 <=
                           g_handler->logs[StageIdB].deltas.size(),
                       "JoinFromBLocked: the deltas stay pending",
                       g_handler->logs[StageIdC].deltas.size(),
                       g_handler->logs[StageIdB].deltas.size());
            }
        } else {
            Expect(g_handler->logs[StageIdC].deltas == g_handler->logs[StageIdB].deltas,
                   "JoinFromB: C's replica took B's deltas",
                   g_handler->logs[StageIdC].deltas.size(),
                   g_handler->logs[StageIdB].deltas.size());
        }
        Order('b');
    };
    if (locked) {
        g_pipe->JoinFromBLocked(op);
    } else {
        g_pipe->JoinFromB(op);
    }
    ++g_b_progress;
    g_expected_c = g_sent;
    g_b_in_record = false;
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: a join from B runs after everything B sent before (K3, F1)",
          "[video_core]") {
    Watchdog watchdog{"JoinFromB"};
    std::mt19937 random{777};
    for (const u8 mode : {u8{1}, u8{2}}) {
        FakeHandler handler;
        TestPipeline pipe{handler};
        ResetK3(handler, pipe);
        REQUIRE(pipe.Start(StageOptions{.mode = mode, .cut_c = true, .check = true}));
        u32 parks_asked = 0;
        for (u32 round = 0; round < 600; ++round) {
            g_spin_c = random() % 4000;
            const bool locked = round % 2 == 1;
            const bool bare = locked && round % 6 == 1;
            const bool ask_park = round % 3 == 0;
            parks_asked += ask_park ? 1 : 0;
            handler.capture = {u64{round} << 8 | 1, u64{round} << 8 | 2};
            PostCallB(pipe, &SendThenJoinB,
                      (bare ? 0 : round % 40) | (locked ? 0x100u : 0u) |
                          (ask_park ? 0x200u : 0u) | (bare ? 0x400u : 0u));
            handler.capture = {u64{round} << 8 | 5};
            PostCallB(pipe, &RanOnB, round);
            if (round % 7 == 0) {
                Drain(pipe);
            }
        }
        Drain(pipe);
        // The last park asked runs at B's next record boundary of ring 2: wait for it.
        while (g_park_ops.load() != parks_asked) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        pipe.Stop();
        REQUIRE(g_failures == 0);
        REQUIRE(handler.bad_sequences == 0);
        REQUIRE(g_order.size() == 600 + parks_asked);
        REQUIRE(handler.hazards[0] == 0);
        REQUIRE(handler.hazards[1] == 0);
        REQUIRE(handler.logs[StageIdC].deltas == handler.logs[StageIdB].deltas);
        REQUIRE(handler.logs[StageIdB].deltas.size() == 1800);
    }
}

namespace {
/// On B: C asks to park; B sends `payload[0]` counted records (some long); its next record of
/// ring 2 brings deltas B passes on only when it parks (R2 F2: C runs on up to there first).
void SendAndParkOnB(const u64* payload, u32) {
    g_b_in_record = true;
    PostCallC(*g_pipe, &AskCheckedPark, 0);
    for (u64 i = 0; i < payload[0]; ++i) {
        PostCallC(*g_pipe, &CountOnC, g_sent++, i % 5 == 0 ? 150 : 0);
    }
    ++g_b_progress;
    g_expected_c = g_sent;
    g_b_in_record = false;
}
void ProgressOnB(const u64*, u32) {
    ++g_b_progress;
}
/// A (the main thread) waits for B: in BeginB / EndB or its join. =2 checks C's replica only
/// then, the primaries where the record left them (T9b review protokol-2).
std::atomic<bool> g_a_waiting{};
void VerifyWhileAWaits(u8 stage) {
    if (stage == StageIdC) {
        Expect(g_a_waiting.load(), "=2: C verifies while A waits");
    }
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: a ParkB's op runs once C ran up to where B parked (K3, F2)",
          "[video_core]") {
    Watchdog watchdog{"ParkB run on"};
    std::mt19937 random{99};
    for (const u8 mode : {u8{1}, u8{2}}) {
        FakeHandler handler;
        TestPipeline pipe{handler};
        ResetK3(handler, pipe);
        handler.on_verify = &VerifyWhileAWaits;
        REQUIRE(pipe.Start(StageOptions{.mode = mode, .cut_c = true}));
        for (u32 round = 0; round < 300; ++round) {
            g_spin_c = random() % 30000;
            handler.capture = {u64{round} << 8 | 6};
            g_a_waiting = true;
            PostCallB(pipe, &SendAndParkOnB, random() % 60);
            g_a_waiting = false;
            // Deltas B applies and has not passed on when C's park comes after this record (B
            // idle after it: A's join, which would pass them on, only once the park ran).
            handler.capture = {u64{round} << 8 | 7, u64{round} << 8 | 8};
            g_a_waiting = true;
            PostCallB(pipe, &ProgressOnB, 0);
            g_a_waiting = false;
            pipe.PublishB();
            while (g_park_ops.load() != round + 1) {
                std::this_thread::yield();
            }
            g_a_waiting = true;
            Drain(pipe);
            g_a_waiting = false;
        }
        pipe.Stop();
        REQUIRE(g_failures == 0);
        REQUIRE(handler.parks == 300);
        REQUIRE(handler.hazards[0] == 0);
        REQUIRE(handler.hazards[1] == 0);
    }
}

namespace {
void SlowAskParkOnC(const u64* payload, u32) {
    std::this_thread::sleep_for(std::chrono::milliseconds{payload[0]});
    AskPark([] {
        const u32 before = g_b_progress.load();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        Expect(g_b_progress.load() == before, "ParkB: B runs nothing while parked",
               g_b_progress.load(), before);
        ++g_park_ops;
        Order('c');
    });
}
void SendSlowAsk(const u64* payload, u32) {
    PostCallC(*g_pipe, &SlowAskParkOnC, payload[0]);
    g_pipe->PublishC();
    ++g_b_progress;
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: =2 checks no record B writes as it parks, A going on (protokol-2)",
          "[video_core]") {
    Watchdog watchdog{"park deltas check"};
    FakeHandler handler;
    TestPipeline pipe{handler};
    ResetK3(handler, pipe);
    handler.on_verify = &VerifyWhileAWaits;
    REQUIRE(pipe.Start(StageOptions{.mode = 2, .cut_c = true}));
    for (u32 round = 0; round < 200; ++round) {
        // Deltas B applies and keeps: the record sends nothing to C.
        handler.capture = {u64{round} << 8 | 9, u64{round} << 8 | 10};
        g_a_waiting = true;
        PostCallB(pipe, &ProgressOnB, 0);
        g_a_waiting = false;
        // C's boundary work asked by A (StageTickWork outside a join): B parks idle, passing
        // them in a record of their own while A goes on.
        g_park_at_boundary = &CheckParked;
        pipe.RequestBoundaryWork();
        while (g_park_ops.load() != round + 1) {
            std::this_thread::yield();
        }
    }
    g_a_waiting = true;
    Drain(pipe);
    g_a_waiting = false;
    pipe.Stop();
    REQUIRE(g_failures == 0);
    REQUIRE(handler.parks == 200);
    REQUIRE(handler.logs[StageIdC].deltas == handler.logs[StageIdB].deltas);
    REQUIRE(handler.logs[StageIdB].deltas.size() == 400);
}

namespace {
/// T9b review kilit-4: C held inside a record (as by a lock B holds) while B joins.
std::atomic<bool> g_c_held{};
std::atomic<bool> g_c_release{};
std::chrono::steady_clock::time_point g_c_released_at;
std::atomic<s64> g_op_after_release_ms{-1};
void HoldOnC(const u64*, u32) {
    g_c_held = true;
    while (!g_c_release.load()) {
        std::this_thread::yield();
    }
}
void HoldCThenJoinOnB(const u64*, u32) {
    PostCallC(*g_pipe, &HoldOnC, 0);
    g_pipe->JoinFromB([] {
        Expect(g_c_release.load(), "the join's op runs once C went on");
        g_op_after_release_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - g_c_released_at)
                                    .count();
        Order('b');
    });
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: STAGE_CHECK reports a join from B while it waits (kilit-4)",
          "[video_core]") {
    Watchdog watchdog{"join stall"};
    for (u32 round = 0; round < 3; ++round) {
        FakeHandler handler;
        TestPipeline pipe{handler};
        ResetK3(handler, pipe);
        g_c_held = false;
        g_c_release = false;
        g_op_after_release_ms = -1;
        REQUIRE(pipe.Start(StageOptions{.mode = 1, .cut_c = true, .check = true}));
        PostCallB(pipe, &HoldCThenJoinOnB, 0);
        pipe.PublishB();
        // The report comes while the join waits (C still held), not once it ended.
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (handler.join_stalls.load() == 0 && std::chrono::steady_clock::now() < give_up) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        const u32 stalls = handler.join_stalls.load();
        Expect(stalls >= 1, "a join waiting past 50 ms is reported while it waits", stalls);
        Expect(handler.join_stall_ms.load() >= 50, "the report says how long it waited",
               handler.join_stall_ms.load());
        Expect(g_c_held.load(), "C is held meanwhile");
        g_c_released_at = std::chrono::steady_clock::now();
        g_c_release = true;
        Drain(pipe);
        pipe.Stop();
        REQUIRE(g_failures == 0);
        REQUIRE(g_order == std::vector<char>({'b'}));
        // C's ack wakes B at once (SignalWatched), not at the watch's next timeout.
        REQUIRE(g_op_after_release_ms.load() >= 0);
        REQUIRE(g_op_after_release_ms.load() < 300);
    }
}

TEST_CASE("Nx2StagePipeline: C waiting in ParkB parks for A's join on its way", "[video_core]") {
    Watchdog watchdog{"ParkB and A's join"};
    for (const u8 mode : {u8{1}, u8{2}}) {
        for (const u64 delay : {u64{0}, u64{1}, u64{20}}) {
            FakeHandler handler;
            TestPipeline pipe{handler};
            ResetK3(handler, pipe);
            REQUIRE(pipe.Start(StageOptions{.mode = mode, .cut_c = true}));
            PostCallB(pipe, &SendSlowAsk, delay);
            // B passes the join on to C and waits for C's ack (no park point): C, maybe in its
            // ParkB wait, runs ring 3 on to the Join and parks for A.
            const u32 join = pipe.JoinABegin();
            Order('a');
            pipe.JoinAEnd(join);
            while (g_park_ops.load() == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            Drain(pipe);
            pipe.Stop();
            REQUIRE(g_failures == 0);
            REQUIRE(g_order.size() == 2);
            REQUIRE(handler.parks == 1);
            REQUIRE(handler.hazards[0] == 0);
        }
    }
}

TEST_CASE("Nx2StagePipeline: a join inside a join sends nothing (K4)", "[video_core]") {
    Watchdog watchdog{"nested join"};
    for (const bool cut_c : {false, true}) {
        FakeHandler handler;
        TestPipeline pipe{handler};
        ResetGlobals(handler, pipe);
        g_b_ran_count = 0;
        REQUIRE(pipe.Start(StageOptions{.mode = 1, .cut_c = cut_c, .check = true}));
        PostCallB(pipe, &CountOnB, 0);
        const u32 outer = pipe.JoinABegin();
        REQUIRE(outer != 0);
        const size_t wrote = handler.wrote[StageIdB];
        // Nested: no record, no wait (the stages are parked already), and its end ends nothing.
        const u32 inner = pipe.JoinABegin();
        REQUIRE(inner == 0);
        pipe.JoinAEnd(inner);
        REQUIRE(handler.wrote[StageIdB] == wrote);
        REQUIRE(g_b_ran_count == 1);
        pipe.JoinAEnd(outer);
        PostCallB(pipe, &CountOnB, 1);
        Drain(pipe);
        REQUIRE(g_b_ran_count == 2);
        pipe.Stop();
        REQUIRE(g_failures == 0);
        REQUIRE(handler.bad_sequences == 0);
    }
}

namespace {
std::atomic<bool> g_join_pending_ack{};
std::atomic<u32> g_join_verifies{};
std::vector<u64> g_expected_deltas;
/// =2: the Join's check runs after its deltas, while A still waits for the park ack (K8).
void VerifyAtJoin(u8 stage) {
    if (!g_join_pending_ack.load()) {
        return;
    }
    Expect(g_handler->logs[stage].deltas == g_expected_deltas,
           "K8: a join verifies after its deltas", g_handler->logs[stage].deltas.size(),
           g_expected_deltas.size());
    ++g_join_verifies;
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: =2 verifies a join before its park ack (K8)", "[video_core]") {
    Watchdog watchdog{"K8"};
    for (const bool cut_c : {false, true}) {
        FakeHandler handler;
        TestPipeline pipe{handler};
        ResetGlobals(handler, pipe);
        handler.on_verify = &VerifyAtJoin;
        g_join_verifies = 0;
        g_expected_deltas.clear();
        REQUIRE(pipe.Start(StageOptions{.mode = 2, .cut_c = cut_c}));
        for (u32 n = 0; n < 200; ++n) {
            PostCallB(pipe, &RanOnB, n);
            const u64 delta = u64{n} << 16 | 0x7;
            handler.capture = {delta};
            g_expected_deltas.push_back(delta);
            g_join_pending_ack = true;
            const u32 join = pipe.JoinABegin();
            g_join_pending_ack = false;
            pipe.JoinAEnd(join);
        }
        pipe.Stop();
        REQUIRE(g_failures == 0);
        // B's check of each join, and C's (the deltas passed on in its Join record).
        REQUIRE(g_join_verifies == (cut_c ? 400U : 200U));
    }
}

TEST_CASE("Nx2StagePipeline: B passes deltas on in records that hold them (K9, K11)",
          "[video_core]") {
    Watchdog watchdog{"passed deltas"};
    FakeHandler handler;
    // Rings of the real size: a record of MaxDeltaWords deltas fits ring 3.
    StagePipeline<FakeHandler> pipe{handler};
    g_failures = 0;
    REQUIRE(pipe.Start(StageOptions{.mode = 1, .cut_c = true, .check = true}));
    std::vector<u64> sent;
    for (u32 n = 0; n < 20; ++n) {
        // Records B consumes alone, each with most of a header's deltas: more than one ring 3
        // record holds pile up on B.
        const u32 count = MaxDeltaWords - n * 37;
        for (u32 i = 0; i < count; ++i) {
            handler.capture.push_back(u64{n} << 32 | i);
            sent.push_back(u64{n} << 32 | i);
        }
        pipe.PostCbData(0x1000 + n * 0x100, reinterpret_cast<const u32*>(&n), 1);
    }
    pipe.JoinAEnd(pipe.JoinABegin());
    pipe.Stop();
    REQUIRE(g_failures == 0);
    REQUIRE(handler.bad_sequences == 0);
    REQUIRE(handler.logs[StageIdB].deltas == sent);
    REQUIRE(handler.logs[StageIdC].deltas == sent);
}

namespace {
std::atomic<u64> g_stored_mark{};
void StoreMark(u64 position) {
    g_stored_mark.store(position, std::memory_order_release);
}
/// The front's Drain without a join: waits (up to 5 s) for the last stage to store `position`.
bool WaitStored(u64 position) {
    for (u32 ms = 0; ms < 5000; ++ms) {
        if (g_stored_mark.load(std::memory_order_acquire) == position) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    return false;
}
std::atomic<u32> g_c_calls{};
void CountCallOnC(const u64*, u32) {
    ++g_c_calls;
}
/// On B: a call for C as the last record of B's batch.
void SendCountToC(const u64*, u32) {
    PostCallC(*g_pipe, &CountCallOnC, 0);
}
std::atomic<bool> g_unblock_b{};
void BlockOnB(const u64*, u32) {
    while (!g_unblock_b.load()) {
        std::this_thread::yield();
    }
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: C stores B's marks without a join (R1 dikis-1)", "[video_core]") {
    Watchdog watchdog{"marks without a join"};
    FakeHandler handler;
    TestPipeline pipe{handler};
    ResetGlobals(handler, pipe);
    handler.on_mark = &StoreMark;
    g_stored_mark = 0;
    g_unblock_b = false;
    REQUIRE(pipe.Start(StageOptions{.mode = 1, .cut_c = true}));
    // Batches ending in a caught-up mark, as RunStagedBack's PostBatchEnd: B sleeps after each.
    u64 position = 0;
    for (u32 batch = 0; batch < 200; ++batch) {
        for (u32 n = 0; n < batch % 5; ++n) {
            PostCallB(pipe, &RanOnB, n);
        }
        position += 100;
        PostMarkB(pipe, position, true);
        pipe.PublishB();
        REQUIRE(WaitStored(position));
    }
    // A record for C at the end of B's batch, no mark after it: C runs it while B sleeps.
    g_c_calls = 0;
    PostCallB(pipe, &SendCountToC, 0);
    pipe.PublishB();
    for (u32 ms = 0; ms < 5000 && g_c_calls.load() == 0; ++ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(g_c_calls == 1);
    // A mark inside B's batch, the record after it long: C stores it meanwhile.
    position += 100;
    PostMarkB(pipe, position, false);
    PostCallB(pipe, &BlockOnB, 0);
    pipe.PublishB();
    const bool stored = WaitStored(position);
    g_unblock_b = true;
    REQUIRE(stored);
    Drain(pipe);
    pipe.Stop();
    REQUIRE(g_failures == 0);
}

namespace {
/// T9b review protokol-1: the mark after which a ParkB's op runs (B asked for it before).
std::atomic<u64> g_held_position{};
std::atomic<bool> g_held_op_ran{};
void HeldMarkOp() {
    // The front's Drain waits for the mark: it must not end while the op uses the caches.
    Expect(g_stored_mark.load() != g_held_position.load(),
           "ParkB's op runs before C stores the mark after its ask", g_stored_mark.load(),
           g_held_position.load());
    g_held_op_ran = true;
    ++g_park_ops;
}
void StoreMarkAfterOp(u64 position) {
    if (position == g_held_position.load()) {
        Expect(g_held_op_ran.load(), "C stores the mark once the op ran", position);
    }
    StoreMark(position);
}
/// On B: counted records for C (some long: C behind), then what StageTickWork does on B: C's
/// boundary work asked, for a ParkB. A's caught-up mark comes next (RunStagedBack's batch end).
void SendThenAskBoundaryOnB(const u64* payload, u32) {
    for (u64 i = 0; i < payload[0]; ++i) {
        PostCallC(*g_pipe, &CountOnC, g_sent++, i % 4 == 0 ? 200 : 0);
    }
    g_pipe->PublishC();
    Spin(g_spin_b.load(std::memory_order_relaxed));
    g_park_at_boundary = &HeldMarkOp;
    g_pipe->RequestBoundaryWork();
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: C stores a mark after the ParkB asked before it (protokol-1)",
          "[video_core]") {
    Watchdog watchdog{"held mark"};
    std::mt19937 random{2027};
    for (const u8 mode : {u8{1}, u8{2}}) {
        FakeHandler handler;
        TestPipeline pipe{handler};
        ResetK3(handler, pipe);
        handler.on_mark = &StoreMarkAfterOp;
        g_stored_mark = 0;
        g_held_position = 0;
        REQUIRE(pipe.Start(StageOptions{.mode = mode, .cut_c = true}));
        u64 position = 0;
        for (u32 round = 0; round < 300; ++round) {
            g_spin_b = random() % 20000;
            g_held_op_ran = false;
            position += 100;
            g_held_position = position;
            PostCallB(pipe, &SendThenAskBoundaryOnB, random() % 40);
            PostMarkB(pipe, position, true);
            pipe.PublishB();
            // The front's Drain: it ends once C stored the mark, the op having run.
            REQUIRE(WaitStored(position));
            REQUIRE(g_held_op_ran);
        }
        Drain(pipe);
        pipe.Stop();
        REQUIRE(g_failures == 0);
        REQUIRE(g_park_ops == 300);
        REQUIRE(handler.parks == 300);
    }
}

namespace {
std::atomic<u64> g_verified_target{};
void VerifyTarget(u8 stage) {
    // A Target record is not checked: the target checked is always the one of a record after it.
    Expect(g_handler->logs[stage].target != 0, "=2: a check has a target");
    g_verified_target = g_handler->logs[stage].target;
}
void PostTargetB(TestPipeline& pipe, u64 primary) {
    u64* const body = pipe.BeginB(RecordKind::Target, 1);
    body[0] = primary;
    pipe.EndB();
}
} // Anonymous namespace

TEST_CASE("Nx2StagePipeline: =2 checks no Target record, the record after it (R1 dikis-4)",
          "[video_core]") {
    Watchdog watchdog{"target check"};
    for (const bool cut_c : {false, true}) {
        FakeHandler handler;
        TestPipeline pipe{handler};
        ResetGlobals(handler, pipe);
        handler.on_verify = &VerifyTarget;
        REQUIRE(pipe.Start(StageOptions{.mode = 2, .cut_c = cut_c}));
        for (u64 target = 1; target <= 100; ++target) {
            const u32 before = handler.logs[StageIdB].verifies;
            PostTargetB(pipe, target);
            REQUIRE(handler.logs[StageIdB].verifies == before);
            PostCallB(pipe, &RanOnB, target);
            REQUIRE(handler.logs[StageIdB].verifies == before + 1);
            REQUIRE(g_verified_target == target);
        }
        Drain(pipe);
        pipe.Stop();
        REQUIRE(g_failures == 0);
        // 100 calls and the drain's join; C (cut c) its join alone.
        REQUIRE(handler.logs[StageIdB].verifies == 101);
        REQUIRE(handler.logs[StageIdC].verifies == (cut_c ? 1U : 0U));
    }
}
