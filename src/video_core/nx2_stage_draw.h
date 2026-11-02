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
/// What C does not read stays out of the packet (k2hiz: the record count at the anchor, the
/// reasons and light.armed were written for every draw and never read).
struct LightFromB {
    u64 epoch; ///< B's copy of the light epoch at the decision (S12; =2's race filter)
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
    u32 vertex_live;   ///< The vertex streams B's buffer cache has enabled after the packet
                       ///< (EnabledVertexBuffersMask): the geometry mirror's live streams (T7)
    u16 handles;       ///< u32 texture / sampler pool indices (tail part 2)
    u8 geometry_slots; ///< GeometrySlot (tail part 3): at most 32 streams, the index and 4 TFB
    u8 draw_words;     ///< u64 draw parameter words (tail part 4: PackDrawArgs)
    std::array<u8, DrawStages> buffer_entries; ///< Per graphics stage, B's descriptor entries
    DrawKind kind;
    bool is_indexed;
    u8 reserved;
};
static_assert(std::is_trivially_copyable_v<DrawToC>);
static_assert(std::is_trivially_copyable_v<GeometrySlot>);
static_assert(sizeof(DrawToC) % sizeof(u64) == 0 && alignof(DrawToC) <= alignof(u64));
static_assert(sizeof(DrawToC) == 48, "six words: what C reads");
static_assert(sizeof(GeometrySlot) % sizeof(u64) == 0 && alignof(GeometrySlot) <= alignof(u64));

/// u64 words of the head and of one GeometrySlot.
inline constexpr u32 DrawHeadWords = static_cast<u32>(sizeof(DrawToC) / sizeof(u64));
inline constexpr u32 GeometrySlotWords = static_cast<u32>(sizeof(GeometrySlot) / sizeof(u64));

/// The draw command of a packet (the Vulkan rasterizer's StageCommand).
enum class DrawCommand : u8 {
    None,          ///< Only B's records (a draw cut short: RESETFIX, no pipeline)
    Direct,        ///< vkCmdDraw / vkCmdDrawIndexed
    Indirect,      ///< vkCmdDraw(Indexed)Indirect
    IndirectCount, ///< vkCmdDraw(Indexed)IndirectCount
    ByteCount,     ///< vkCmdDrawIndirectByteCountEXT
};

/// What Draw / DrawIndirect's draw_func read, taken by B from its replica and its buffer cache
/// (the Vulkan rasterizer's StageDrawParams). In the packet packed by its command (PackDrawArgs).
struct DrawArgs {
    u64 indirect_buffer; ///< VkBuffer (kept by B's runtime until C is done: BuryBuffer)
    u64 indirect_offset;
    u64 count_buffer;
    u64 count_offset;
    u32 base_instance;
    u32 num_instances;
    u32 base_vertex;
    u32 num_vertices;
    u32 first_index;
    u32 max_draw_counts;
    u32 stride;
    DrawCommand command;
    bool is_indexed; ///< The command's (quads draw indexed)
    u8 topology;     ///< Maxwell's, for C's replica (UpdateDynamicStates)
    u8 reserved;
};
static_assert(std::is_trivially_copyable_v<DrawArgs>);

/// Most words PackDrawArgs writes (an indirect count draw).
inline constexpr u32 MaxDrawArgWords = 6;

/// The words of `args` a draw of its command reads, to `out` (MaxDrawArgWords room); returns how
/// many. Word 0: command, is_indexed and topology in bits 0-23, then per command
///   Direct:        word 0 bits 32-63 first_index; base_instance, num_instances; base_vertex,
///                  num_vertices (three words; 72 bytes unpacked, k2hiz)
///   Indirect, ByteCount: word 0 bits 32-63 stride; indirect_buffer; indirect_offset;
///                  max_draw_counts (four words)
///   IndirectCount: those, then count_buffer, count_offset (six words)
///   None:          word 0 alone
[[nodiscard]] inline u32 PackDrawArgs(const DrawArgs& args, u64* out) noexcept {
    const u64 flags = u64{static_cast<u8>(args.command)} | (u64{args.is_indexed ? 1u : 0u} << 8) |
                      (u64{args.topology} << 16);
    switch (args.command) {
    case DrawCommand::Direct:
        out[0] = flags | (u64{args.first_index} << 32);
        out[1] = u64{args.base_instance} | (u64{args.num_instances} << 32);
        out[2] = u64{args.base_vertex} | (u64{args.num_vertices} << 32);
        return 3;
    case DrawCommand::Indirect:
    case DrawCommand::ByteCount:
    case DrawCommand::IndirectCount:
        out[0] = flags | (u64{args.stride} << 32);
        out[1] = args.indirect_buffer;
        out[2] = args.indirect_offset;
        out[3] = args.max_draw_counts;
        if (args.command != DrawCommand::IndirectCount) {
            return 4;
        }
        out[4] = args.count_buffer;
        out[5] = args.count_offset;
        return 6;
    case DrawCommand::None:
        break;
    }
    out[0] = flags;
    return 1;
}

