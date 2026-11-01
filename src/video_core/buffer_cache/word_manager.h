// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "video_core/host1x/gpu_device_memory_manager.h"

namespace VideoCommon {

constexpr u64 PAGES_PER_WORD = 64;
constexpr u64 BYTES_PER_PAGE = Core::DEVICE_PAGESIZE;
constexpr u64 BYTES_PER_WORD = PAGES_PER_WORD * BYTES_PER_PAGE;

enum class Type {
    CPU,
    GPU,
    CachedCPU,
    Untracked,
    Preflushable,
    Max
};

template <class DeviceTracker, size_t stack_words, size_t size_bytes>
struct WordManager {
    static constexpr size_t num_words = Common::DivCeil(size_bytes, BYTES_PER_WORD);

    explicit WordManager(VAddr cpu_addr_, DeviceTracker& tracker_) : tracker{&tracker_}, cpu_addr{cpu_addr_} {
        std::fill_n(heap.data() + size_t(Type::CPU) * num_words, num_words, ~u64{0});
        std::fill_n(heap.data() + size_t(Type::Untracked) * num_words, num_words, ~u64{0});
        // Clean up tailing bits
        u64 const last_word_size = size_bytes % BYTES_PER_WORD;
        u64 const last_local_page = Common::DivCeil(last_word_size, BYTES_PER_PAGE);
        u64 const shift = (PAGES_PER_WORD - last_local_page) % PAGES_PER_WORD;
        u64 const last_word = (~u64{0} << shift) >> shift;
        heap[num_words * size_t(Type::CPU) + num_words - 1] = last_word;
        heap[num_words * size_t(Type::Untracked) + num_words - 1] = last_word;
        for (size_t index = 0; index < num_words; ++index) {
            RefreshDirtyWord(index);
        }
    }
    explicit WordManager() = default;

    static u64 ExtractBits(u64 word, size_t page_start, size_t page_end) {
        constexpr size_t number_bits = sizeof(u64) * 8;
        const size_t limit_page_end = number_bits - (std::min)(page_end, number_bits);
        u64 bits = (word >> page_start) << page_start;
        bits = (bits << limit_page_end) >> limit_page_end;
        return bits;
    }

    static std::pair<size_t, size_t> GetWordPage(VAddr address) {
        const size_t converted_address = size_t(address);
        const size_t word_number = converted_address / BYTES_PER_WORD;
        const size_t amount_pages = converted_address % BYTES_PER_WORD;
        return std::make_pair(word_number, amount_pages / BYTES_PER_PAGE);
    }

    /// Word and page bounds IterateWords(offset, size) walks; empty when start_word == end_word.
    struct WordRange {
        size_t start_word = 0;
        size_t start_page = 0;
        size_t end_word = 0;
        size_t end_page = 0;
    };

    static WordRange GetWordRange(size_t offset, size_t size) {
        WordRange range{};
        const size_t start = size_t(std::max<s64>(s64(offset), 0LL));
        const size_t end = size_t(std::max<s64>(s64(offset + size), 0LL));
        if (start >= size_bytes || end <= start) {
            return range;
        }
        auto [start_word, start_page] = GetWordPage(start);
        auto [end_word, end_page] = GetWordPage(end + BYTES_PER_PAGE - 1ULL);
        start_word = (std::min)(start_word, num_words);
        end_word = (std::min)(end_word, num_words);
        const size_t diff = end_word - start_word;
        end_word += (end_page + PAGES_PER_WORD - 1ULL) / PAGES_PER_WORD;
        end_word = (std::min)(end_word, num_words);
        end_page += diff * PAGES_PER_WORD;
        range = {start_word, start_page, end_word, end_page};
        return range;
    }

    /// One bit per word in [start_word, end_word).
    static u64 WordRangeBits(const WordRange& range) {
        const u64 below_end = range.end_word >= 64 ? ~u64{0} : (u64{1} << range.end_word) - 1;
        const u64 below_start = range.start_word >= 64 ? ~u64{0} : (u64{1} << range.start_word) - 1;
        return below_end & ~below_start;
    }

