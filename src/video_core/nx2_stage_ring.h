// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <type_traits>

#include "common/common_types.h"

// Back stages (EDEN_NX2_BACK_STAGES): the record format of the rings between the stages and the
// ring itself, header-only so the unit tests take them alone. Owner: T1 (the format is the
// contract, nx2_stages.h).
namespace VideoCommon::Nx2Stages {

/// What a record of ring 2 (A to B) or ring 3 (B to C) holds after its header.
enum class RecordKind : u8 {
    Wrap = 0,    ///< The rest of the ring is unused: continue at its start (no words)
    Call = 1,    ///< Word 0 a Thunk, then its payload (Thunk gets the payload and its size)
    Mark = 2,    ///< Word 0 the ring 1 position A ran up to; the last stage stores it (D1)
    Join = 3,    ///< No words: run everything before it, park until released (seq: handshake)
    ExecA = 4,   ///< =3: word 0 the ring 1 position, word 1 the words to run with GpuSplit::Execute
    Forward = 5, ///< A ring 1 Call entry as it is: word 0 its Ring1Thunk, then its payload (S18)
    Target = 6,  ///< Word 0 the primary Maxwell3D the deltas from here on are for (ReplicaOf)
    CbData = 7,  ///< Word 0 the GPU address, word 1 the bytes, then the data (StageCbWrite, S5/3)
};

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
constexpr u64 MakeHeader(RecordKind kind, u32 words, u32 delta_words, u16 seq, u32 flags = 0) {
    return static_cast<u64>(kind) | (u64{flags & 0xf} << 4) | (u64{words & 0xffffff} << 8) |
           (u64{delta_words & 0xffff} << 32) | (u64{seq} << 48);
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

/// Most words one record may hold after its header.
inline constexpr u32 MaxRecordWords = 0xffffff;
/// Most delta words one header may announce; A's capture holds at most one per Maxwell3D register
/// (0xE00), so a flush always fits.
inline constexpr u32 MaxDeltaWords = 0xffff;

/// Ring 2: 2^19 words (4 MiB); ring 3 (K2 only): 2^18 words (2 MiB).
inline constexpr size_t RingToBWords = size_t{1} << 19;
inline constexpr size_t RingToCWords = size_t{1} << 18;

/// Single-producer single-consumer ring of u64 records (T1). The GpuSplit ring's protocol: the
/// producer reserves room for a whole record (Wrap at the end, waiting for space as
/// GpuSplit::WaitForSpace), commits it, and publishes every PublishEvery words, at the end of a
/// batch, and after a drain from 8 words doubling (GpuSplit::PublishRing); the consumer spins
/// briefly, then sleeps on an atomic wait (GpuSplit::BackLoop), and hands space back as it goes.
/// Either wait may be a park point (nx2_stages.h, K3: B's on ring 3 and ring 2, C's on ring 3):
/// it then also answers a park request, and the one asking wakes it where it sleeps.
template <size_t Words>
class StageRing {
public:
    static_assert((Words & (Words - 1)) == 0, "a power of two");
    static constexpr size_t Mask = Words - 1;
    static constexpr size_t PublishEvery = 256;

    // Producer side (T1): Reserve, Commit, Publish, WaitForSpace.
    // Consumer side (T1): Available, Consume, the wait for work.
};

} // namespace VideoCommon::Nx2Stages
