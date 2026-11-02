// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdlib>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#include "common/common_types.h"

namespace Vulkan {
class GraphicsPipeline;
class RecordList;
} // namespace Vulkan

// Back stages, cut c (Kesim-2): the draw B sends C in ring 3. B does the buffer half of the draw
// (memory, buffer cache, pipeline lookup, its Vulkan commands into a record list), C the texture
// half and the recording (GraphicsPipeline::ConfigureBuffers / ConfigureTextures). Owner T6; T7
// builds and reads it (PrepareDrawB / PrepareDrawC). Free of Vulkan for the unit tests
// (tests/video_core/nx2_stage_draw.cpp). Nothing here is made with the stages off.
namespace VideoCommon::Nx2Stages {

enum class DrawKind : u8 {
    Full,         ///< B: GraphicsPipeline::ConfigureBuffers; C: ConfigureTextures
    LightPlain,   ///< B took the light way (geometry only); C checks it and repairs (§3.7)
    LightUniform, ///< The light way with uniform buffers: B ConfigureUniformsBuffers, C
                  ///< RecordUniformsFrom (or, repairing, ConfigureTextures with LastHandles)
    Joined,       ///< Set up by B with C parked (ConfigureBuffers said Join: texture/image
                  ///< buffers, transform feedback)
};

/// B's light decision, for C to check (a: command state, b: textures) and repair. T7's to fill.
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

/// The graphics stages (Maxwell3D::Regs::MaxShaderStage).
inline constexpr u32 DrawStages = 5;
/// u64 words of one descriptor entry (Vulkan::DescriptorUpdateEntry, asserted where it is known:
/// vk_graphics_pipeline.cpp).
inline constexpr u32 DrawEntryWords = 3;

/// Ring 3 Draw record payload: this head, then its tail (StageDrawWriter has the layout). The
/// counts of the tail are the writer's (WriteTo), so the head and the tail never disagree.
struct DrawToC {
    u64 seq; ///< B's packet seq (BufferCacheRuntime::BeginRecords), never wraps (K5): the tick
             ///< handover (T3: NoteSeqTick, CDoneSeq), the page summary (Nx2GpuPages::PreSet)
    Vulkan::GraphicsPipeline* pipeline;
    Vulkan::RecordList* records; ///< B's Vulkan commands for it, replayed by C first (T3); may
                                 ///< be null (nothing recorded)
    LightFromB light;
    u32 instance_count;
    DrawKind kind;
    bool is_indexed;
    bool indirect;
    bool any_upload; ///< B uploaded buffers for it (its PostCopyBarrier is in `records`)
    std::array<u8, DrawStages> buffer_entries; ///< Per graphics stage, B's descriptor entries
    u8 reserved;
    u16 handles;        ///< u32 texture / sampler pool indices (tail part 2)
    u16 geometry_slots; ///< GeometrySlot (tail part 3)
    u16 draw_words;     ///< u64 draw parameter words (tail part 4)
    u32 vertex_live;    ///< The vertex streams B's buffer cache has enabled after the packet
                        ///< (EnabledVertexBuffersMask): the geometry mirror's live streams (T7)
};
static_assert(std::is_trivially_copyable_v<DrawToC>);
static_assert(std::is_trivially_copyable_v<GeometrySlot>);
static_assert(sizeof(DrawToC) % sizeof(u64) == 0 && alignof(DrawToC) <= alignof(u64));
static_assert(sizeof(DrawToC) == 72, "vertex_live takes the head's tail padding");
static_assert(sizeof(GeometrySlot) % sizeof(u64) == 0 && alignof(GeometrySlot) <= alignof(u64));

/// u64 words of the head and of one GeometrySlot.
inline constexpr u32 DrawHeadWords = static_cast<u32>(sizeof(DrawToC) / sizeof(u64));
inline constexpr u32 GeometrySlotWords = static_cast<u32>(sizeof(GeometrySlot) / sizeof(u64));

/// B: builds one draw's ring 3 payload in B's memory while B holds its cache locks; the payload
/// goes to ring 3 once they are let go (K3):
///     u64* const out = Reserve(Ring::ToC, thunk, writer.Words());
///     writer.WriteTo(out);
///     Commit(Ring::ToC);
/// The tail after the head, each part a whole number of u64 words, in this order:
///   1. descriptor entries, DrawEntryWords words each: per graphics stage in stage order,
///      buffer_entries[stage] of them (B's buffer binds: uniform, storage, texture and image
///      buffers, as BindHostStageBuffers makes them; C adds them before the stage's images)
///   2. handles, u32, two per word (an odd count leaves the high half of the last word zero):
///      texture / sampler pool indices in ConfigureImpl's config_stage order. Per stage, per
///      texture buffer, image buffer, texture and image element its TIC index; a texture's TSC
///      index right after its TIC index
///   3. geometry_slots GeometrySlot (the geometry mirror, T7)
///   4. draw_words u64, the draw parameters (layout T7)
/// Reused draw after draw (Begin): it allocates only to grow.
class StageDrawWriter {
public:
    /// Starts a draw: the head zeroed (kind Full), the tail empty.
    void Begin() noexcept {
        head = {};
        ClearSetup();
        geometry.clear();
        draw.clear();
    }