    /// The page mask IterateWords passes for word 'index' of the range.
    static u64 WordPageMask(const WordRange& range, size_t index) {
        const size_t page_begin = index == range.start_word ? range.start_page : 0;
        const size_t page_end = range.end_page - (index - range.start_word) * PAGES_PER_WORD;
        return ExtractBits(~0ULL, page_begin, page_end);
    }

    // EDEN_NX2_SYNC_SKIP: a CPU upload query (ForEachModifiedRange with Type::CPU) visits only the
    // words cpu_dirty_words marks. A word whose CPU and Untracked bits are all clear yields no
    // range, no bit change and no tracker update there, so skipping it is exact.
    // EDEN_NX2_SYNC_SKIP=0 walks every word, as before; =2 also checks the bits on every query.
    static int SyncSkipMode() {
        if constexpr (num_words > 64) {
            return 0;
        } else {
            static const int mode = [] {
                const char* value = std::getenv("EDEN_NX2_SYNC_SKIP");
                return value == nullptr ? 1 : (value[0] == '0' ? 0 : (value[0] == '2' ? 2 : 1));
            }();
            return mode;
        }
    }

    /// Types whose ChangeRegionState / ForEachModifiedRange write the CPU or Untracked words.
    static constexpr bool TouchesCpuWords(Type type) {
        return type == Type::CPU || type == Type::CachedCPU || type == Type::Untracked;
    }

    /// Recomputes bit 'index' of cpu_dirty_words after word 'index' of CPU or Untracked changed.
    void RefreshDirtyWord(size_t index) noexcept {
        if constexpr (num_words <= 64) {
            const u64 bit = u64{1} << index;
            const bool dirty = (heap[num_words * size_t(Type::CPU) + index] |
                                heap[num_words * size_t(Type::Untracked) + index]) != 0;
            cpu_dirty_words = dirty ? (cpu_dirty_words | bit) : (cpu_dirty_words & ~bit);
        }
    }

