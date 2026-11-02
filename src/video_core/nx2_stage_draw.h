// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <type_traits>

#include "common/common_types.h"

namespace Vulkan {
class GraphicsPipeline;
class RecordList;
} // namespace Vulkan

// Back stages, cut c (Kesim-2): the draw B sends C in ring 3. B does the buffer half of the draw
// (memory, buffer cache, pipeline lookup, its Vulkan commands into a record list), C the texture
// half and the recording. First draft (T0); owner T6, used by T7.
namespace VideoCommon::Nx2Stages {

enum class DrawKind : u8 {
    Full,         ///< Set up in full: buffers by B, textures and the rest by C
    LightPlain,   ///< B took the light way (geometry only); C checks it and repairs (§3.7)
    LightUniform, ///< The light way with uniform buffers
    Joined,       ///< Set up by B with C parked (texture/image buffers, transform feedback)
};

/// B's light decision, for C to check (a: command state, b: textures) and repair.
struct LightFromB {
    u64 anchor_records; ///< B's record list count at the anchor draw
    u64 epoch;          ///< B's copy of the light epoch at the decision (S12)
    u32 reasons;        ///< Why not light (Counter::LightNotArmed... order), 0 when light
    bool armed;         ///< light.armed as B left it
};

/// One changed vertex / index / transform feedback slot (the geometry mirror, §3.6/6).
struct GeometrySlot {
    u8 slot; ///< 0-31 vertex streams, 32 the index buffer, 33-36 transform feedback
    u8 index_format;
    u16 reserved;
    u32 stride;
    u64 buffer; ///< VkBuffer
    u64 offset;
    u64 size;
};

/// Ring 3 Draw record payload. After it, in this order: the descriptor entries B's buffer half
/// made (buffer_entries per stage, from B's scratch queue), `handles` texture/sampler handle
/// values (in config_stage order), `geometry_slots` GeometrySlot, the draw parameters
/// (draw_state, indirect_state). Layout of the tail: T6 with T7.
struct DrawToC {
    u64 seq;            ///< B's draw sequence, never wraps (K5): the tick handover (T3:
                        ///< NoteSeqTick, CDoneSeq), the page summary (Nx2GpuPages::PreSet)
    DrawKind kind;
    bool is_indexed;
    bool indirect;
    bool any_upload;    ///< B uploaded buffers for it
    u32 instance_count;
    Vulkan::GraphicsPipeline* pipeline;
    Vulkan::RecordList* records; ///< B's Vulkan commands for it, replayed by C first (T3)
    std::array<u8, 5> buffer_entries;
    u16 handles;
    u16 geometry_slots;
    LightFromB light;
};
static_assert(std::is_trivially_copyable_v<DrawToC>);
static_assert(std::is_trivially_copyable_v<GeometrySlot>);

} // namespace VideoCommon::Nx2Stages
