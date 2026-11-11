// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"

namespace VideoCommon {

namespace Detail {
/// UsageTracker generations, unique over every tracker of the process (0 is none).
inline std::atomic<u64> g_usage_generation{1};
} // namespace Detail

class UsageTracker {
    // PAGE_SHIFT is a macro on FreeBSD
    static constexpr size_t BUFFER_BYTES_PER_BITSHIFT = 6;
    static constexpr size_t BUFFER_PAGE_SHIFT = 6 + BUFFER_BYTES_PER_BITSHIFT;
    static constexpr size_t BUFFER_PAGE_BYTES = 1 << BUFFER_PAGE_SHIFT;
public:
    explicit UsageTracker(size_t size) : generation{NextGeneration()} {
        const size_t num_pages = (size >> BUFFER_PAGE_SHIFT) + 1;
        pages.resize(num_pages, 0ULL);
    }

    void Reset() noexcept {
        // EDEN_NX2_USAGE_RESET_SKIP (Selevet d8c95fef47): every free buffer is reset each frame
        // (a word per 4 KiB); one nothing was tracked in since the last reset is already clear.
        if (ResetSkipEnabled() && !tracked) {
            return;
        }
        tracked = false;
        std::ranges::fill(pages, 0ULL);
        recent.fill({~u64{0}, 0});
        full_begin = 0;
        full_end = 0;
        generation = NextGeneration();
    }

    /// What tracked ranges stay valid for (UsageMarkMemo): a value of this tracker alone, new at
    /// each Reset. Between two Resets a range tracked once is tracked: Track only adds bits. A
    /// Reset EDEN_NX2_USAGE_RESET_SKIP skips keeps it: nothing was tracked in it, so no memo
    /// holds it (a memo notes a generation after its Track).
    [[nodiscard]] u64 Generation() const noexcept {
        return generation;
    }

    void Track(u64 offset, u64 size) noexcept {
        // Tracking is idempotent until Reset: skip ranges tracked just before. Draws bind the
        // same (often multi-megabyte) storage buffers again and again, and each Track wrote a
        // word per 4 KiB of the range.
        for (const auto& [recent_offset, recent_size] : recent) {
            if (recent_offset == offset && recent_size == size) {
                return;
            }
        }
        tracked = true;
        recent[recent_cursor++ % recent.size()] = {offset, size};
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return;
        }
        TrackPage(page, offset, size);
        if (page == page_end) {
            return;
        }
        FillPages(page + 1, page_end);
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        TrackPage(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

    [[nodiscard]] bool IsUsed(u64 offset, u64 size) const noexcept {
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return false;
        }
        if (IsPageUsed(page, offset, size)) {
            return true;
        }
        if (page == page_end) {
            return false;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            if (pages[i] != 0) {
                return true;
            }
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        return IsPageUsed(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

private:
    static u64 NextGeneration() noexcept {
        return Detail::g_usage_generation.fetch_add(1, std::memory_order_relaxed);
    }

    /// EDEN_NX2_USAGE_RESET_SKIP=0 clears the pages at every Reset, as before.
    static bool ResetSkipEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_USAGE_RESET_SKIP");
            return value == nullptr || value[0] != '0';
        }();
        return enabled;
    }

    /// EDEN_NX2_USAGE_SKIP=0 writes every whole page of a tracked range, as before.
    static bool FullPagesSkipEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("EDEN_NX2_USAGE_SKIP");
            return value == nullptr || value[0] != '0';
        }();
        return enabled;
    }

    /// Sets pages [begin, end) to all ones. Indexed draws track their index buffer from its
    /// start to first + count indices, a range that grows with 'first' when meshes share one
    /// index buffer (each draw wrote a word per 4 KiB of it; ~0.65% of the GPU thread for
    /// direct draws on title 1), and vertex streams track up to their limit. Page bits are only
    /// ever added until Reset, so the pages a previous call set to all ones still are: this
    /// skips the run of them it remembers ([full_begin, full_end)) and writes the rest. The
    /// pages end up the same either way.
    void FillPages(size_t begin, size_t end) noexcept {
        if (begin >= end) {
            return;
        }
        if (!FullPagesSkipEnabled()) {
            std::fill(pages.data() + begin, pages.data() + end, ~u64{0});
            return;
        }
        if (end < full_begin || begin > full_end) {
            // Apart from the remembered run: keep the longer one.
            std::fill(pages.data() + begin, pages.data() + end, ~u64{0});
            if (end - begin > full_end - full_begin) {
                full_begin = begin;
                full_end = end;
            }
            return;
        }
        // Overlapping or touching: write the parts outside the run and join them to it.
        if (begin < full_begin) {
            std::fill(pages.data() + begin, pages.data() + full_begin, ~u64{0});
        }
        if (end > full_end) {
            std::fill(pages.data() + full_end, pages.data() + end, ~u64{0});
        }
        full_begin = (std::min)(begin, full_begin);
        full_end = (std::max)(end, full_end);
    }

    // One bit per 64 bytes: set every bit that the bytes [offset, offset + size) touch within
    // the page, including partly covered units at both ends.
    static u64 PageMask(u64 offset, u64 size) noexcept {
        const size_t offset_in_page = offset % BUFFER_PAGE_BYTES;
        const size_t end_in_page = std::min<size_t>(
            offset_in_page + std::min<u64>(size, BUFFER_PAGE_BYTES), BUFFER_PAGE_BYTES);
        if (end_in_page <= offset_in_page) {
            return 0;
        }
        const size_t first_bit = offset_in_page >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t last_bit = (end_in_page - 1) >> BUFFER_BYTES_PER_BITSHIFT;
        return (~u64{0} >> (63 - (last_bit - first_bit))) << first_bit;
    }

    void TrackPage(u64 page, u64 offset, u64 size) noexcept {
        pages[page] |= PageMask(offset, size);
    }

    bool IsPageUsed(u64 page, u64 offset, u64 size) const noexcept {
        return (pages[page] & PageMask(offset, size)) != 0;
    }

private:
    std::vector<u64> pages;
    /// Ranges tracked last, (offset, size).
    std::array<std::pair<u64, u64>, 4> recent{{{~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}}};
    size_t recent_cursor{};
    /// Pages [full_begin, full_end) are all ones (FillPages); empty when equal.
    size_t full_begin{};
    size_t full_end{};
    /// Whether Track ran since the last Reset (EDEN_NX2_USAGE_RESET_SKIP): else the pages, recent
    /// and the full run are as Reset leaves them.
    bool tracked{};
    u64 generation{};
};

/// Back stages, cut c (BufferCache::MarkVertexStreamsUsed): what one vertex stream tracked last,
/// its buffer's slot and that buffer's tracker generation then. The same range of the same
/// buffer in the same generation is tracked already (Track adds bits until Reset, which starts a
/// new generation; a buffer made in a reused slot has a new one too): only its use stamp is
/// left to renew. Every packet of B's marks every stream (geo-markusage-1): ~12 Tracks a draw,
/// most of them over ranges tracked by the draw before.
struct UsageMarkMemo {
    u32 buffer = ~u32{0};
    u64 generation = 0;
    u64 offset = 0;
    u64 size = 0;

    [[nodiscard]] bool Holds(u32 buffer_, u64 generation_, u64 offset_, u64 size_) const noexcept {
        return generation == generation_ && buffer == buffer_ && offset == offset_ &&
               size == size_;
    }
    void Note(u32 buffer_, u64 generation_, u64 offset_, u64 size_) noexcept {
        buffer = buffer_;
        generation = generation_;
        offset = offset_;
        size = size_;
    }
};

} // namespace VideoCommon