    /// Empties the parts a pipeline setup writes (descriptor entries, handles), so a setup that
    /// gave up (ConfigureUniformsBuffers) leaves nothing for the one after it.
    void ClearSetup() noexcept {
        entries.clear();
        handles.clear();
        stage_entries = {};
        next_stage = 0;
    }

    /// What T7 fills: seq, records, kind, the draw's flags, light. The counts are set by WriteTo.
    [[nodiscard]] DrawToC& Head() noexcept {
        return head;
    }
    [[nodiscard]] const DrawToC& Head() const noexcept {
        return head;
    }

    /// Stage `stage`'s descriptor entries: `count` of DrawEntryWords words each at `data`.
    /// Stages in increasing order, each at most once.
    void AddStageEntries(u32 stage, const void* data, u32 count) {
        if (stage < next_stage || stage >= DrawStages || count > 0xff) [[unlikely]] {
            std::abort();
        }
        next_stage = stage + 1;
        stage_entries[stage] = static_cast<u8>(count);
        const size_t first = entries.size();
        entries.resize(first + size_t{count} * DrawEntryWords);
        if (count != 0) {
            std::memcpy(entries.data() + first, data, size_t{count} * DrawEntryWords * sizeof(u64));
        }
    }

    void AddHandle(u32 value) {
        handles.push_back(value);
    }

    void AddGeometry(const GeometrySlot& slot) {
        geometry.push_back(slot);
    }

    void AddDrawWords(const u64* data, u32 count) {
        draw.insert(draw.end(), data, data + count);
    }

    [[nodiscard]] u32 EntryCount() const noexcept {
        return static_cast<u32>(entries.size() / DrawEntryWords);
    }
    [[nodiscard]] std::span<const u32> Handles() const noexcept {
        return handles;
    }

    /// The payload's words: the head and the tail.
    [[nodiscard]] u32 Words() const noexcept {
        return DrawHeadWords + static_cast<u32>(entries.size()) +
               static_cast<u32>((handles.size() + 1) / 2) +
               static_cast<u32>(geometry.size()) * GeometrySlotWords +
               static_cast<u32>(draw.size());
    }

