// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

// Back stages, cut c (Kesim-2): B skips constant buffer writes of held bytes (EDEN_NX2_SAME_SKIP)
// only where no cache holds GPU-written data (HoldsGpuWrittenData), and the texture and query
// caches are C's. A lock-free summary per device page (4 KiB) of where C may hold GPU-written
// data (images, pending query results, writes around the caches) lets B answer without C's lock.
// A page holds a sequence number, not a bit (S12): B sets pages for the draws and queries it
// sends C before it sends them, conservatively (D6); C clears a page only when the sequence it
// ran up to is at least the page's, so an older image's clean-up never clears what B set for a
// later draw. Owner: T8. Skeleton (T0): every page may hold data.
namespace VideoCommon::Nx2Stages {

class Nx2GpuPages {
public:
    /// B, before sending draw or query `seq` to C: the render targets, depth, storage images
    /// and query results it may write.
    void PreSet(DAddr begin, u64 size, u32 seq) {}
    /// C: pages it holds GPU-written data for, as of its `seq` (an image marked GPU-modified, a
    /// pending query, a write around the caches).
    void Set(DAddr begin, u64 size, u32 seq) {}
    /// C: pages that no longer hold any, as of its `seq`; a page set for a later seq stays.
    void Clear(DAddr begin, u64 size, u32 seq) {}
    /// B, lock-free: whether a page of [begin, begin + size) may hold C's GPU-written data.
    [[nodiscard]] bool Holds(DAddr begin, u64 size) const {
        return true;
    }
};

} // namespace VideoCommon::Nx2Stages
