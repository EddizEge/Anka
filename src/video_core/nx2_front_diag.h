// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <utility>
#include <vector>

#include "common/common_types.h"

// EDEN_NX2_GPU_PROFILE: what the split GPU thread's front (DmaPusher, the puller, Maxwell3D's
// front macros) spends its time on, logged per profile window by nx2_front_diag_hooks.h. This
// header keeps the logic, apart from the emulator so it can be tested.
//
// MME DMA reads (Maxwell3D::FrontDmaRead) drain the back first. Whether a read needed it is asked
// of PendingWrites: the guest memory writes the front sent to the back that may not have run yet
// (the ring position each entry ends by, SentEndBound, against how far the back ran) and the work
// sent whose writes the front cannot tell (draws, dispatches, blits). Observes only.
namespace VideoCommon::Nx2FrontDiag {

/// Writes of guest memory the front sends with the range they write.
enum class WriteKind : u8 {
    CbData,   ///< Maxwell3D constant buffer data (CB_DATA)
    Inline,   ///< Inline to memory: Maxwell3D, KeplerMemory and KeplerCompute uploads
    Dma,      ///< MaxwellDMA copies and fills into pitch memory
    Release,  ///< Semaphore releases: the puller's, MaxwellDMA's
    Report,   ///< Maxwell3D report semaphores (query results, releases)
    Download, ///< FlushRegion commands (cache data copied back to guest memory)
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(WriteKind::Count)> write_kind_names{
    "cb data", "inline", "dma", "release", "report", "download",
};

/// Work sent to the back that may write guest memory where the front cannot tell.
enum class WorkKind : u8 {
    Draw,      ///< Maxwell3D draw manager methods: draws, clears (storage and image stores)
    ResetFix,  ///< Of them: draws with NVN's indirect draw reset program (RESETFIX's records)
    Compute,   ///< KeplerCompute launches, dispatches, macros, report semaphores, inline QMDs
    Copy2D,    ///< Fermi2D blits
    DmaOther,  ///< MaxwellDMA copies into block linear memory
    Fences,    ///< ReleaseFences sent to the back (fenced query results)
    Tick,      ///< GPU tick work (presents, CPU flush requests)
    Run,       ///< Run commands
    Syncpoint, ///< Syncpoint increments (the syncpoint shim's words change)
    Unranged,  ///< Writes the diag cannot range: too wide, registers not seen, unbacked sparse
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(WorkKind::Count)> work_kind_names{
    "draw", "resetfix", "compute", "2d", "dma other", "fences", "tick", "run", "syncpoint", "unranged",
};

/// The widest write noted page by page; wider ones are work (WorkKind::Unranged).
inline constexpr u64 MaxNotedBytes = u64{64} << 20;

/// A ring position at or after the end of everything the front sent (GpuSplit): the write
/// position, or with macro writes held (EDEN_NX2_WRITE_BATCH) the furthest their batch entry can
/// end. The batch goes at the write position before any other entry, a header and up to
/// `max_held` writes; at the ring's start if it would cross the end (entries never wrap).
[[nodiscard]] constexpr u64 SentEndBound(u64 write_position, u32 held, u64 ring_words,
                                         u32 max_held) noexcept {
    if (held == 0) {
        return write_position;
    }
    const u64 words = 1 + u64{max_held};
    const u64 index = write_position & (ring_words - 1);
    if (index + words <= ring_words) {
        return write_position + words;
    }
    return write_position - index + ring_words + words;
}

/// The writes and work sent to the back and not known to have run, front thread.
class PendingWrites {
public:
    static constexpr size_t MaxEntries = size_t{1} << 16;

    /// What may still be pending over a range.
    struct Pending {
        u32 writes{};     ///< Bit per WriteKind of a pending write intersecting the range
        u32 work{};       ///< Bit per WorkKind with work pending
        bool overflow{};  ///< Writes were dropped (list full) that may not have run
        u32 entries{};    ///< Writes pending in all
    };

