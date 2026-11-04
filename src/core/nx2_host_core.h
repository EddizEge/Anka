// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

#include "core/hardware_properties.h"

namespace Core {

/// The slot of the memory's per core rasterizer state (read and write areas, scratch buffers) a
/// host thread uses. Threads that are not guest cores (services, audio; their host thread id is
/// past the cores, up to 255) share the last core's slot, which the caller guards with a lock.
[[nodiscard]] constexpr std::size_t HostCoreSlot(std::size_t host_thread_id) noexcept {
    constexpr std::size_t sys_core = Hardware::NUM_CPU_CORES - 1;
    return host_thread_id < sys_core ? host_thread_id : sys_core;
}

} // namespace Core
