// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

#include "common/common_types.h"

// Back stages (EDEN_NX2_BACK_STAGES): the record format of the rings between the stages, the ring
// itself and the stages below A that run them (StagePipeline), header-only so the unit tests take
// them alone. Owner: T1 (the format is the contract, nx2_stages.h).
namespace VideoCommon::Nx2Stages {

/// What a record of ring 2 (A to B) or ring 3 (B to C) holds after its header.
enum class RecordKind : u8 {
    Wrap = 0,    ///< The rest of the ring is unused: continue at its start (no words)
    Call = 1,    ///< Word 0 a Thunk, then its payload (Thunk gets the payload and its size)
    Mark = 2,    ///< Word 0 the ring 1 position A ran up to; the last stage stores it (D1)
    Join = 3,    ///< No words: run everything before it, park until released (seq: handshake)
    ExecA = 4,   ///< =3: word 0 the ring 1 position, word 1 the words to run with GpuSplit::Execute
    Forward = 5, ///< A ring 1 Call entry as it is: word 0 its Ring1Thunk, then its payload (S18)
    Target = 6,  ///< Word 0 the primary Maxwell3D the deltas from here on are for (ReplicaOf);
                 ///< B passes it on to ring 3 (K11)
    CbData = 7,  ///< Word 0 the GPU address, word 1 the bytes, then the data (StageCbWrite, S5/3)
};

/// Ring 2: 2^19 words (4 MiB); ring 3 (K2 only): 2^18 words (2 MiB).
inline constexpr size_t RingToBWords = size_t{1} << 19;
inline constexpr size_t RingToCWords = size_t{1} << 18;

/// Record size (K9). A record larger than its ring would wait for room forever, so:
/// - MaxRecordWords: most words after a header, deltas included; a quarter of the smaller ring.
/// - MaxDeltaWords: most delta words; A's capture holds one per Maxwell3D register at most
///   (Nx2RegReplica::NumRegs, asserted there), so a flush always fits.
/// - MaxBodyWords: most words of the record's own after its deltas (a Call's thunk and payload, a
///   CbData's address, bytes and data). Producers assert it. Larger work is the caller's to
///   split (PostCbData) or to run in a join (the proxy: AccelerateInlineToMemory's data, a draw
///   whose side state is too long; JoinReason::Upload, Other).
inline constexpr u32 MaxRecordWords = static_cast<u32>(RingToCWords / 4);
inline constexpr u32 MaxDeltaWords = 0xE00;
inline constexpr u32 MaxBodyWords = MaxRecordWords - MaxDeltaWords;

namespace Detail {
/// A header whose fields do not fit (K9): the consumer would lose its place in the ring, so stop
/// here. Not constexpr: a constant evaluation reaching it does not compile.
[[noreturn]] inline void BadHeader(u32 words, u32 delta_words, u32 flags) {
    std::fprintf(stderr, "Nx2Stages: record header does not fit: words %u, deltas %u, flags %u\n",
                 words, delta_words, flags);
    std::abort();
}
/// A record from A inside its join (K4): the stages are parked, it would run after the join.
[[noreturn]] inline void RecordInJoin() {
    std::fputs("Nx2Stages: a record from A inside a join\n", stderr);
    std::abort();
}
} // namespace Detail

/// Record header, one u64:
///   bits 0-3   kind (RecordKind)
///   bits 4-7   flags (per kind; none defined yet)
///   bits 8-31  words after the header (delta words included)
///   bits 32-47 delta words: the first words after the header are register deltas
///              (Nx2RegReplica delta words) for the current Target, applied before the record
///              (S5/1: no record of their own; "delta before its consumer", D3)
///   bits 48-63 sequence number (Join and =2 lockstep handshakes, depth 1: compared for
///              equality; wraps; any order test takes SeqAtLeast, K5)
/// A record never wraps around the ring's end: the producer writes Wrap and starts over.
/// MakeHeader stops the process (Detail::BadHeader) rather than cut a field that does not fit
/// (K9): words over MaxRecordWords, deltas over MaxDeltaWords or over words, flags over 0xf.
constexpr u64 MakeHeader(RecordKind kind, u32 words, u32 delta_words, u16 seq, u32 flags = 0) {
    if (words > MaxRecordWords || delta_words > MaxDeltaWords || delta_words > words ||
        flags > 0xf) [[unlikely]] {
        Detail::BadHeader(words, delta_words, flags);
    }
    return static_cast<u64>(kind) | (u64{flags} << 4) | (u64{words} << 8) |
           (u64{delta_words} << 32) | (u64{seq} << 48);
}
constexpr RecordKind HeaderKind(u64 header) {
    return static_cast<RecordKind>(header & 0xf);
}
constexpr u32 HeaderFlags(u64 header) {
    return static_cast<u32>((header >> 4) & 0xf);
}
constexpr u32 HeaderWords(u64 header) {
    return static_cast<u32>((header >> 8) & 0xffffff);
}
constexpr u32 HeaderDeltaWords(u64 header) {
    return static_cast<u32>((header >> 32) & 0xffff);
}
constexpr u16 HeaderSeq(u64 header) {
    return static_cast<u16>(header >> 48);
}

/// Sequence numbers (K5). What counts draws or records across the stages (DrawToC::seq, T3's
/// Replay / CDoneSeq / NoteSeqTick / TickOfSeq, Nx2GpuPages) is u64 and never wraps; a u32 would
/// in ~3.8 h at 312.7k draws/s, and "done >= seq" would then never free a slice again. A narrower
/// copy (the header's 16 bits, a u32 page slot) is compared with SeqAtLeast: whether `a` is `b`
/// or later, wrap-safe while the two are less than half its range apart; a copy left behind by
/// more than that reads as later.
template <typename T>
    requires std::is_unsigned_v<T>
constexpr bool SeqAtLeast(T a, T b) {
    return static_cast<std::make_signed_t<T>>(static_cast<T>(a - b)) >= 0;
}

/// Mark flag: A had run everything ring 1 held when it sent the mark (the end of its batch). The
/// last stage storing it tells the busy clock (Nx2BusyClock::BackStored), as BackLoop did.
inline constexpr u32 MarkCaughtUp = 1;

/// The stages' values (Nx2Stages::Stage, nx2_stages.h, which includes this header).
inline constexpr u8 StageIdA = 0;
inline constexpr u8 StageIdB = 1;
inline constexpr u8 StageIdC = 2;

/// What StagePipeline reports through Handler::Hazard (K3; STAGE_CHECK reports them).
enum class StageHazard : u8 {
    ParkInRecord, ///< =1: C asked ParkB inside a record; it cannot run on to where B parks
    JoinShort,    ///< C acked a join from B short of its target (a = C's read, b = the target)
};

/// A Call record's function and a Forward record's (Nx2Stages::Thunk and Ring1Thunk).
using RecordThunk = void (*)(const u64* payload, u32 words);
using RecordRing1Thunk = void (*)(const u64* payload);

inline void RingPause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

/// What the rings and the stages do around their waits: scopes (default constructible) the
/// waits run in. Nothing by default; nx2_stages.cpp times them as profile zones.
struct RingWaitScopes {
    struct InWait {};   ///< A consumer waits for records
    struct OutWait {};  ///< A producer waits for room (or, =2, for its record to run)
    struct JoinWait {}; ///< A joiner waits for the stages below to park
    struct ParkWait {}; ///< A parked stage waits to go on; C waits for B to park (ParkB)
};

/// Single-producer single-consumer ring of u64 records (T1). The GpuSplit ring's protocol: the
/// producer reserves room for a whole record (Wrap at the end, waiting for space as
/// GpuSplit::WaitForSpace), commits it, and publishes every PublishEvery words, at the end of a
/// batch, and after a drain from 8 words doubling (GpuSplit::PublishRing); the consumer spins
/// briefly, then sleeps on an atomic wait (GpuSplit::BackLoop), and hands space back as it goes.
/// A producer waiting for room spins, then sleeps until the consumer hands some back.
/// Either wait may be a park point (nx2_stages.h, K3: B's on ring 3 and ring 2, C's on ring 3):
/// it then also answers a park request, and the one asking wakes it where it sleeps.
template <size_t Words, typename Scopes = RingWaitScopes>
class StageRing {
public:
    static_assert((Words & (Words - 1)) == 0, "a power of two");
    static constexpr size_t Mask = Words - 1;
    static constexpr size_t PublishEvery = 256;