    /// A write of [begin, end) (device addresses) sent to the back, in the ring by `bound`.
    void NoteWrite(WriteKind kind, u64 begin, u64 end, u64 bound) {
        if (begin >= end) {
            return;
        }
        bound = std::max(bound, last_bound);
        last_bound = bound;
        if (count != 0) {
            // Touching or overlapping the last write of the kind: one range (runs of CB_DATA).
            Entry& last = entries[(head + count - 1) % MaxEntries];
            if (last.kind == kind && begin <= last.end && end >= last.begin) {
                last.begin = std::min(last.begin, begin);
                last.end = std::max(last.end, end);
                last.bound = bound;
                return;
            }
        }
        if (count == MaxEntries) {
            overflow_bound = bound;
            return;
        }
        if (entries.size() < MaxEntries) {
            entries.resize(MaxEntries);
        }
        entries[(head + count) % MaxEntries] = {begin, end, bound, kind};
        ++count;
    }

    /// Work whose writes the front cannot tell, in the ring by `bound`.
    void NoteWork(WorkKind kind, u64 bound) {
        u64& latest = work_bound[static_cast<size_t>(kind)];
        latest = std::max(latest, bound);
    }

    /// The back ran the ring up to `consumed`: what was in by then ran.
    void Retire(u64 consumed) {
        while (count != 0 && entries[head].bound <= consumed) {
            head = (head + 1) % MaxEntries;
            --count;
        }
    }

    [[nodiscard]] Pending Query(u64 begin, u64 end, u64 consumed) {
        Retire(consumed);
        Pending pending;
        pending.entries = static_cast<u32>(count);
        pending.overflow = overflow_bound > consumed;
        for (size_t i = 0; begin < end && i < count; ++i) {
            const Entry& entry = entries[(head + i) % MaxEntries];
            if (entry.begin < end && begin < entry.end) {
                pending.writes |= 1U << static_cast<u32>(entry.kind);
            }
        }
        for (size_t kind = 0; kind < work_bound.size(); ++kind) {
            if (work_bound[kind] > consumed) {
                pending.work |= 1U << kind;
            }
        }
        return pending;
    }

    [[nodiscard]] size_t Size() const {
        return count;
    }

    /// A new split session: ring positions start over.
    void Reset() {
        *this = PendingWrites{};
    }

private:
    struct Entry {
        u64 begin;
        u64 end;
        u64 bound;
        WriteKind kind;
    };

