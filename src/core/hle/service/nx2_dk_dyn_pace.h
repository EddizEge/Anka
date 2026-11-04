// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"

// EDEN_NX2_DK_DYN on the vi2 side (nx2_gsv.h), once Bananza's dynamic frame rate patch went into
// its main module (core/loader/nx2_dk_dynamic_fps.h, Core::g_nx2_dk_dyn_applied) and Bananza is
// the application. Only then: told VRR works the patched game times its own frames, while the
// unpatched game on its VRR path kept its rate at 60 and ran at ~0.73x (E-2).
namespace Service::Nx2DkDynPace {

constexpr u64 ProgramId = 0x040074A01BF12000ULL;

/// vi2 107's reply with VRR supported: bit 63 of its first u64, where the SDK reads it (E-2's
/// "107: 0x0 0x80000000"). ZendNevs patched the SDK's QueryVrrSupported instead.
constexpr std::array<u32, 2> VrrReply{0x0, 0x80000000};

constexpr bool ReportVrr(bool applied, u64 application) {
    return applied && application == ProgramId;
}

} // namespace Service::Nx2DkDynPace