    /// A wait's interruption: called while a producer waits; returns whether it must not sleep.
    struct NoInterrupt {
        bool operator()() const {
            return false;
        }
    };

    StageRing() : ring{std::make_unique<u64[]>(Words)} {}

    // Producer side.

    /// Room for a record of `words` words (its header included) at the write position, waiting
    /// for the consumer if needed (calling `interrupt` meanwhile). A record never crosses the
    /// ring's end: a Wrap header fills the rest first, and the write position moves past it.
    template <typename Interrupt = NoInterrupt>
    [[nodiscard]] u64* Reserve(size_t words, Interrupt&& interrupt = {}) {
        const size_t index = write_position & Mask;
        if (index + words <= Words && write_position + words - consumed_cache <= Words) [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words, interrupt);
    }

    /// The `words` reserved are written: the consumer sees them from the next publish on, at
    /// the latest PublishEvery words later.
    void Commit(size_t words) {
        write_position += words;
        if (write_position - published_local >= publish_every) {
            Publish();
        }
    }

    /// Makes everything committed visible to the consumer, waking it if it sleeps.
    void Publish() {
        if (published_local == write_position) {
            return;
        }
        published_local = write_position;
        publish_every = (std::min)(publish_every * 2, PublishEvery);
        published.store(write_position, std::memory_order_seq_cst);
        if (consumer_waiting.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_one();
        }
    }

    /// The consumer ran everything sent (a join): the next publishes come from 8 words on,
    /// doubling up to PublishEvery (GpuSplit after a drain).
    void PublishSoon() {
        publish_every = 8;
    }

    [[nodiscard]] u64 WritePosition() const {
        return write_position;
    }
    /// Whether the consumer may already see the record starting at `position`.
    [[nodiscard]] bool Published(u64 position) const {
        return position < published_local;
    }
    /// The producer's own record at `position` (to change it before it is published).
    [[nodiscard]] u64* At(u64 position) {
        return &ring[position & Mask];
    }
    /// Whether the last record, starting at `start` and ending at the write position, may grow
    /// by `words` in place: without reaching the ring's end, with room and without a wait.
    [[nodiscard]] bool FitsAfter(u64 start, size_t words) {
        if ((start & Mask) + (write_position - start) + words > Words) {
            return false;
        }
        if (write_position + words - consumed_cache <= Words) {
            return true;
        }
        consumed_cache = consumed.load(std::memory_order_acquire);
        return write_position + words - consumed_cache <= Words;
    }

    /// Waits until the consumer handed back everything before `position` (the =2 lockstep: until
    /// it ran the last record). Publishes first.
    template <typename Interrupt = NoInterrupt>
    void WaitConsumed(u64 position, Interrupt&& interrupt = {}) {
        Publish();
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache >= position) {
            return;
        }
        [[maybe_unused]] typename Scopes::OutWait scope;
        for (u32 spins = 0;; ++spins) {
            consumed_cache = consumed.load(std::memory_order_acquire);
            if (consumed_cache >= position) {
                return;
            }
            if (interrupt() || spins < 4096) {
                RingPause();
                continue;
            }
            // Release bumps `space_wake` when it sees producer_waiting: read it before the last
            // look at `consumed`, so a release in between ends the wait at once.
            producer_waiting.store(true, std::memory_order_seq_cst);
            const u32 seen = space_wake.load(std::memory_order_seq_cst);
            consumed_cache = consumed.load(std::memory_order_seq_cst);
            if (consumed_cache < position && !interrupt()) {
                space_wake.wait(seen, std::memory_order_seq_cst);
            }
            producer_waiting.store(false, std::memory_order_relaxed);
        }
    }

    /// Ends a producer's wait for room early (to look at `interrupt`): any thread.
    void WakeProducer() {
        space_wake.fetch_add(1, std::memory_order_seq_cst);
        space_wake.notify_one();
    }

    // Consumer side.

    /// The position published so far.
    [[nodiscard]] u64 Available() const {
        return published.load(std::memory_order_acquire);
    }
    [[nodiscard]] const u64* Read(u64 position) const {
        return &ring[position & Mask];
    }
    /// Where the records go on after a Wrap record at `position`: the ring's start.
    [[nodiscard]] static constexpr u64 AfterWrap(u64 position) {
        return position + (Words - (position & Mask));
    }
    /// Hands the room before `position` back to the producer, waking it if it waits for room.
    void Release(u64 position) {
        consumed.store(position, std::memory_order_seq_cst);
        if (producer_waiting.load(std::memory_order_seq_cst)) {
            space_wake.fetch_add(1, std::memory_order_seq_cst);
            space_wake.notify_one();
        }
    }