    std::vector<Entry> entries; ///< A ring of MaxEntries, allocated at the first write
    size_t head{};
    size_t count{};
    u64 last_bound{};
    u64 overflow_bound{};
    std::array<u64, static_cast<size_t>(WorkKind::Count)> work_bound{};
};

/// Where an MME DMA read stands, the first that applies.
enum class ReadClass : u8 {
    Idle,     ///< The back had run everything sent: the drain would not wait
    WriteHit, ///< A pending write of a known range intersects it (or writes were dropped)
    GpuPage,  ///< A page of it was written by the GPU through the buffer cache (ever)
    Work,     ///< Work whose writes the front cannot tell is pending
    Free,     ///< None of these
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(ReadClass::Count)> read_class_names{
    "idle", "write hit", "gpu page", "work", "free",
};

[[nodiscard]] constexpr ReadClass Classify(bool idle, const PendingWrites::Pending& pending,
                                           bool gpu_page) noexcept {
    if (idle) {
        return ReadClass::Idle;
    }
    if (pending.writes != 0 || pending.overflow) {
        return ReadClass::WriteHit;
    }
    if (gpu_page) {
        return ReadClass::GpuPage;
    }
    if (pending.work != 0) {
        return ReadClass::Work;
    }
    return ReadClass::Free;
}

/// [begin, end) GPU addresses an inline to memory upload (Upload::State) writes. Pitch: its lines;
/// block linear: the whole surface as Texture::CalculateSize gives it, bytes wide.
[[nodiscard]] constexpr std::pair<u64, u64> InlineRange(u64 dest, u32 line_length, u32 line_count,
                                                        u32 pitch, bool linear, u32 width,
                                                        u32 height, u32 depth, u32 block_height,
                                                        u32 block_depth) noexcept {
    if (line_length == 0 || line_count == 0) {
        return {dest, dest};
    }
    if (linear) {
        return {dest, dest + u64{pitch} * (line_count - 1) + line_length};
    }
    const auto align = [](u64 value, u32 shift) {
        return (value + (u64{1} << shift) - 1) >> shift << shift;
    };
    const u64 size =
        align(width, 6) * align(height, 3 + block_height) * align(std::max(depth, 1U), block_depth);
    return {dest, dest + size};
}

/// [begin, end) GPU addresses a MaxwellDMA launch writes into pitch memory, or nothing for a
/// block linear destination (WorkKind::DmaOther). Multi-line from pitch memory: its lines `pitch`
/// apart (a negative pitch: unknown). Multi-line from block linear memory (CopyBlockLinearToPitch):
/// |pitch| bytes a line written back whole, a line up to 16 bytes an element with a remap.
/// Single line: the bytes (words with a remap), in whole 512-byte blocks (the copy may swizzle
/// within them).
struct DmaWrite {
    u64 begin{};
    u64 end{};
    bool known{};
};

[[nodiscard]] constexpr DmaWrite DmaRange(u64 offset_out, u32 pitch_out, u32 line_length,
                                          u32 line_count, bool multi_line, bool src_pitch,
                                          bool dst_pitch, bool remap) noexcept {
    if (multi_line) {
        if (!dst_pitch) {
            return {};
        }
        if (line_length == 0 || line_count == 0) {
            return {offset_out, offset_out, true};
        }
        const s32 signed_pitch = static_cast<s32>(pitch_out);
        if (!src_pitch) {
            const u64 pitch = signed_pitch < 0 ? u64{0U - pitch_out} : u64{pitch_out};
            const u64 line = u64{line_length} * (remap ? 16 : 1);
            const u64 bytes = std::max(pitch * line_count, pitch * (line_count - 1) + line);
            return {offset_out, offset_out + bytes, true};
        }
        if (signed_pitch < 0) {
            return {};
        }
        return {offset_out, offset_out + u64{pitch_out} * (line_count - 1) + line_length, true};
    }
    const u64 bytes = u64{line_length} * (remap ? 4 : 1);
    const u64 begin = offset_out & ~u64{0x1ff};
    const u64 end = (offset_out + bytes + 0x1ff) & ~u64{0x1ff};
    return {begin, bytes == 0 ? begin : end, true};
}

/// Cheap sampling for the timed counters: xorshift32, one call in 2^bits.
class Sampler {
public:
    [[nodiscard]] bool Next(u32 bits) noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (state & ((1U << bits) - 1)) == 0;
    }

private:
    u32 state{0x9e3779b9U};
};

/// Front macro calls by their start address: calls, sampled exclusive time (nested profile zones
/// out: drains, ring waits) and the nested time itself, DMA reads; JIT or HLE (FrontIndirectDraw);
/// the JIT calls' emits (with those the same value filter dropped) and register reads (MME_READ).
class MacroTable {
public:
    static constexpr size_t Slots = 512;

    struct Entry {
        u32 start{};
        bool used{};
        u64 calls{};
        u64 hle_calls{};
        u64 sampled{};
        u64 ticks{};       ///< Of the sampled calls: exclusive
        u64 child_ticks{}; ///< Of the sampled calls: in nested zones
        u64 dma_reads{};
        u64 dma_words{};
        u64 emits{};
        u64 reads{};

        /// Exclusive ticks over all calls (the sample's mean times the calls).
        [[nodiscard]] double EstimatedTicks() const {
            return sampled != 0 ? static_cast<double>(ticks) * static_cast<double>(calls) /
                                      static_cast<double>(sampled)
                                : 0.0;
        }
    };