    /// Writes the payload (Words() words) to `out`, the head with the tail's counts.
    void WriteTo(u64* out) const {
        if (handles.size() > 0xffff || geometry.size() > 0xffff || draw.size() > 0xffff)
            [[unlikely]] {
            std::abort();
        }
        DrawToC written = head;
        written.buffer_entries = stage_entries;
        written.handles = static_cast<u16>(handles.size());
        written.geometry_slots = static_cast<u16>(geometry.size());
        written.draw_words = static_cast<u16>(draw.size());
        std::memcpy(out, &written, sizeof(written));
        out += DrawHeadWords;
        if (!entries.empty()) {
            std::memcpy(out, entries.data(), entries.size() * sizeof(u64));
            out += entries.size();
        }
        const size_t handle_words = (handles.size() + 1) / 2;
        if (handle_words != 0) {
            out[handle_words - 1] = 0;
            std::memcpy(out, handles.data(), handles.size() * sizeof(u32));
            out += handle_words;
        }
        if (!geometry.empty()) {
            std::memcpy(out, geometry.data(), geometry.size() * sizeof(GeometrySlot));
            out += geometry.size() * GeometrySlotWords;
        }
        if (!draw.empty()) {
            std::memcpy(out, draw.data(), draw.size() * sizeof(u64));
        }
    }

private:
    DrawToC head{};
    std::array<u8, DrawStages> stage_entries{};
    u32 next_stage{};
    std::vector<u64> entries;
    std::vector<u32> handles;
    std::vector<GeometrySlot> geometry;
    std::vector<u64> draw;
};

/// C: a draw's payload as StageDrawWriter wrote it (the ring 3 record's thunk gets the payload
/// and its words). The views point into the payload: valid while the record is.
class StageDrawReader {
public:
    StageDrawReader(const u64* payload, u32 words) {
        if (words < DrawHeadWords) [[unlikely]] {
            std::abort();
        }
        std::memcpy(&head, payload, sizeof(head));
        const u64* it = payload + DrawHeadWords;
        entries = it;
        u32 entry_count = 0;
        for (u32 stage = 0; stage < DrawStages; ++stage) {
            entry_begin[stage] = entry_count;
            entry_count += head.buffer_entries[stage];
        }
        entry_begin[DrawStages] = entry_count;
        it += size_t{entry_count} * DrawEntryWords;
        handles = {reinterpret_cast<const u32*>(it), head.handles};
        it += (size_t{head.handles} + 1) / 2;
        geometry = {reinterpret_cast<const GeometrySlot*>(it), head.geometry_slots};
        it += size_t{head.geometry_slots} * GeometrySlotWords;
        draw = {it, head.draw_words};
        it += head.draw_words;
        if (it != payload + words) [[unlikely]] {
            std::abort();
        }
    }

    [[nodiscard]] const DrawToC& Head() const noexcept {
        return head;
    }

    /// Stage `stage`'s descriptor entries (DrawEntryWords words each) and how many.
    [[nodiscard]] const u64* StageEntries(u32 stage) const noexcept {
        return entries + size_t{entry_begin[stage]} * DrawEntryWords;
    }
    [[nodiscard]] u32 StageEntryCount(u32 stage) const noexcept {
        return entry_begin[stage + 1] - entry_begin[stage];
    }
    [[nodiscard]] u32 EntryCount() const noexcept {
        return entry_begin[DrawStages];
    }

    [[nodiscard]] std::span<const u32> Handles() const noexcept {
        return handles;
    }
    [[nodiscard]] std::span<const GeometrySlot> Geometry() const noexcept {
        return geometry;
    }
    [[nodiscard]] std::span<const u64> DrawWords() const noexcept {
        return draw;
    }

    /// The same draw with other handles: C repairing a light draw with its anchor's
    /// (GraphicsPipeline::LastHandles), the view having to outlive the copy.
    [[nodiscard]] StageDrawReader WithHandles(std::span<const u32> other) const noexcept {
        StageDrawReader copy = *this;
        copy.handles = other;
        return copy;
    }

private:
    DrawToC head{};
    std::array<u32, DrawStages + 1> entry_begin{};
    const u64* entries{};
    std::span<const u32> handles;
    std::span<const GeometrySlot> geometry;
    std::span<const u64> draw;
};

} // namespace VideoCommon::Nx2Stages