/// The arguments PackDrawArgs packed (the fields its command does not read zero). A word count
/// that is not its command's: the process stops (the reader's own check).
[[nodiscard]] inline DrawArgs UnpackDrawArgs(std::span<const u64> words) noexcept {
    if (words.empty()) [[unlikely]] {
        std::abort();
    }
    DrawArgs args{};
    const u64 w0 = words[0];
    args.command = static_cast<DrawCommand>(w0 & 0xff);
    args.is_indexed = ((w0 >> 8) & 1) != 0;
    args.topology = static_cast<u8>(w0 >> 16);
    const auto need = [&](size_t count) {
        if (words.size() != count) [[unlikely]] {
            std::abort();
        }
    };
    switch (args.command) {
    case DrawCommand::Direct:
        need(3);
        args.first_index = static_cast<u32>(w0 >> 32);
        args.base_instance = static_cast<u32>(words[1]);
        args.num_instances = static_cast<u32>(words[1] >> 32);
        args.base_vertex = static_cast<u32>(words[2]);
        args.num_vertices = static_cast<u32>(words[2] >> 32);
        return args;
    case DrawCommand::Indirect:
    case DrawCommand::ByteCount:
    case DrawCommand::IndirectCount:
        need(args.command == DrawCommand::IndirectCount ? 6 : 4);
        args.stride = static_cast<u32>(w0 >> 32);
        args.indirect_buffer = words[1];
        args.indirect_offset = words[2];
        args.max_draw_counts = static_cast<u32>(words[3]);
        if (args.command == DrawCommand::IndirectCount) {
            args.count_buffer = words[4];
            args.count_offset = words[5];
        }
        return args;
    case DrawCommand::None:
        need(1);
        return args;
    }
    std::abort();
}

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
///   3. geometry_slots GeometrySlot (the geometry mirror, T7): added one by one, or the packet's
///      own (SetGeometry: B's runtime's PacketGeometry, copied to the ring by WriteTo alone)
///   4. draw_words u64, the draw parameters (PackDrawArgs)
/// Reused draw after draw (Begin): it allocates only to grow.
class StageDrawWriter {
public:
    /// Starts a draw: the head zeroed (kind Full), the tail empty.
    void Begin() noexcept {
        head = {};
        ClearSetup();
        geometry.clear();
        geometry_view = {};
        draw_count = 0;
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

    /// The geometry part as `slots` (instead of AddGeometry): read by WriteTo, so valid until
    /// then (B's runtime keeps PacketGeometry until its next BeginRecords).
    void SetGeometry(std::span<const GeometrySlot> slots) noexcept {
        geometry_view = slots;
    }

    void AddDrawWords(const u64* data, u32 count) {
        if (draw_count + count > draw.size()) [[unlikely]] {
            std::abort();
        }
        std::memcpy(draw.data() + draw_count, data, size_t{count} * sizeof(u64));
        draw_count += count;
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
               static_cast<u32>(GeometrySlots().size()) * GeometrySlotWords + draw_count;
    }

    /// Writes the payload (Words() words) to `out`, the head with the tail's counts.
    void WriteTo(u64* out) const {
        const std::span<const GeometrySlot> slots = GeometrySlots();
        if (handles.size() > 0xffff || slots.size() > 0xff ||
            (!geometry.empty() && !geometry_view.empty())) [[unlikely]] {
            std::abort();
        }
        DrawToC written = head;
        written.buffer_entries = stage_entries;
        written.handles = static_cast<u16>(handles.size());
        written.geometry_slots = static_cast<u8>(slots.size());
        written.draw_words = static_cast<u8>(draw_count);
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
        if (!slots.empty()) {
            std::memcpy(out, slots.data(), slots.size() * sizeof(GeometrySlot));
            out += slots.size() * GeometrySlotWords;
        }
        if (draw_count != 0) {
            std::memcpy(out, draw.data(), size_t{draw_count} * sizeof(u64));
        }
    }

private:
    [[nodiscard]] std::span<const GeometrySlot> GeometrySlots() const noexcept {
        return geometry_view.empty() ? std::span<const GeometrySlot>{geometry} : geometry_view;
    }

    DrawToC head{};
    std::array<u8, DrawStages> stage_entries{};
    u32 next_stage{};
    std::vector<u64> entries;
    std::vector<u32> handles;
    std::vector<GeometrySlot> geometry;
    std::span<const GeometrySlot> geometry_view;
    std::array<u64, 16> draw{};
    u32 draw_count{};
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
