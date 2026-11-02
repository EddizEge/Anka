// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdlib>

#include "common/common_types.h"

// Root table 0 and the Ampere lowering's bank 17 (both off by default):
// - NVN2 fullscreen blits read a parameter block from constant bank 24, which the lowering reads
//   from bank 17 (lower_to_maxwell.h); BlitConstantsPass (position_pass.cpp) replaces every bank 17
//   read of a VertexB or Fragment shader with the blit values the console provides.
// - Bank 24 is root table 0, which a game may load itself through the 3D methods 0x504 (select a
//   table, bits 0-2, and a byte offset, bits 8-15) and 0x508 (the next word); NVN2 keeps push
//   constants there (Duck912 0e0ccd768b, e65cfe9014 in moceure/eden-nx2-android: Fortune's Weave's
//   material constants). A shader that is no blit then reads 0, 2560... instead.
// EDEN_NX2_ROOT_DIAG=1: log the writes to 0x504 / 0x508 (maxwell_3d.cpp) and the shaders reading
// bank 17 outside the blit block or only words the block leaves zero (vk_pipeline_cache.cpp, by
// hash). Changes nothing.
// EDEN_NX2_ROOT_TABLES=1: those shaders keep their bank 17 reads, the 3D engine keeps the eight
// tables and the buffer cache streams table 0 as an unbound bank 17 (buffer_cache.h).
namespace Shader::Optimization::Nx2Root {

inline bool On(const char* name) {
    const char* text = std::getenv(name);
    return text != nullptr && text[0] != '0';
}

inline bool DiagEnabled() {
    static const bool enabled = On("EDEN_NX2_ROOT_DIAG");
    return enabled;
}

inline bool TablesEnabled() {
    static const bool enabled = On("EDEN_NX2_ROOT_TABLES");
    return enabled;
}

/// Words of the blit block: texel size and size (0x0-0x1c), UV clamp rectangle (0x30-0x48 with
/// the dither bias) and source rectangle (0x50-0x5c).
constexpr bool InBlitBlock(u32 offset) {
    return offset < 0x20 || (offset >= 0x30 && offset < 0x4c) || (offset >= 0x50 && offset < 0x60);
}

/// Of those, the words the blit values make nonzero.
constexpr bool NonzeroBlitWord(u32 offset) {
    return offset < 0x20 || offset == 0x38 || offset == 0x3c || offset == 0x58 || offset == 0x5c;
}

/// The bank 17 reads of one VertexB or Fragment program, as BlitConstantsPass found them.
struct Reads {
    u32 immediate{};        ///< reads at an immediate offset (a pair counts once)
    u32 outside{};          ///< of them past the blit block (a pair: either word)
    u32 nonzero{};          ///< of them reading a word the blit values make nonzero
    u32 indirect{};         ///< reads at a register offset (never replaced)
    std::array<u32, 8> offsets{}; ///< the first offsets past the blit block
    u32 offset_count{};

    void Add(u32 offset, bool pair) {
        ++immediate;
        const bool past = !InBlitBlock(offset) || (pair && !InBlitBlock(offset + 4));
        if (past) {
            ++outside;
            if (offset_count < offsets.size()) {
                offsets[offset_count++] = offset;
            }
        }
        if (NonzeroBlitWord(offset) || (pair && NonzeroBlitWord(offset + 4))) {
            ++nonzero;
        }
    }

    /// A program reading bank 17 past the blit block, or only words the blit leaves zero, reads
    /// a root table, not the blit block: EDEN_NX2_ROOT_TABLES keeps its reads.
    bool ReadsTable() const {
        return immediate != 0 && (outside != 0 || nonzero == 0);
    }
};

/// The last program BlitConstantsPass ran on, on this thread (EDEN_NX2_ROOT_DIAG: the pipeline
/// cache logs it right after translating each stage).
inline Reads& LastReads() {
    thread_local Reads reads;
    return reads;
}

} // namespace Shader::Optimization::Nx2Root