    /// The entry of `start`, or the shared one of the rest when the table is full.
    Entry& Find(u32 start) {
        size_t slot = (start * 0x9E3779B1U) >> 23;
        for (size_t probe = 0; probe < Slots; ++probe, slot = (slot + 1) % Slots) {
            Entry& entry = slots[slot];
            if (!entry.used) {
                entry.used = true;
                entry.start = start;
                ++used;
                return entry;
            }
            if (entry.start == start) {
                return entry;
            }
        }
        rest.start = ~0U;
        return rest;
    }

    /// The `n` entries with the most estimated time, most first.
    [[nodiscard]] std::vector<const Entry*> Top(size_t n) const {
        std::vector<const Entry*> top;
        for (const Entry& entry : slots) {
            if (entry.used && entry.calls != 0) {
                top.push_back(&entry);
            }
        }
        if (rest.calls != 0) {
            top.push_back(&rest);
        }
        std::sort(top.begin(), top.end(), [](const Entry* a, const Entry* b) {
            const double ta = a->EstimatedTicks();
            const double tb = b->EstimatedTicks();
            return ta != tb ? ta > tb : a->calls > b->calls;
        });
        if (top.size() > n) {
            top.resize(n);
        }
        return top;
    }

    [[nodiscard]] size_t Used() const {
        return used;
    }

    void Clear() {
        slots.fill(Entry{});
        rest = Entry{};
        used = 0;
    }

private:
    std::array<Entry, Slots> slots{};
    Entry rest{};
    size_t used{};
};

/// 3D register writes by method (Maxwell3D::FrontWrite): all, those of the value the register
/// held, and those the front did not send (state writes of the same value); and by where they
/// came from (pushed, written by a front macro).
class RegisterWrites {
public:
    static constexpr u32 NumRegs = 0xE00;

    void Note(u32 method, bool same, bool skipped, bool from_macro = false) {
        if (method >= NumRegs) {
            return;
        }
        ++total[method];
        same_value[method] += same ? 1 : 0;
        skipped_writes[method] += skipped ? 1 : 0;
        auto& origin = by_origin[from_macro ? 1 : 0];
        ++origin[0];
        origin[1] += same ? 1 : 0;
        origin[2] += skipped ? 1 : 0;
    }

    /// All, same value and not sent of the pushed writes (false) or the macros' (true).
    [[nodiscard]] const std::array<u64, 3>& Origin(bool from_macro) const {
        return by_origin[from_macro ? 1 : 0];
    }

    struct Row {
        u32 method;
        u64 total;
        u64 same;
        u64 skipped;
    };

    /// The `n` methods with the most same value writes, most first.
    [[nodiscard]] std::vector<Row> Top(size_t n) const {
        std::vector<Row> rows;
        for (u32 method = 0; method < NumRegs; ++method) {
            if (same_value[method] != 0) {
                rows.push_back({method, total[method], same_value[method], skipped_writes[method]});
            }
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.same != b.same ? a.same > b.same : a.method < b.method;
        });
        if (rows.size() > n) {
            rows.resize(n);
        }
        return rows;
    }

    /// Sums over every method: all, same value, skipped.
    [[nodiscard]] std::array<u64, 3> Totals() const {
        std::array<u64, 3> sums{};
        for (u32 method = 0; method < NumRegs; ++method) {
            sums[0] += total[method];
            sums[1] += same_value[method];
            sums[2] += skipped_writes[method];
        }
        return sums;
    }

    void Clear() {
        total.fill(0);
        same_value.fill(0);
        skipped_writes.fill(0);
        by_origin = {};
    }

private:
    std::array<u64, NumRegs> total{};
    std::array<u64, NumRegs> same_value{};
    std::array<u64, NumRegs> skipped_writes{};
    std::array<std::array<u64, 3>, 2> by_origin{};
};

/// DmaPusher's command headers to Maxwell3D's front: increasing runs of state methods by length
/// (1, 2, 3-4, 5-8, more words), and how increase-once macro calls arrive (their first word as one
/// method call, the rest as one more within the same command list, or split across lists).
struct FrontRuns {
    static constexpr std::size_t Buckets = 5;
    std::array<u64, Buckets> state_runs{};
    u64 state_words{};
    u64 macro_runs{};       ///< Increasing runs into the macro range
    u64 once_one{};         ///< Increase once, macro range, one word: one send
    u64 once_two{};         ///< ... more words, all in the list: two sends
    u64 once_split{};       ///< ... words past the list's end: more
    u64 once_state{};       ///< Increase once below the macro range

