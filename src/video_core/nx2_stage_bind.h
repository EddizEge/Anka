// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCommon::Nx2Stages {

/// Control::Scheduler's BindChannel while the back stages run (front thread; T5). GPU::BindChannel
/// does nothing for the channel bound already, so a bind of the channel the front bound last in
/// this split session, staged, on the same address space goes to BC in order (Owner; S9/1). Any
/// other joins on A, staging the channel first (S3): the first bind of a session (a channel
/// InstallProxy missed, or one InitChannel bound to the real rasterizer after it), a channel
/// change, an address space bound since, a channel not staged.
class StagedBind {
public:
    /// Whether this bind joins. Either way it is the last one from here on.
    bool Join(u64 session_, s32 bind_id, const void* memory_, bool staged) {
        const bool same = session == session_ && bind == bind_id && memory == memory_;
        session = session_;
        bind = bind_id;
        memory = memory_;
        return !same || !staged;
    }

private:
    u64 session{~u64{}};
    s32 bind{-1};
    const void* memory{};
};

} // namespace VideoCommon::Nx2Stages
