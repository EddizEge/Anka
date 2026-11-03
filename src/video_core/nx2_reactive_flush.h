// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <optional>

#include "common/common_types.h"
#include "common/nx2_reactive.h"
#include "video_core/rasterizer_download_area.h"

/// Experimental reactive flushing switches (off by default; they matter with use_reactive_flushing
/// on, where guest CPU reads of GPU-cached pages reach RasterizerVulkan::GetFlushArea).
namespace VideoCommon::Nx2Reactive {

/// EDEN_NX2_FLUSH_FIRST_READ=1: a guest CPU read that notes a buffer page (NoteCpuRead) the GPU
/// wrote (IsRegionGpuModified) flushes it once, synchronously (OnCPURead's FlushRegion). Without
/// it buffer reads are always preemptive: data the GPU wrote before the page was noted reaches the
/// guest only if the buffer is bound as written again and a fence copies it back, never with the
/// GPU idle (the first "clear, GPU writes, poll" round is lost). Later reads are left to the
/// fence copy back, as before.
[[nodiscard]] inline bool FlushFirstRead() noexcept {
    static const bool enabled =
        Common::Nx2Reactive::ParseFlag(Common::Nx2Reactive::GetEnv("EDEN_NX2_FLUSH_FIRST_READ"));
    return enabled;
}

/// Whether a GetFlushArea flushes its buffer pages (FlushFirstRead).
[[nodiscard]] constexpr bool FirstReadFlushes(bool enabled, bool newly_noted,
                                              bool gpu_modified) noexcept {
    return enabled && newly_noted && gpu_modified;
}

/// GetFlushArea's answer from the texture cache's (if it has GPU-modified images there) and the
/// read's buffer pages [page_start, page_end). Without a buffer flush it is the texture cache's,
/// else those pages, preemptive (as without FlushFirstRead); with one it covers both and is not
/// preemptive.
[[nodiscard]] inline VideoCore::RasterizerDownloadArea CombineFlushArea(
    const std::optional<VideoCore::RasterizerDownloadArea>& texture_area, DAddr page_start,
    DAddr page_end, bool flush_buffer) noexcept {
    if (texture_area) {
        VideoCore::RasterizerDownloadArea area = *texture_area;
        if (flush_buffer) {
            area.start_address = (std::min)(area.start_address, page_start);
            area.end_address = (std::max)(area.end_address, page_end);
            area.preemtive = false;
        }
        return area;
    }
    return VideoCore::RasterizerDownloadArea{
        .start_address = page_start,
        .end_address = page_end,
        .preemtive = !flush_buffer,
    };
}

} // namespace VideoCommon::Nx2Reactive