    [[nodiscard]] static constexpr std::size_t Bucket(u32 words) noexcept {
        return words <= 1 ? 0 : words == 2 ? 1 : words <= 4 ? 2 : words <= 8 ? 3 : 4;
    }

    /// A header of `words` words; `macro`: its method in the macro range; `in_list`: all its
    /// words in the command list it starts in.
    void Note(bool increase_once, bool macro, u32 words, bool in_list) {
        if (words == 0) {
            return;
        }
        if (!increase_once) {
            if (macro) {
                ++macro_runs;
            } else {
                ++state_runs[Bucket(words)];
                state_words += words;
            }
            return;
        }
        if (!macro) {
            ++once_state;
        } else if (!in_list) {
            ++once_split;
        } else if (words == 1) {
            ++once_one;
        } else {
            ++once_two;
        }
    }
};

/// A drain's wait in time stamp counter ticks, by power of two.
class DrainHistogram {
public:
    static constexpr size_t Buckets = 48;
    /// Bands logged, upper bounds in microseconds (the last open).
    static constexpr std::array<double, 7> band_us{2, 8, 32, 128, 512, 2048, 8192};
    static constexpr size_t Bands = band_us.size() + 1;

    void Note(bool waited, u64 ticks) {
        ++calls;
        if (!waited) {
            return;
        }
        const size_t bucket = ticks <= 1 ? 0 : static_cast<size_t>(63 - std::countl_zero(ticks));
        ++buckets[std::min(bucket, Buckets - 1)];
    }

    /// Waits per band with `ns_per_tick`: each bucket goes where its middle (1.5 * 2^b ticks) is.
    [[nodiscard]] std::array<u64, Bands> ToBands(double ns_per_tick) const {
        std::array<u64, Bands> bands{};
        for (size_t bucket = 0; bucket < Buckets; ++bucket) {
            if (buckets[bucket] == 0) {
                continue;
            }
            const double us =
                1.5 * static_cast<double>(u64{1} << bucket) * ns_per_tick / 1000.0;
            size_t band = 0;
            while (band < band_us.size() && us >= band_us[band]) {
                ++band;
            }
            bands[band] += buckets[bucket];
        }
        return bands;
    }

    [[nodiscard]] u64 Calls() const {
        return calls;
    }

private:
    std::array<u64, Buckets> buckets{};
    u64 calls{};
};

/// The kind of write the front first saw sent to each device page (WriteKind + 1; 0 none), for
/// the pages MME DMA reads.
class FirstWriters {
public:
    explicit FirstWriters(u64 pages_) : pages{pages_} {}

    void Note(WriteKind kind, u64 begin, u64 end) {
        if (begin >= end) {
            return;
        }
        if (first.empty()) {
            first.resize(pages);
        }
        const u64 last = std::min((end - 1) >> PageBits, pages - 1);
        for (u64 page = begin >> PageBits; page <= last; ++page) {
            if (first[page] == 0) {
                first[page] = static_cast<u8>(static_cast<u8>(kind) + 1);
            }
        }
    }

    /// WriteKind + 1 of the page's first write, 0 if none was seen.
    [[nodiscard]] u8 Of(u64 address) const {
        const u64 page = address >> PageBits;
        return first.empty() || page >= pages ? 0 : first[page];
    }

    void Clear() {
        std::vector<u8>().swap(first);
    }

    static constexpr u64 PageBits = 12;

private:
    u64 pages;
    std::vector<u8> first;
};

} // namespace VideoCommon::Nx2FrontDiag