    /// Waits for records after `read_position`: spins briefly, then sleeps until a publish or a
    /// Wake. Returns the position published (still `read_position` when `stop()` ended it).
    template <typename Stop>
    u64 WaitForWork(u64 read_position, Stop&& stop) {
        u64 available = published.load(std::memory_order_acquire);
        if (available != read_position || stop()) {
            return available;
        }
        [[maybe_unused]] typename Scopes::InWait scope;
        // The producer usually sends more within microseconds: spin briefly before sleeping.
        for (u32 spins = 0; spins < 8192 && available == read_position; ++spins) {
            RingPause();
            available = published.load(std::memory_order_acquire);
            if ((spins & 63) == 63 && stop()) {
                return available;
            }
        }
        if (available != read_position) {
            return available;
        }
        // Publish bumps `wake` when it sees consumer_waiting: read it before the last look at
        // `published`, so a publish in between ends the wait at once (GpuSplit::BackLoop).
        consumer_waiting.store(true, std::memory_order_seq_cst);
        const u32 seen = wake.load(std::memory_order_seq_cst);
        available = published.load(std::memory_order_seq_cst);
        if (available == read_position && !stop()) {
            wake.wait(seen, std::memory_order_seq_cst);
        }
        consumer_waiting.store(false, std::memory_order_relaxed);
        return published.load(std::memory_order_acquire);
    }

