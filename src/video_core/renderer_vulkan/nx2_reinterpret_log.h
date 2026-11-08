// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

#include "common/common_types.h"

// "nx2 reinterpret" lines: how often TextureCacheRuntime::ReinterpretImage runs and with which
// aspects, for the device losses on NVIDIA (no VK_EXT_shader_stencil_export, so its colour <->
// depth-stencil copies go through a buffer; one region with DEPTH|STENCIL lost the device before
// 31a7923e3f). Always on and cheap: two relaxed counters per call, a line for each of the first
// DetailedCalls calls, then a summary at most every SummarySeconds when the count moved.
namespace Vulkan::Nx2ReinterpretLog {

/// VK_IMAGE_ASPECT_COLOR_BIT, _DEPTH_BIT, _STENCIL_BIT (vk_texture_cache.cpp checks they match)
inline constexpr u32 AspectColor = 0x1;
inline constexpr u32 AspectDepth = 0x2;
inline constexpr u32 AspectStencil = 0x4;

inline constexpr u64 DetailedCalls = 8;
inline constexpr u64 SummarySeconds = 60;

/// "COLOR", "DEPTH|STENCIL", ...; other bits as hex, nothing as "none".
inline std::string AspectName(u32 mask) {
    std::string name;
    const auto add = [&name](std::string_view part) {
        if (!name.empty()) {
            name += '|';
        }
        name += part;
    };
    if (mask & AspectColor) {
        add("COLOR");
    }
    if (mask & AspectDepth) {
        add("DEPTH");
    }
    if (mask & AspectStencil) {
        add("STENCIL");
    }
    if (const u32 rest = mask & ~(AspectColor | AspectDepth | AspectStencil)) {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string hex;
        for (u32 value = rest; value != 0; value >>= 4) {
            hex.insert(hex.begin(), digits[value & 0xF]);
        }
        add("0x" + hex);
    }
    return name.empty() ? std::string{"none"} : name;
}

/// Both depth and stencil: the copies that named the two in one region before 31a7923e3f.
constexpr bool IsDepthStencil(u32 mask) {
    return (mask & (AspectDepth | AspectStencil)) == (AspectDepth | AspectStencil);
}

struct Summary {
    u64 total;
    u64 depth_stencil; ///< Calls with a DEPTH|STENCIL image on either side
};

class Counter {
public:
    /// One call; `depth_stencil` as IsDepthStencil says of its source or destination. Returns its
    /// number (1 for the first) when its own line is due, 0 otherwise.
    u64 Note(bool depth_stencil) {
        const u64 number = total.fetch_add(1, std::memory_order_relaxed) + 1;
        if (depth_stencil) {
            depth_stencil_total.fetch_add(1, std::memory_order_relaxed);
        }
        return number <= DetailedCalls ? number : 0;
    }

    /// Asked every frame from one thread: the summary to log, at most once per SummarySeconds and
    /// only when calls came since the last one (the first SummarySeconds start at the first ask).
    std::optional<Summary> SummaryDue(u64 now_seconds) {
        if (!started) {
            started = true;
            last_seconds = now_seconds;
            return std::nullopt;
        }
        if (now_seconds - last_seconds < SummarySeconds) {
            return std::nullopt;
        }
        last_seconds = now_seconds;
        const u64 now_total = total.load(std::memory_order_relaxed);
        if (now_total == last_total) {
            return std::nullopt;
        }
        last_total = now_total;
        return Summary{now_total, depth_stencil_total.load(std::memory_order_relaxed)};
    }

private:
    std::atomic<u64> total{};
    std::atomic<u64> depth_stencil_total{};
    bool started{};
    u64 last_seconds{};
    u64 last_total{};
};

} // namespace Vulkan::Nx2ReinterpretLog
