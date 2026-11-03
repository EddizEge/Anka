// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <iterator>
#include <type_traits>

#include "common/common_types.h"

namespace Vulkan {

/// Whether a shader descriptor has a secondary handle (Shader::TextureDescriptor and
/// Shader::TextureBufferDescriptor). A named concept with a parameter: MSVC 19.44 gives
/// `if constexpr (requires { desc.has_secondary; })` on a generic lambda's local false, so the
/// GraphicsPipeline constructor never listed the secondary handle words.
template <typename Descriptor>
concept HasSecondaryHandle = requires(const Descriptor& desc) { desc.has_secondary; };

/// EDEN_NX2_DESC_SECONDARY: unset, empty or 1 the descriptor words list secondary handle words;
/// 0 they do not (the old behaviour).
[[nodiscard]] constexpr bool ParseDescSecondary(const char* env) noexcept {
    return env == nullptr || env[0] != '0';
}

/// EDEN_NX2_DESC_SECONDARY, read at the first call.
[[nodiscard]] inline bool DescSecondaryOn() {
    static std::atomic<s8> mode{-1};
    s8 value = mode.load(std::memory_order_relaxed);
    if (value < 0) [[unlikely]] {
        value = static_cast<s8>(ParseDescSecondary(std::getenv("EDEN_NX2_DESC_SECONDARY")));
        mode.store(value, std::memory_order_relaxed);
    }
    return value != 0;
}

/// Calls add(cbuf_index, cbuf_offset) for each constant buffer word ConfigureImpl's read_handle
/// reads for these descriptors, in its order: per index the handle, then (with secondary) its
/// secondary handle. Returns the number of secondary handle words the descriptors have (added
/// or not).
template <typename Descriptors, typename Add>
u32 AddHandleWords(const Descriptors& descriptors, bool secondary, Add&& add) {
    using Descriptor = std::remove_cvref_t<decltype(*std::begin(descriptors))>;
    u32 secondary_words = 0;
    for (const Descriptor& desc : descriptors) {
        for (u32 index = 0; index < desc.count; ++index) {
            const u32 index_offset = index << desc.size_shift;
            add(desc.cbuf_index, desc.cbuf_offset + index_offset);
            if constexpr (HasSecondaryHandle<Descriptor>) {
                if (desc.has_secondary) {
                    ++secondary_words;
                    if (secondary) {
                        add(desc.secondary_cbuf_index, desc.secondary_cbuf_offset + index_offset);
                    }
                }
            }
        }
    }
    return secondary_words;
}

} // namespace Vulkan