    /// Ends a consumer's wait for work early (to look at its `stop`): any thread.
    void Wake() {
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

private:
    template <typename Interrupt>
    u64* ReserveSlow(size_t words, Interrupt& interrupt) {
        size_t index = write_position & Mask;
        if (index + words > Words) {
            // Records never wrap: skip to the start of the ring.
            const size_t rest = Words - index;
            WaitForSpace(rest, interrupt);
            ring[index] = MakeHeader(RecordKind::Wrap, 0, 0, 0);
            write_position += rest;
            index = 0;
        }
        WaitForSpace(words, interrupt);
        return &ring[index];
    }

    template <typename Interrupt>
    void WaitForSpace(size_t words, Interrupt& interrupt) {
        if (write_position + words - consumed_cache <= Words) {
            return;
        }
        WaitConsumed(write_position + words - Words, interrupt);
    }

    /// Read for each record by both sides, written only at construction: a line of its own
    /// (R1 sifir-2), apart from the producer's position written for each record.
    std::unique_ptr<u64[]> ring;

    // Producer side.
    alignas(64) u64 write_position{};
    u64 published_local{};
    u64 consumed_cache{};
    /// Words between publishes: small right after a join, while the consumer has nothing to do.
    size_t publish_every{PublishEvery};

    alignas(64) std::atomic<u64> published{};
    alignas(64) std::atomic<u64> consumed{};
    alignas(64) std::atomic<bool> consumer_waiting{};
    std::atomic<u32> wake{};
    alignas(64) std::atomic<bool> producer_waiting{};
    std::atomic<u32> space_wake{};
};

/// Waits until `word` holds `value` (spins briefly, then sleeps on it): the join and park
/// handshakes, whose other side stores with Signal.
template <typename Scope>
void AwaitValue(const std::atomic<u32>& word, u32 value) {
    if (word.load(std::memory_order_acquire) == value) {
        return;
    }
    [[maybe_unused]] Scope scope;
    for (u32 spins = 0;; ++spins) {
        const u32 seen = word.load(std::memory_order_acquire);
        if (seen == value) {
            return;
        }
        if (spins < 8192) {
            RingPause();
            continue;
        }
        word.wait(seen, std::memory_order_acquire);
    }
}

inline void Signal(std::atomic<u32>& word, u32 value) {
    word.store(value, std::memory_order_release);
    word.notify_one();
}

/// Wakes the one sleeping in AwaitServing on `wake` to look again (a request for it, or the value
/// it waits for stored before this).
inline void Nudge(std::atomic<u32>& wake) {
    wake.fetch_add(1, std::memory_order_seq_cst);
    wake.notify_all();
}

/// A park point's wait (K3): until `done()`, calling `serve` meanwhile (true: it served a
/// request, the wait starts over). Spins briefly, then sleeps on `wake`, which the side making
/// `done` true and anyone asking bump (Nudge) after their store.
template <typename Scope, typename Done, typename Serve>
void AwaitServing(Done&& done, std::atomic<u32>& wake, Serve&& serve) {
    if (done()) {
        return;
    }
    [[maybe_unused]] Scope scope;
    for (u32 spins = 0;; ++spins) {
        if (done()) {
            return;
        }
        if (serve()) {
            spins = 0;
            continue;
        }
        if (spins < 8192) {
            RingPause();
            continue;
        }
        // Read `wake` before the last looks: a store and Nudge in between end the wait at once.
        const u32 seen = wake.load(std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (done() || serve()) {
            spins = 0;
            continue;
        }
        wake.wait(seen, std::memory_order_seq_cst);
    }
}

/// How the stages run: EDEN_NX2_BACK_STAGES (mode: 0 off, 1 on, 2 verify, 3 pass-through),
/// EDEN_NX2_BACK_CUTS (cut c: B | C; never with 3), EDEN_NX2_STAGE_CHECK=1,
/// EDEN_NX2_BACK_STAGES_EVERY=n (=2 compares every n-th record). Nx2Stages::Get's source.
struct StageOptions {
    u8 mode{};
    bool cut_c{};
    bool check{};
    u32 verify_every{1};
};

/// The switches' values (null: unset). Anything but 1, 2 or 3 leaves the stages off.
inline StageOptions ParseStageOptions(const char* stages, const char* cuts, const char* check,
                                      const char* every) {
    StageOptions options;
    if (stages == nullptr || stages[0] < '1' || stages[0] > '3') {
        return options;
    }
    options.mode = static_cast<u8>(stages[0] - '0');
    options.cut_c = options.mode != 3 && cuts != nullptr && std::strchr(cuts, 'c') != nullptr;
    options.check = check != nullptr && check[0] != '\0' && check[0] != '0';
    if (every != nullptr) {
        const unsigned long value = std::strtoul(every, nullptr, 10);
        options.verify_every = value != 0 && value <= 0xffffffffUL ? static_cast<u32>(value) : 1;
    }
    return options;
}

/// The stages below A (T1): B ("GPUMem"; BC with cut a alone) and, with cut c, C ("GPURec"),
/// their threads, the rings to them (2: A to B, 3: B to C), and the joins and parks between
/// them. Header-only so the unit tests run it with a handler of their own; nx2_stages.cpp runs it
/// with the GPU's. What a stage does with a record (Nx2Stages::RecordKind):
/// - first the register deltas at its start, for the stage's current target (Handler::Apply);
///   B with cut c passes them on to C, in the next record it sends there (K11);
/// - Call / Forward: the thunk, with the payload; Target: Handler::Target (B passes it on);
///   CbData: Handler::CbData; ExecA: Handler::ExecA (=3);
/// - Mark: the last stage stores it (Handler::Mark), B with cut c passes it on (D1);
/// - Join: B with cut c makes C park first; then the stage acks the producer and waits for its
///   release (park).
/// =2 (lockstep, depth 1): a producer waits after each record until it ran, Handler::Verify
/// before its end; for a Join the joiner waits for the park ack instead, Verify before the ack
/// (S8, K8). A stage stops at a record boundary once asked (Stop): with nothing left after a
/// drain, and at shutdown without running what it holds.
/// Between B and C (K2; S13, K3 as decided after R2, nx2_stages.h): JoinFromB, JoinFromBLocked
/// and ParkB ask with flags outside the rings; the one asked answers at a park point:
/// - B at each record boundary of ring 2 and its wait for ring 2, nothing else: there it passes
///   its pending deltas, publishes ring 3 and notes where it parked. Its waits on ring 3 (room,
///   the =2 end of a record, C's ack of a Join it passes on) and for a join serve no park.
/// - C at each record boundary of ring 3, its wait for ring 3 and its wait in ParkB, once it ran
///   everything B sent before asking (its read at the join's target), never short of it.
/// - C asks ParkB at a record boundary (its boundary work, RequestBoundaryWork) and runs ring 3
///   on while it waits, so a B blocked on C never waits on the request; the op runs once B
///   parked and C ran ring 3 up to there. =2 lockstep, inside a record: B waits for that
///   record's end, the op runs at once. =1 inside a record: Handler::Hazard, and it waits for
///   B's park without running ring 3 on.
///
/// Handler (nx2_stages.cpp's GpuHandler; the unit tests' fake):
///   Scopes: InWait, OutWait, JoinWait, ParkWait (see RingWaitScopes)
///   u32 PendingDeltas(); void TakeDeltas(u64* out, u32 count);   A's register capture (ring 2)
///   void Apply(u8 stage, const u64* deltas, u32 count);
///   void Target(u8 stage, u64 primary);
///   void CbData(u8 stage, u64 address, const u32* data, u32 count);
///   void ExecA(u64 ring1_position, u64 words);
///   void Mark(u8 stage, u64 ring1_position, bool caught_up);
///   void Verify(u8 stage);                                     =2, every verify_every records
///   void BadSequence(u8 stage, u16 expected, u16 seen);        =2 or STAGE_CHECK
///   void ThreadStart(u8 stage); void Report(u8 stage);         on the stage's thread
///   void Busy(u8 stage, bool busy);                            around a stage's batches
///   void Wrote(u8 to_stage, size_t bytes); void Parked();      counters (Parked: B for ParkB)
///   void Hazard(u8 stage, StageHazard what, u64 a, u64 b);     K3 (on the stage's thread)
///   void BoundaryWork();                                       C at a record boundary, asked
///                                                              (RequestBoundaryWork); may ParkB
template <typename Handler, size_t BWords = RingToBWords, size_t CWords = RingToCWords>
class StagePipeline {
    using Scopes = typename Handler::Scopes;

public:
    template <size_t Words>
    struct Link {
        StageRing<Words, Scopes> ring;
        /// Join handshake (one join at a time): the consumer acks the producer's join count once
        /// parked at its Join record; the producer releases it with the same count.
        alignas(64) std::atomic<u32> ack{};
        alignas(64) std::atomic<u32> release{};

        // Producer side, written for each record: a line of its own, as the consumer's (R1
        // sifir-3).
        alignas(64) u16 seq{};
        u32 joins{};
        RecordKind last_kind{RecordKind::Wrap};
        u64 last_start{};
        u32 pending_words{};
        /// The GPU address the last record's data ends at, when it is CbData.
        u64 cb_end{};

        // Consumer side.
        alignas(64) u16 seen_seq{};
        u32 joins_seen{};
    };
    using LinkB = Link<BWords>;
    using LinkC = Link<CWords>;

    /// Most u32 of constant buffer data one CbData record holds (its address and bytes words
    /// besides, MaxBodyWords; K9).
    static constexpr u32 MaxCbDataCount = (MaxBodyWords - 2) * 2;

    explicit StagePipeline(Handler& handler_) : handler{handler_} {}

    StagePipeline(const StagePipeline&) = delete;
    StagePipeline& operator=(const StagePipeline&) = delete;

    /// Makes the rings and starts B (and C with cut c). Nothing with mode 0: no thread, no ring.
    /// Returns whether it started.
    bool Start(const StageOptions& options_) {
        if (options_.mode == 0 || running) {
            return false;
        }
        options = options_;
        options.cut_c = options.cut_c && options.mode != 3;
        options.verify_every = (std::max)(options.verify_every, u32{1});
        to_b = std::make_unique<LinkB>();
        to_c = options.cut_c ? std::make_unique<LinkC>() : nullptr;
        b = {};
        c = {};
        passed.clear();
        a_in_join = false;
        park_request.store(0, std::memory_order_relaxed);
        park_ack.store(0, std::memory_order_relaxed);
        park_release.store(0, std::memory_order_relaxed);
        park_served = 0;
        park_count = 0;
        join_request.store(0, std::memory_order_relaxed);
        join_ack.store(0, std::memory_order_relaxed);
        join_release.store(0, std::memory_order_relaxed);
        join_target.store(0, std::memory_order_relaxed);
        join_read.store(0, std::memory_order_relaxed);
        park_position.store(0, std::memory_order_relaxed);
        boundary_work.store(false, std::memory_order_relaxed);
        c_in_record = false;
        join_served = 0;
        join_count = 0;
        stop_b.store(false, std::memory_order_relaxed);
        stop_c.store(false, std::memory_order_relaxed);
        b_running.store(true, std::memory_order_seq_cst);
        running = true;
        b_thread = std::jthread([this] { RunB(); });
        if (to_c) {
            c_thread = std::jthread([this] { RunC(); });
        }
        return true;
    }

    /// B stops (at its next record boundary or wait), then C; the rings are freed. The
    /// producers (A for ring 2) stopped before.
    void Stop() {
        if (!running) {
            return;
        }
        stop_b.store(true, std::memory_order_seq_cst);
        to_b->ring.Wake();
        b_thread.join();
        if (to_c) {
            stop_c.store(true, std::memory_order_seq_cst);
            to_c->ring.Wake();
            c_thread.join();
        }
        to_b.reset();
        to_c.reset();
        running = false;
    }

    [[nodiscard]] bool Running() const {
        return running;
    }
    [[nodiscard]] const StageOptions& Options() const {
        return options;
    }

    // Ring 2's producer (A): a record of `kind` with `body_words` words after the deltas A's
    // capture holds (Handler::PendingDeltas, taken into it); returns where its body goes.
    // EndB commits it (=2: and waits for B to run it, but for a Join). PublishB publishes.
    // Never inside a join (K4: the stages are parked, a record would run after it).

    u64* BeginB(RecordKind kind, u32 body_words, u32 flags = 0) {
        if (a_in_join) [[unlikely]] {
            Detail::RecordInJoin();
        }
        CaptureDeltas deltas{handler};
        return Begin(*to_b, deltas, kind, body_words, flags, NoInterrupt{});
    }
    void EndB() {
        End(*to_b, StageIdB, NoInterrupt{});
    }
    void PublishB() {
        to_b->ring.Publish();
    }

    // Ring 3's producer (B, cut c): the same, with the deltas B passes on to C. Its waits there
    // (room, =2) are no park points (K3): B may be inside a packet; C runs ring 3 on meanwhile,
    // in its ParkB wait too.

    u64* BeginC(RecordKind kind, u32 body_words, u32 flags = 0) {
        PassedDeltas deltas{passed};
        return Begin(*to_c, deltas, kind, body_words, flags, NoInterruptC{});
    }
    void EndC() {
        End(*to_c, StageIdC, NoInterruptC{});
    }
    void PublishC() {
        to_c->ring.Publish();
    }

    /// A: constant buffer data for ring 2 (CbData: [address][bytes][data]). Joins the CbData
    /// record just before when it is the last one written, not published yet, ends at `address`,
    /// A's capture holds no delta (K6: a register written between the two writes applies before
    /// the second) and the sum fits one record (MaxBodyWords); a joined record is one write for
    /// StageCbWrite (one IsLightSameWrite, SAME_SKIP decision). Else new records, as many as
    /// `count` needs (MaxCbDataCount each, in order; K9).
    void PostCbData(u64 address, const u32* data, u32 count) {
        LinkB& link = *to_b;
        if (link.last_kind == RecordKind::CbData && link.cb_end == address &&
            !link.ring.Published(link.last_start) && handler.PendingDeltas() == 0) {
            u64* const record = link.ring.At(link.last_start);
            const u64 header = record[0];
            const u32 delta_words = HeaderDeltaWords(header);
            u64* const body = record + 1 + delta_words;
            const u32 old_bytes = static_cast<u32>(body[1]);
            const u64 bytes = u64{count} * sizeof(u32);
            const u64 data_words = (old_bytes + bytes + 7) / 8;
            const u64 grow = data_words - (old_bytes + 7) / 8;
            if (2 + data_words <= MaxBodyWords && link.ring.FitsAfter(link.last_start, grow)) {
                const u32 words = HeaderWords(header) + static_cast<u32>(grow);
                if (grow != 0) {
                    body[1 + data_words] = 0;
                }
                std::memcpy(reinterpret_cast<u8*>(body + 2) + old_bytes, data, bytes);
                body[1] = old_bytes + bytes;
                record[0] = MakeHeader(RecordKind::CbData, words, delta_words, HeaderSeq(header),
                                       HeaderFlags(header));
                link.cb_end = address + bytes;
                link.ring.Commit(grow);
                handler.Wrote(StageIdB, grow * sizeof(u64));
                return;
            }
        }
        do {
            const u32 part = (std::min)(count, MaxCbDataCount);
            const u32 bytes = part * static_cast<u32>(sizeof(u32));
            const u32 data_words = (bytes + 7) / 8;
            u64* const body = BeginB(RecordKind::CbData, 2 + data_words);
            body[0] = address;
            body[1] = bytes;
            if (data_words != 0) {
                body[1 + data_words] = 0;
            }
            std::memcpy(body + 2, data, bytes);
            link.cb_end = address + bytes;
            EndB();
            address += bytes;
            data += part;
            count -= part;
        } while (count != 0);
    }

    /// A: B (and C) run everything sent before and park; returns the join to end with JoinAEnd
    /// once A's work is done. Inside a join (nested, K4) it sends nothing and returns 0: the
    /// stages are parked already.
    u32 JoinABegin() {
        if (a_in_join) {
            return 0;
        }
        LinkB& link = *to_b;
        const u32 id = ++link.joins;
        static_cast<void>(BeginB(RecordKind::Join, 0));
        EndB();
        link.ring.Publish();
        AwaitValue<typename Scopes::JoinWait>(link.ack, id);
        a_in_join = true;
        return id;
    }
    void JoinAEnd(u32 id) {
        if (id == 0) {
            return;
        }
        a_in_join = false;
        Signal(to_b->release, id);
        to_b->ring.PublishSoon();
    }

    /// B (cut c), outside a packet, holding no cache lock: C runs everything B sent before and
    /// parks; `op` runs on B; C goes on. The deltas B has not passed on yet go to C first (a
    /// record of their own; its waits there serve no park), so C's replica is where B's is. A
    /// flag, not a record (K3): C answers at a park point once it ran up to the join's target
    /// (in its ParkB wait too, running ring 3 on to there). This wait is no park point. Without
    /// cut c `op` runs at once.
    template <typename Op>
    void JoinFromB(Op&& op) {
        if (!to_c) {
            op();
            return;
        }
        if (!passed.empty()) {
            FlushPassed();
        }
        const u32 id = AskJoin();
        op();
        EndJoin(id);
    }

    /// B (cut c) inside a packet, holding the buffer cache mutex (K3 (a), R2 F6): as JoinFromB,
    /// but it writes nothing to ring 3 (no wait for room there while it holds the mutex): the
    /// deltas it has not passed stay pending, for its next ring 3 record. C's replica lags B's
    /// by them meanwhile. Without cut c `op` runs at once.
    template <typename Op>
    void JoinFromBLocked(Op&& op) {
        if (!to_c) {
            op();
            return;
        }
        const u32 id = AskJoin();
        op();
        EndJoin(id);
    }

    /// C (cut c), at a record boundary of ring 3: B parks at its next park point (a record
    /// boundary of ring 2 or its wait for ring 2); C runs ring 3 on up to where B parked,
    /// answering B's joins once reached; then `op` runs on C and B goes on (R2 F1, F2). Inside a
    /// record: at =2 B waits for that record's end (lockstep), so `op` runs at once; at =1
    /// Handler::Hazard(ParkInRecord), then it waits for B's park without running ring 3 on.
    /// Without cut c, or once B stopped, `op` runs at once.
    template <typename Op>
    void ParkB(Op&& op) {
        if (!to_c || !b_running.load(std::memory_order_seq_cst)) {
            op();
            return;
        }
        const bool in_record = c_in_record;
        if (in_record) {
            if (options.mode == 2) {
                handler.Parked();
                op();
                return;
            }
            handler.Hazard(StageIdC, StageHazard::ParkInRecord, c.read, 0);
        }
        const u32 id = ++park_count;
        park_request.store(id, std::memory_order_seq_cst);
        // B may sleep waiting for ring 2's records.
        to_b->ring.Wake();
        AwaitParked(id, !in_record);
        op();
        Signal(park_release, id);
    }

    /// Any thread: C calls Handler::BoundaryWork at its next record boundary of ring 3 (now if
    /// it waits for records). Several asks before it runs are one. Nothing without cut c.
    void RequestBoundaryWork() {
        if (!to_c) {
            return;
        }
        boundary_work.store(true, std::memory_order_seq_cst);
        to_c->ring.Wake();
    }

    /// C: inside a record of ring 3 (its thunk runs), where ParkB cannot run ring 3 on.
    [[nodiscard]] bool CInRecord() const {
        return c_in_record;
    }

private:
    using NoInterrupt = typename StageRing<BWords, Scopes>::NoInterrupt;
    using NoInterruptC = typename StageRing<CWords, Scopes>::NoInterrupt;

    struct Consumer {
        u64 read{};
        u64 available{};
        u32 joins_served{};
        u32 verify_count{};
    };

    struct CaptureDeltas {
        Handler& handler;
        u32 Pending() {
            return handler.PendingDeltas();
        }
        void Take(u64* out, u32 count) {
            handler.TakeDeltas(out, count);
        }
    };
    struct PassedDeltas {
        std::vector<u64>& words;
        u32 Pending() {
            return static_cast<u32>(words.size());
        }
        void Take(u64* out, u32 count) {
            std::memcpy(out, words.data(), size_t{count} * sizeof(u64));
            words.clear();
        }
    };
    static void NopThunk(const u64*, u32) {}

    /// Sets C's in-record flag for a record's run, back to what it was after (a record C runs
    /// in its ParkB wait sits between two). Nothing for B (null).
    struct InRecord {
        explicit InRecord(bool* flag_) : flag{flag_}, was{flag_ != nullptr && *flag_} {
            if (flag != nullptr) {
                *flag = true;
            }
        }
        ~InRecord() {
            if (flag != nullptr) {
                *flag = was;
            }
        }
        InRecord(const InRecord&) = delete;
        InRecord& operator=(const InRecord&) = delete;
        bool* flag;
        bool was;
    };

    template <typename L, typename Deltas, typename Interrupt>
    u64* Begin(L& link, Deltas& deltas, RecordKind kind, u32 body_words, u32 flags,
               Interrupt interrupt) {
        const u32 delta_words = deltas.Pending();
        const u32 words = 1 + delta_words + body_words;
        u64* const record = link.ring.Reserve(words, interrupt);
        link.last_start = link.ring.WritePosition();
        link.last_kind = kind;
        link.pending_words = words;
        record[0] = MakeHeader(kind, delta_words + body_words, delta_words, ++link.seq, flags);
        if (delta_words != 0) {
            deltas.Take(record + 1, delta_words);
        }
        return record + 1 + delta_words;
    }

    template <typename L, typename Interrupt>
    void End(L& link, u8 to_stage, Interrupt interrupt) {
        link.ring.Commit(link.pending_words);
        handler.Wrote(to_stage, size_t{link.pending_words} * sizeof(u64));
        if (options.mode == 2 && link.last_kind != RecordKind::Join) {
            link.ring.WaitConsumed(link.ring.WritePosition(), interrupt);
        }
    }

    [[nodiscard]] bool ParkRequested() const {
        return park_request.load(std::memory_order_acquire) != park_served;
    }

    /// B at a record boundary of ring 2 or its wait for ring 2 (its only park points, K3): parks
    /// for C's request (ParkB) until C released it. First C gets everything B has for it: the
    /// deltas it has not passed (C's replica where B's is), ring 3 published up to the position
    /// C runs on to before its op (R2 F2).
    void ServePark() {
        const u32 id = park_request.load(std::memory_order_acquire);
        park_served = id;
        LinkC& link = *to_c;
        if (!passed.empty()) {
            FlushPassed();
        }
        link.ring.Publish();
        park_position.store(link.ring.WritePosition(), std::memory_order_relaxed);
        handler.Parked();
        Signal(park_ack, id);
        // C may sleep waiting for ring 3's records in its ParkB wait (or, =1 inside a record, on
        // c_wake).
        link.ring.Wake();
        Nudge(c_wake);
        AwaitValue<typename Scopes::ParkWait>(park_release, id);
    }

    [[nodiscard]] bool JoinRequested() const {
        return join_request.load(std::memory_order_acquire) != join_served;
    }
    /// C at a record boundary or its wait for ring 3: B asked, and C ran everything B sent before.
    [[nodiscard]] bool JoinReached(u64 read) const {
        return JoinRequested() && read >= join_target.load(std::memory_order_relaxed);
    }

    /// C, its read position at the join's target (JoinReached): parks for B's JoinFromB until B
    /// released it. The ack carries `read` (B checks it, Handler::Hazard(JoinShort)).
    void ServeJoin(u64 read) {
        const u32 id = join_request.load(std::memory_order_acquire);
        join_served = id;
        join_read.store(read, std::memory_order_relaxed);
        Signal(join_ack, id);
        AwaitValue<typename Scopes::ParkWait>(join_release, id);
    }

    /// B: asks C to run ring 3 up to everything B committed and park; returns once it did.
    u32 AskJoin() {
        LinkC& link = *to_c;
        link.ring.Publish();
        const u32 id = ++join_count;
        const u64 target = link.ring.WritePosition();
        join_target.store(target, std::memory_order_relaxed);
        join_request.store(id, std::memory_order_seq_cst);
        // C may sleep waiting for ring 3's records, in its ParkB wait too.
        link.ring.Wake();
        Nudge(c_wake);
        AwaitValue<typename Scopes::JoinWait>(join_ack, id);
        const u64 read = join_read.load(std::memory_order_relaxed);
        if (read < target) [[unlikely]] {
            handler.Hazard(StageIdB, StageHazard::JoinShort, read, target);
        }
        return id;
    }
    void EndJoin(u32 id) {
        Signal(join_release, id);
        to_c->ring.PublishSoon();
    }

    /// C in ParkB until B parked (its ack) and, with `run_on`, C ran ring 3 up to where B parked:
    /// meanwhile it runs ring 3 on (B, maybe blocked on C, never waits on the request) and
    /// answers B's joins once reached (R2 F1, F2). Without `run_on` (=1 inside a record) only
    /// B's ack. Ends too once B stopped (RunB acked what it saw): nothing runs beside C any more.
    void AwaitParked(u32 id, bool run_on) {
        const auto parked = [this, id] {
            return park_ack.load(std::memory_order_acquire) == id ||
                   !b_running.load(std::memory_order_seq_cst);
        };
        if (!run_on) {
            AwaitServing<typename Scopes::ParkWait>(parked, c_wake, [] { return false; });
            return;
        }
        LinkC& link = *to_c;
        for (;;) {
            if (parked()) {
                if (!b_running.load(std::memory_order_seq_cst) ||
                    c.read >= park_position.load(std::memory_order_relaxed)) {
                    return;
                }
            }
            if (JoinReached(c.read)) {
                ServeJoin(c.read);
                continue;
            }
            if (c.read == c.available) {
                c.available =
                    link.ring.WaitForWork(c.read, [&] { return parked() || JoinReached(c.read); });
                continue;
            }
            Step<false>(link, c);
            if (options.mode != 2) {
                // Room for B, which may wait for it.
                link.ring.Release(c.read);
            }
        }
    }

    /// C at a record boundary: what RequestBoundaryWork asked for. An ask made while it runs
    /// runs again after.
    void RunBoundaryWork() {
        boundary_work.store(false, std::memory_order_seq_cst);
        handler.BoundaryWork();
    }

    /// B: the deltas it ran that C has not got, at most one header's (MaxDeltaWords) before they
    /// go in a record of their own; each record from A holds at most that many.
    void PassDeltas(const u64* words, u32 count) {
        if (passed.size() + count > MaxDeltaWords) {
            FlushPassed();
        }
        passed.insert(passed.end(), words, words + count);
    }

    void FlushPassed() {
        u64* const body = BeginC(RecordKind::Call, 1);
        body[0] = reinterpret_cast<u64>(&NopThunk);
        EndC();
    }

    /// =2: Handler::Verify every verify_every records, Joins included (K8).
    void MaybeVerify(u8 stage, Consumer& self) {
        if (++self.verify_count >= options.verify_every) {
            self.verify_count = 0;
            handler.Verify(stage);
        }
    }

    /// Runs the record at the consumer's read position (a Wrap: moves to the ring's start).
    template <bool IsB, typename L>
    void Step(L& in, Consumer& self) {
        constexpr u8 stage = IsB ? StageIdB : StageIdC;
        const u64 start = self.read;
        const u64* const record = in.ring.Read(start);
        const u64 header = record[0];
        const RecordKind kind = HeaderKind(header);
        if (kind == RecordKind::Wrap) {
            self.read = in.ring.AfterWrap(start);
            return;
        }
        const u32 words = HeaderWords(header);
        const u32 delta_words = HeaderDeltaWords(header);
        self.read = start + 1 + words;
        // C inside the record (ParkB's choice, CInRecord); the record may run inside C's ParkB
        // wait, at its boundary.
        [[maybe_unused]] const InRecord in_record{IsB ? nullptr : &c_in_record};
        if (options.mode == 2 || options.check) {
            // Every record, in order: the 16 bits compared for equality (depth 1, K5).
            const u16 expected = ++in.seen_seq;
            if (HeaderSeq(header) != expected) [[unlikely]] {
                handler.BadSequence(stage, expected, HeaderSeq(header));
                in.seen_seq = HeaderSeq(header);
            }
        }
        if (delta_words != 0) {
            handler.Apply(stage, record + 1, delta_words);
            if constexpr (IsB) {
                if (to_c) {
                    PassDeltas(record + 1, delta_words);
                }
            }
        }
        const u64* const body = record + 1 + delta_words;
        const u32 body_words = words - delta_words;
        switch (kind) {
        case RecordKind::Call:
            reinterpret_cast<RecordThunk>(body[0])(body + 1, body_words - 1);
            break;
        case RecordKind::Forward:
            reinterpret_cast<RecordRing1Thunk>(body[0])(body + 1);
            break;
        case RecordKind::Target:
            handler.Target(stage, body[0]);
            if constexpr (IsB) {
                if (to_c) {
                    u64* const out = BeginC(RecordKind::Target, 1);
                    out[0] = body[0];
                    EndC();
                }
            }
            break;
        case RecordKind::CbData:
            handler.CbData(stage, body[0], reinterpret_cast<const u32*>(body + 2),
                           static_cast<u32>(body[1] / sizeof(u32)));
            break;
        case RecordKind::ExecA:
            handler.ExecA(body[0], body[1]);
            break;
        case RecordKind::Mark:
            if constexpr (IsB) {
                if (to_c) {
                    u64* const out = BeginC(RecordKind::Mark, 1, HeaderFlags(header));
                    out[0] = body[0];
                    EndC();
                    // C stores it (D1): the front's Drain waits for it, B may sleep or run long
                    // records next.
                    to_c->ring.Publish();
                    break;
                }
            }
            handler.Mark(stage, body[0], (HeaderFlags(header) & MarkCaughtUp) != 0);
            break;
        case RecordKind::Join:
            Park<IsB>(in, self);
            break;
        default:
            break;
        }
        if (options.mode == 2) {
            // Lockstep: the producer waits for this release, so the primaries are where this
            // record left them (a Join verified before its ack, Park). Not after a Target: it
            // carries no state, and A may have touched the new target's primary before selecting
            // it (its writes go with the next record); that record checks it (R1 dikis-4).
            if (kind != RecordKind::Join && kind != RecordKind::Target) {
                MaybeVerify(stage, self);
            }
            in.ring.Release(self.read);
        }
    }

    /// A Join record: =2 verifies first (the deltas applied, the joiner waiting; K8); B with
    /// cut c makes C park at the same point; then ack, wait.
    template <bool IsB, typename L>
    void Park(L& in, Consumer& self) {
        constexpr u8 stage = IsB ? StageIdB : StageIdC;
        const u32 id = ++in.joins_seen;
        if (options.mode == 2) {
            MaybeVerify(stage, self);
        }
        [[maybe_unused]] u32 c_id = 0;
        if constexpr (IsB) {
            if (to_c) {
                LinkC& out = *to_c;
                c_id = ++out.joins;
                static_cast<void>(BeginC(RecordKind::Join, 0));
                EndC();
                out.ring.Publish();
                // No park point (K3): C runs ring 3 on to this Join, in its ParkB wait too.
                AwaitValue<typename Scopes::JoinWait>(out.ack, c_id);
            }
        }
        Signal(in.ack, id);
        AwaitValue<typename Scopes::ParkWait>(in.release, id);
        if constexpr (IsB) {
            if (to_c) {
                Signal(to_c->release, c_id);
                to_c->ring.PublishSoon();
            }
        }
        ++self.joins_served;
    }

    /// The other stage's request this one answers at a park point: B parks for C's ParkB, C for
    /// B's JoinFromB once it ran what B sent before. Whether it served one.
    template <bool IsB>
    bool ServeRequest(const Consumer& self) {
        if constexpr (IsB) {
            if (to_c && ParkRequested()) {
                ServePark();
                return true;
            }
        } else {
            if (JoinReached(self.read)) {
                ServeJoin(self.read);
                return true;
            }
        }
        return false;
    }

    /// The loop of B and C: batches of the records published, room handed back every 4096 words
    /// and at the end of a batch.
    template <bool IsB, typename L>
    void Loop(L& in, Consumer& self, std::atomic<bool>& stop) {
        constexpr u8 stage = IsB ? StageIdB : StageIdC;
        const auto stopping = [&] {
            if constexpr (IsB) {
                if (to_c && ParkRequested()) {
                    return true;
                }
            } else {
                if (JoinReached(self.read) || boundary_work.load(std::memory_order_relaxed)) {
                    return true;
                }
            }
            return stop.load(std::memory_order_relaxed);
        };
        for (;;) {
            handler.Report(stage);
            self.available = in.ring.WaitForWork(self.read, stopping);
            if (ServeRequest<IsB>(self)) {
                continue;
            }
            if (stop.load(std::memory_order_relaxed)) {
                return;
            }
            if constexpr (!IsB) {
                if (boundary_work.load(std::memory_order_relaxed)) {
                    // Asked while C waited for records (ParkB may run ring 3 on).
                    handler.Busy(stage, true);
                    RunBoundaryWork();
                    in.ring.Release(self.read);
                    handler.Busy(stage, false);
                    continue;
                }
            }
            if (self.available == self.read) {
                continue;
            }
            handler.Busy(stage, true);
            u64 handed = self.read;
            while (self.read != self.available) {
                Step<IsB>(in, self);
                ServeRequest<IsB>(self);
                if constexpr (!IsB) {
                    // A record boundary: what C left for one (ParkB may run ring 3 on, keeping
                    // `available` at or past `read`).
                    if (boundary_work.load(std::memory_order_relaxed)) [[unlikely]] {
                        RunBoundaryWork();
                    }
                }
                if (stop.load(std::memory_order_relaxed)) [[unlikely]] {
                    break;
                }
                // Hand ring space back now and then while working through a long batch.
                if (self.read - handed >= 4096 && self.read != self.available) {
                    in.ring.Release(self.read);
                    handed = self.read;
                }
            }
            if constexpr (IsB) {
                // The end of B's batch: what it sent C is visible before B sleeps (ring 2's
                // producer, A, publishes at the end of its batch the same way).
                if (to_c) {
                    to_c->ring.Publish();
                }
            }
            in.ring.Release(self.read);
            handler.Busy(stage, false);
        }
    }

    void RunB() {
        handler.ThreadStart(StageIdB);
        Loop<true>(*to_b, b, stop_b);
        // C waiting for B to park (ParkB) runs alone from here.
        b_running.store(false, std::memory_order_seq_cst);
        const u32 id = park_request.load(std::memory_order_seq_cst);
        if (id != park_served) {
            park_served = id;
            Signal(park_ack, id);
        }
        if (to_c) {
            to_c->ring.Wake();
            Nudge(c_wake);
        }
    }

    void RunC() {
        handler.ThreadStart(StageIdC);
        Loop<false>(*to_c, c, stop_c);
    }

    // What A reads for each record (BeginB, EndB): one line, written by Start and Stop, and by
    // A alone at its joins (a_in_join); apart from what B and C write for each record (R1
    // sifir-3).
    Handler& handler;
    StageOptions options;
    bool running{};
    /// A: inside JoinABegin / JoinAEnd.
    bool a_in_join{};
    std::unique_ptr<LinkB> to_b;
    std::unique_ptr<LinkC> to_c;

    // B's own.
    alignas(64) Consumer b;
    /// B: the deltas it ran that C has not got yet (cut c).
    std::vector<u64> passed;
    u32 park_served{}; ///< B: the last ParkB request it served
    u32 join_count{};  ///< B: JoinFromB requests made

    // C's own.
    alignas(64) Consumer c;
    u32 park_count{};  ///< C: ParkB requests made
    u32 join_served{}; ///< C: the last JoinFromB request it served
    bool c_in_record{}; ///< C: inside a record of ring 3 (Step)

    // ParkB: C asks (park_request), B acks once parked (ring 3 published up to park_position),
    // C runs ring 3 up to there, then releases.
    alignas(64) std::atomic<u32> park_request{};
    alignas(64) std::atomic<u32> park_ack{};
    std::atomic<u64> park_position{};
    alignas(64) std::atomic<u32> park_release{};
    // JoinFromB: B asks (join_request, for ring 3 up to join_target), C acks parked (with its
    // read position, join_read), B releases.
    alignas(64) std::atomic<u32> join_request{};
    std::atomic<u64> join_target{};
    alignas(64) std::atomic<u32> join_ack{};
    std::atomic<u64> join_read{};
    alignas(64) std::atomic<u32> join_release{};
    /// Where C sleeps in ParkB inside a record (=1, a hazard: AwaitServing).
    alignas(64) std::atomic<u32> c_wake{};
    /// C's boundary work asked (RequestBoundaryWork).
    alignas(64) std::atomic<bool> boundary_work{};
    std::atomic<bool> b_running{};
    alignas(64) std::atomic<bool> stop_b{};
    std::atomic<bool> stop_c{};

    std::jthread b_thread;
    std::jthread c_thread;
};

} // namespace VideoCommon::Nx2Stages