    /// EDEN_NX2_SYNC_SKIP=2: logs (once) a word whose cpu_dirty_words bit disagrees with its words.
    void CheckDirtyWords() const {
        for (size_t index = 0; index < num_words; ++index) {
            const bool dirty = (heap[num_words * size_t(Type::CPU) + index] |
                                heap[num_words * size_t(Type::Untracked) + index]) != 0;
            if (dirty != (((cpu_dirty_words >> index) & 1) != 0)) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    LOG_ERROR(HW_GPU, "EDEN_NX2_SYNC_SKIP: stale dirty bit, word {} of {:#x}", index,
                              cpu_addr);
                }
                return;
            }
        }
    }

    /// False when ForEachModifiedRange(Type::CPU, ...) over [offset, offset + size) would do
    /// nothing: no covered word holds CPU or Untracked bits. Always true with EDEN_NX2_SYNC_SKIP=0.
    [[nodiscard]] bool MayHaveCpuDirtyWords(size_t offset, size_t size) const {
        const int skip_mode = SyncSkipMode();
        if (skip_mode == 0) {
            return true;
        }
        if (skip_mode == 2) {
            CheckDirtyWords();
        }
        return (cpu_dirty_words & WordRangeBits(GetWordRange(offset, size))) != 0;
    }

    template <typename Func>
    void IterateWords(size_t offset, size_t size, Func&& func) const {
        using FuncReturn = std::invoke_result_t<Func, std::size_t, u64>;
        const size_t start = size_t(std::max<s64>(s64(offset), 0LL));
        const size_t end = size_t(std::max<s64>(s64(offset + size), 0LL));
        if (!(start >= size_bytes || end <= start)) {
            auto [start_word, start_page] = GetWordPage(start);
            auto [end_word, end_page] = GetWordPage(end + BYTES_PER_PAGE - 1ULL);
            start_word = (std::min)(start_word, num_words);
            end_word = (std::min)(end_word, num_words);
            const size_t diff = end_word - start_word;
            end_word += (end_page + PAGES_PER_WORD - 1ULL) / PAGES_PER_WORD;
            end_word = (std::min)(end_word, num_words);
            end_page += diff * PAGES_PER_WORD;
            constexpr u64 base_mask{~0ULL};
            for (size_t word_index = start_word; word_index < end_word; word_index++) {
                const u64 mask = ExtractBits(base_mask, start_page, end_page);
                start_page = 0;
                end_page -= PAGES_PER_WORD;
                if constexpr (std::is_same_v<FuncReturn, bool>) { // bool return
                    if (func(word_index, mask))
                        return;
                } else {
                    func(word_index, mask);
                }
            }
        }
    }

    template <typename Func>
    void IteratePages(u64 mask, Func&& func) const {
        size_t offset = 0;
        while (mask != 0) {
            const size_t empty_bits = std::countr_zero(mask);
            offset += empty_bits;
            mask = mask >> empty_bits;

            const size_t continuous_bits = std::countr_one(mask);
            func(offset, continuous_bits);
            mask = continuous_bits < PAGES_PER_WORD ? (mask >> continuous_bits) : 0;
            offset += continuous_bits;
        }
    }

    /// @brief Change the state of a range of pages
    /// @param type          Type of the page
    /// @param enable        If enabling or disabling
    /// @param dirty_addr    Base address to mark or unmark as modified
    /// @param size          Size in bytes to mark or unmark as modified
    void ChangeRegionState(Type type, bool enable, u64 dirty_addr, u64 size) noexcept {
        std::span<u64> state_words = Span(type);
        [[maybe_unused]] std::span<u64> untracked_words = Span(Type::Untracked);
        [[maybe_unused]] std::span<u64> cached_words = Span(Type::CachedCPU);
        std::vector<std::pair<VAddr, u64>> ranges;
        IterateWords(dirty_addr - cpu_addr, size, [&](size_t index, u64 mask) {
            if (type == Type::CPU || type == Type::CachedCPU) {
                CollectChangedRanges(!enable, index, untracked_words[index], mask, ranges);
            }
            if (enable) {
                state_words[index] |= mask;
                if (type == Type::CPU || type == Type::CachedCPU)
                    untracked_words[index] |= mask;
                if (type == Type::CPU)
                    cached_words[index] &= ~mask;
            } else {
                if (type == Type::CPU)
                    cached_words[index] &= ~(state_words[index] & mask);
                state_words[index] &= ~mask;
                if (type == Type::CPU || type == Type::CachedCPU)
                    untracked_words[index] &= ~mask;
            }
            if (TouchesCpuWords(type))
                RefreshDirtyWord(index);
        });
        if (!ranges.empty()) {
            ApplyCollectedRanges(ranges, (!enable) ? 1 : -1);
        }
    }

    /// @brief Loop over each page in the given range.
    /// Turn off those bits and notify the tracker if needed. Call the given function on each turned off range.
    /// @param type            Type of the address
    /// @param clear           Whetever to clear
    /// @param query_cpu_range Base CPU address to loop over
    /// @param size            Size in bytes of the CPU range to loop over
    /// @param func            Function to call for each turned off region
    template <typename Func>
    void ForEachModifiedRange(Type type, bool clear, VAddr query_cpu_range, s64 size, Func&& func) {
        //static_assert(type != Type::Untracked);
        std::span<u64> state_words = Span(type);
        std::span<u64> untracked_words = Span(Type::Untracked);
        std::span<u64> cached_words = Span(Type::CachedCPU);
        size_t const offset = query_cpu_range - cpu_addr;
        // EDEN_NX2_SYNC_SKIP: a CPU query visits only the words cpu_dirty_words marks.
        const int skip_mode = type == Type::CPU ? SyncSkipMode() : 0;
        WordRange word_range{};
        if (skip_mode != 0) {
            if (skip_mode == 2) {
                CheckDirtyWords();
            }
            word_range = GetWordRange(offset, size_t(size));
            if ((cpu_dirty_words & WordRangeBits(word_range)) == 0) {
                return;
            }
        }
        bool pending = false;
        size_t pending_offset{};
        size_t pending_pointer{};
        const auto release = [&]() {
            func(cpu_addr + pending_offset * BYTES_PER_PAGE,
                 (pending_pointer - pending_offset) * BYTES_PER_PAGE);
        };
        std::vector<std::pair<VAddr, u64>> ranges;
        const auto per_word = [&](size_t index, u64 mask) {
            if (type == Type::GPU)
                mask &= ~untracked_words[index];
            const u64 word = state_words[index] & mask;
            if (clear) {
                if (type == Type::CPU || type == Type::CachedCPU) {
                    CollectChangedRanges(true, index, untracked_words[index], mask, ranges);
                }
                state_words[index] &= ~mask;
                if (type == Type::CPU || type == Type::CachedCPU)
                    untracked_words[index] &= ~mask;
                if (type == Type::CPU)
                    cached_words[index] &= ~word;
                if (TouchesCpuWords(type))
                    RefreshDirtyWord(index);
            }
            const size_t base_offset = index * PAGES_PER_WORD;
            IteratePages(word, [&](size_t pages_offset, size_t pages_size) {
                if (!pending) {
                    pending_offset = base_offset + pages_offset;
                    pending_pointer = base_offset + pages_offset + pages_size;
                    pending = true;
                } else if (pending_pointer == base_offset + pages_offset) {
                    pending_pointer += pages_size;
                } else {
                    func(cpu_addr + pending_offset * BYTES_PER_PAGE, (pending_pointer - pending_offset) * BYTES_PER_PAGE);
                    pending_offset = base_offset + pages_offset;
                    pending_pointer = base_offset + pages_offset + pages_size;
                }
            });
        };
        if (skip_mode != 0) {
            // Ascending, re-reading cpu_dirty_words before each word as IterateWords would.
            u64 remaining = WordRangeBits(word_range);
            for (u64 candidates = cpu_dirty_words & remaining; candidates != 0;
                 candidates = cpu_dirty_words & remaining) {
                const size_t index = size_t(std::countr_zero(candidates));
                remaining &= ~((u64{2} << index) - 1);
                per_word(index, WordPageMask(word_range, index));
            }
        } else {
            IterateWords(offset, size, per_word);
        }
        if (pending) {
            release();
        }
        if (!ranges.empty()) {
            ApplyCollectedRanges(ranges, 1);
        }
    }

    /// @brief Returns true when a region has been modified
    /// @param type   Type of region
    /// @param offset Offset in bytes from the start of the buffer
    /// @param size   Size in bytes of the region to query for modifications
    [[nodiscard]] bool IsRegionModified(Type type, u64 offset, u64 size) const noexcept {
        //static_assert(type != Type::Untracked);
        const std::span<const u64> state_words = Span(type);
        const std::span<const u64> untracked_words = Span(Type::Untracked);
        bool result = false;
        IterateWords(offset, size, [&](size_t index, u64 mask) {
            if (type == Type::GPU)
                mask &= ~untracked_words[index];
            return (state_words[index] & mask) != 0 ? (result = true) : false;
        });
        return result;
    }

    /// @brief Returns a begin end pair with the inclusive modified region
    /// @param offset Offset in bytes from the start of the buffer
    /// @param size   Size in bytes of the region to query for modifications
    [[nodiscard]] std::pair<u64, u64> ModifiedRegion(Type type, u64 offset, u64 size) const noexcept {
        //static_assert(type != Type::Untracked);
        const std::span<const u64> state_words = Span(type);
        const std::span<const u64> untracked_words = Span(Type::Untracked);
        u64 begin = (std::numeric_limits<u64>::max)(), end = 0;
        IterateWords(offset, size, [&](size_t index, u64 mask) {
            if (type == Type::GPU)
                mask &= ~untracked_words[index];
            const u64 word = state_words[index] & mask;
            if (word != 0) {
                const u64 local_page_begin = std::countr_zero(word);
                const u64 local_page_end = PAGES_PER_WORD - std::countl_zero(word);
                const u64 page_index = index * PAGES_PER_WORD;
                begin = (std::min)(begin, page_index + local_page_begin);
                end = page_index + local_page_end;
            }
        });
        return begin < end ? std::make_pair<u64, u64>(begin * BYTES_PER_PAGE, end * BYTES_PER_PAGE)
            : std::make_pair<u64, u64>(0, 0);
    }

    void FlushCachedWrites() noexcept {
        auto const cached_words = Span(Type::CachedCPU);
        auto const untracked_words = Span(Type::Untracked);
        auto const cpu_words = Span(Type::CPU);
        std::vector<std::pair<VAddr, u64>> ranges;
        for (u64 word_index = 0; word_index < num_words; ++word_index) {
            const u64 cached_bits = cached_words[word_index];
            CollectChangedRanges(false, word_index, untracked_words[word_index], cached_bits, ranges);
            untracked_words[word_index] |= cached_bits;
            cpu_words[word_index] |= cached_bits;
            cached_words[word_index] = 0;
            RefreshDirtyWord(word_index);
        }
        if (!ranges.empty()) {
            ApplyCollectedRanges(ranges, -1);
        }
    }

    /// @brief Notify tracker about changes in the CPU tracking state of a word in the buffer
    /// @param add_to_tracker If add to tracker (selects changed bits)
    /// @param word_index     Index to the word to notify to the tracker
    /// @param current_bits   Current state of the word
    /// @param new_bits       New state of the word
    /// @tparam add_to_tracker True when the tracker should start tracking the new pages
    void CollectChangedRanges(bool add_to_tracker, u64 word_index, u64 current_bits, u64 new_bits, std::vector<std::pair<VAddr, u64>>& out_ranges) const {
        u64 changed_bits = (add_to_tracker ? current_bits : ~current_bits) & new_bits;
        VAddr addr = cpu_addr + word_index * BYTES_PER_WORD;
        IteratePages(changed_bits, [&](size_t offset, size_t size) {
            out_ranges.emplace_back(addr + offset * BYTES_PER_PAGE, size * BYTES_PER_PAGE);
        });
    }

    void ApplyCollectedRanges(std::vector<std::pair<VAddr, u64>>& ranges, int delta) const {
        if (ranges.empty())
            return;
        std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        // Coalesce adjacent/contiguous ranges
        std::vector<std::pair<VAddr, size_t>> coalesced;
        coalesced.reserve(ranges.size());
        VAddr cur_addr = ranges[0].first;
        size_t cur_size = static_cast<size_t>(ranges[0].second);
        for (size_t i = 1; i < ranges.size(); ++i) {
            if (cur_addr + cur_size == ranges[i].first) {
                cur_size += static_cast<size_t>(ranges[i].second);
            } else {
                coalesced.emplace_back(cur_addr, cur_size);
                cur_addr = ranges[i].first;
                cur_size = static_cast<size_t>(ranges[i].second);
            }
        }
        coalesced.emplace_back(cur_addr, cur_size);
        // Use batch API to reduce lock acquisitions and contention.
        tracker->UpdatePagesCachedBatch(coalesced, delta);
        ranges.clear();
    }

    /// @brief Notify tracker about changes in the CPU tracking state of a word in the buffer
    /// @param add_to_tracker True when the tracker should start tracking the new pages
    /// @param word_index   Index to the word to notify to the tracker
    /// @param current_bits Current state of the word
    /// @param new_bits     New state of the word
    void NotifyRasterizer(bool add_to_tracker, u64 word_index, u64 current_bits, u64 new_bits) const {
        u64 changed_bits = (add_to_tracker ? current_bits : ~current_bits) & new_bits;
        VAddr addr = cpu_addr + word_index * BYTES_PER_WORD;
        IteratePages(changed_bits, [&](size_t offset, size_t size) {
            tracker->UpdatePagesCachedCount(addr + offset * BYTES_PER_PAGE, size * BYTES_PER_PAGE, add_to_tracker ? 1 : -1);
        });
    }

    std::span<u64> Span(Type type) noexcept {
        return std::span<u64>(heap.data() + num_words * size_t(type), num_words);
    }

    std::span<const u64> Span(Type type) const noexcept {
        return std::span<const u64>(heap.data() + num_words * size_t(type), num_words);
    }

    std::array<u64, size_t(Type::Max) * num_words> heap = {};
    DeviceTracker* tracker = nullptr;
    VAddr cpu_addr = 0;
    /// Bit i is set exactly while word i of CPU or Untracked is non-zero (EDEN_NX2_SYNC_SKIP).
    u64 cpu_dirty_words = 0;
};

} // namespace VideoCommon
