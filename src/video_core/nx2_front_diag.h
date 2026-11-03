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
namespace VideoCommon::Nx2FrontDiag {

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
/// out: drains, ring waits) and the nested time itself, DMA reads; JIT or HLE (FrontIndirectDraw).
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
/// held, and those the front did not send (state writes of the same value).
class RegisterWrites {
public:
    static constexpr u32 NumRegs = 0xE00;

    void Note(u32 method, bool same, bool skipped) {
        if (method >= NumRegs) {
            return;
        }
        ++total[method];
        same_value[method] += same ? 1 : 0;
        skipped_writes[method] += skipped ? 1 : 0;
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
    }

private:
    std::array<u64, NumRegs> total{};
    std::array<u64, NumRegs> same_value{};
    std::array<u64, NumRegs> skipped_writes{};
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

} // namespace VideoCommon::Nx2FrontDiag
