// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

// Back stages, cut c (Kesim-2): B skips constant buffer writes of held bytes (EDEN_NX2_SAME_SKIP)
// only where no cache holds GPU-written data (HoldsGpuWrittenData), and the texture and query
// caches are C's. A lock-free bit per device page (4 KiB), clearable, of where C holds
// GPU-written data lets B answer without C's lock. B sets the pages of the render targets, depth
// and storage images of the draws it sends C before sending them (D6); only C clears a bit.
// Owner: T8. Skeleton (T0): every page may hold data.
namespace VideoCommon::Nx2Stages {

class Nx2GpuPages {
public:
    /// B, before sending a draw to C: the render targets, depth and storage images it may write.
    void PreSet(DAddr begin, u64 size) {}
    /// C: pages it holds GPU-written data for (an image marked GPU-modified, a pending query).
    void Set(DAddr begin, u64 size) {}
    /// C: pages that no longer hold any.
    void Clear(DAddr begin, u64 size) {}
    /// B, lock-free: whether a page of [begin, begin + size) may hold C's GPU-written data.
    [[nodiscard]] bool Holds(DAddr begin, u64 size) const {
        return true;
    }
};

} // namespace VideoCommon::Nx2Stages
