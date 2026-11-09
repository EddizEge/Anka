// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>

#include "common/settings.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/nx2_profile.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra::Engines {

void Maxwell3D::DrawManager::ProcessMethodCall(Maxwell3D& maxwell3d, u32 method, u32 argument) {
    switch (method) {
    case MAXWELL3D_REG_INDEX(clear_surface):
        return Clear(maxwell3d, 1);
    case MAXWELL3D_REG_INDEX(draw.begin):
        return DrawBegin(maxwell3d);
    case MAXWELL3D_REG_INDEX(draw.end):
        return DrawEnd(maxwell3d);
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
    case MAXWELL3D_REG_INDEX(index_buffer.first):
        break;
    case MAXWELL3D_REG_INDEX(index_buffer.count):
        draw_state.draw_indexed = true;
        break;
    case MAXWELL3D_REG_INDEX(index_buffer32_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer16_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer8_subsequent):
        draw_state.instance_count++;
        [[fallthrough]];
    case MAXWELL3D_REG_INDEX(index_buffer32_first):
    case MAXWELL3D_REG_INDEX(index_buffer16_first):
    case MAXWELL3D_REG_INDEX(index_buffer8_first):
        return DrawIndexSmall(maxwell3d, argument);
    case MAXWELL3D_REG_INDEX(draw_inline_index):
        SetInlineIndexBuffer(maxwell3d, argument);
        break;
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
        SetInlineIndexBuffer(maxwell3d, maxwell3d.regs.inline_index_2x16.even);
        SetInlineIndexBuffer(maxwell3d, maxwell3d.regs.inline_index_2x16.odd);
        break;
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
        SetInlineIndexBuffer(maxwell3d, maxwell3d.regs.inline_index_4x8.index0);
        SetInlineIndexBuffer(maxwell3d, maxwell3d.regs.inline_index_4x8.index1);
        SetInlineIndexBuffer(maxwell3d, maxwell3d.regs.inline_index_4x8.index2);
        SetInlineIndexBuffer(maxwell3d, maxwell3d.regs.inline_index_4x8.index3);
        break;
    case MAXWELL3D_REG_INDEX(vertex_array_instance_first):
        DrawArrayInstanced(maxwell3d, maxwell3d.regs.vertex_array_instance_first.topology.Value(), maxwell3d.regs.vertex_array_instance_first.start.Value(), maxwell3d.regs.vertex_array_instance_first.count.Value(), false);
        break;
    case MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent): {
        DrawArrayInstanced(maxwell3d, maxwell3d.regs.vertex_array_instance_subsequent.topology.Value(), maxwell3d.regs.vertex_array_instance_subsequent.start.Value(), maxwell3d.regs.vertex_array_instance_subsequent.count.Value(), true);
        break;
    }
    case MAXWELL3D_REG_INDEX(draw_texture.src_y0): {
        DrawTexture(maxwell3d);
        break;
    }
    default:
        break;
    }
}

void Maxwell3D::DrawManager::Clear(Maxwell3D& maxwell3d, u32 layer_count) {
    if (maxwell3d.ShouldExecute()) {
        maxwell3d.rasterizer->Clear(layer_count);
    }
}

void Maxwell3D::DrawManager::DrawDeferred(Maxwell3D& maxwell3d) {
    if (draw_state.draw_mode != DrawMode::Instance || draw_state.instance_count == 0) {
        return;
    }
    DrawEnd(maxwell3d, draw_state.instance_count + 1, true);
    draw_state.instance_count = 0;
}

bool Maxwell3D::DrawManager::IsInstanceLoopMethod(u32 method) noexcept {
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw.begin):
    case MAXWELL3D_REG_INDEX(draw.end):
    case MAXWELL3D_REG_INDEX(vertex_buffer.first):
    case MAXWELL3D_REG_INDEX(vertex_buffer.count):
    case MAXWELL3D_REG_INDEX(index_buffer.first):
    case MAXWELL3D_REG_INDEX(index_buffer.count):
    case MAXWELL3D_REG_INDEX(index_buffer32_first):
    case MAXWELL3D_REG_INDEX(index_buffer16_first):
    case MAXWELL3D_REG_INDEX(index_buffer8_first):
    case MAXWELL3D_REG_INDEX(index_buffer32_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer16_subsequent):
    case MAXWELL3D_REG_INDEX(index_buffer8_subsequent):
    case MAXWELL3D_REG_INDEX(draw_inline_index):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.count):
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.count):
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_first):
    case MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent):
        return true;
    default:
        return false;
    }
}

void Maxwell3D::DrawManager::DrawArray(Maxwell3D& maxwell3d, Maxwell3D::Regs::PrimitiveTopology topology, u32 vertex_first, u32 vertex_count, u32 base_instance, u32 num_instances) {
    draw_state.topology = topology;
    draw_state.vertex_buffer.first = vertex_first;
    draw_state.vertex_buffer.count = vertex_count;
    draw_state.base_instance = base_instance;
    ProcessDraw(maxwell3d, false, num_instances);
}

void Maxwell3D::DrawManager::DrawArrayInstanced(Maxwell3D& maxwell3d, Maxwell3D::Regs::PrimitiveTopology topology, u32 vertex_first, u32 vertex_count, bool subsequent) {
    draw_state.topology = topology;
    draw_state.vertex_buffer.first = vertex_first;
    draw_state.vertex_buffer.count = vertex_count;
    if (!subsequent) {
        draw_state.instance_count = 1;
    }
    draw_state.base_instance = draw_state.instance_count - 1;
    draw_state.draw_mode = DrawMode::Instance;
    draw_state.instance_count++;
    ProcessDraw(maxwell3d, false, 1);
}

void Maxwell3D::DrawManager::DrawIndex(Maxwell3D& maxwell3d, Maxwell3D::Regs::PrimitiveTopology topology, u32 index_first, u32 index_count, u32 base_index, u32 base_instance, u32 num_instances) {
    draw_state.topology = topology;
    draw_state.index_buffer = maxwell3d.regs.index_buffer;
    draw_state.index_buffer.first = index_first;
    draw_state.index_buffer.count = index_count;
    draw_state.base_index = base_index;
    draw_state.base_instance = base_instance;
    ProcessDraw(maxwell3d, true, num_instances);
}

void Maxwell3D::DrawManager::DrawArrayIndirect(Maxwell3D& maxwell3d, Maxwell3D::Regs::PrimitiveTopology topology) {
    draw_state.topology = topology;
    ProcessDrawIndirect(maxwell3d);
}

void Maxwell3D::DrawManager::DrawIndexedIndirect(Maxwell3D& maxwell3d, Maxwell3D::Regs::PrimitiveTopology topology, u32 index_first, u32 index_count) {
    draw_state.topology = topology;
    draw_state.index_buffer = maxwell3d.regs.index_buffer;
    draw_state.index_buffer.first = index_first;
    draw_state.index_buffer.count = index_count;
    ProcessDrawIndirect(maxwell3d);
}

void Maxwell3D::DrawManager::SetInlineIndexBuffer(Maxwell3D& maxwell3d, u32 index) {
    draw_state.inline_index_draw_indexes.push_back(u8(index & 0x000000ff));
    draw_state.inline_index_draw_indexes.push_back(u8((index & 0x0000ff00) >> 8));
    draw_state.inline_index_draw_indexes.push_back(u8((index & 0x00ff0000) >> 16));
    draw_state.inline_index_draw_indexes.push_back(u8((index & 0xff000000) >> 24));
    draw_state.draw_mode = DrawMode::InlineIndex;
}

void Maxwell3D::DrawManager::SetInlineIndexBuffer(Maxwell3D& maxwell3d, u32 method,
                                                  const u32* base_start, u32 amount) {
    auto& index_buffer = draw_state.inline_index_draw_indexes;
    switch (method) {
    case MAXWELL3D_REG_INDEX(draw_inline_index): {
        const auto* const bytes = reinterpret_cast<const u8*>(base_start);
        index_buffer.insert(index_buffer.end(), bytes, bytes + size_t(amount) * sizeof(u32));
        break;
    }
    case MAXWELL3D_REG_INDEX(inline_index_2x16.even): {
        const size_t offset = index_buffer.size();
        index_buffer.resize(offset + size_t(amount) * 2 * sizeof(u32));
        u8* dst = index_buffer.data() + offset;
        for (u32 i = 0; i < amount; ++i) {
            const u32 word = base_start[i];
            const u32 indexes[2]{word & 0xFFFF, word >> 16};
            std::memcpy(dst, indexes, sizeof(indexes));
            dst += sizeof(indexes);
        }
        break;
    }
    case MAXWELL3D_REG_INDEX(inline_index_4x8.index0): {
        const size_t offset = index_buffer.size();
        index_buffer.resize(offset + size_t(amount) * 4 * sizeof(u32));
        u8* dst = index_buffer.data() + offset;
        for (u32 i = 0; i < amount; ++i) {
            const u32 word = base_start[i];
            const u32 indexes[4]{word & 0xFF, (word >> 8) & 0xFF, (word >> 16) & 0xFF,
                                 word >> 24};
            std::memcpy(dst, indexes, sizeof(indexes));
            dst += sizeof(indexes);
        }
        break;
    }
    }
    draw_state.draw_mode = DrawMode::InlineIndex;
}

void Maxwell3D::DrawManager::DrawBegin(Maxwell3D& maxwell3d) {
    auto reset_instance_count = maxwell3d.regs.draw.instance_id == Maxwell3D::Regs::Draw::InstanceId::First;
    auto increment_instance_count = maxwell3d.regs.draw.instance_id == Maxwell3D::Regs::Draw::InstanceId::Subsequent;
    if (reset_instance_count) {
        DrawDeferred(maxwell3d);
        draw_state.instance_count = 0;
        draw_state.draw_mode = DrawMode::General;
    } else if (increment_instance_count) {
        draw_state.instance_count++;
        draw_state.draw_mode = DrawMode::Instance;
    }
    draw_state.topology = maxwell3d.regs.draw.topology;
}

void Maxwell3D::DrawManager::DrawEnd(Maxwell3D& maxwell3d, u32 instance_count, bool force_draw) {
    switch (draw_state.draw_mode) {
    case DrawMode::Instance:
        if (!force_draw) {
            break;
        }
        [[fallthrough]];
    case DrawMode::General:
        draw_state.base_instance = maxwell3d.regs.global_base_instance_index;
        draw_state.base_index = maxwell3d.regs.global_base_vertex_index;
        if (draw_state.draw_indexed) {
            draw_state.index_buffer = maxwell3d.regs.index_buffer;
            ProcessDraw(maxwell3d, true, instance_count);
        } else {
            draw_state.vertex_buffer = maxwell3d.regs.vertex_buffer;
            ProcessDraw(maxwell3d, false, instance_count);
        }
        draw_state.draw_indexed = false;
        break;
    case DrawMode::InlineIndex:
        draw_state.base_instance = maxwell3d.regs.global_base_instance_index;
        draw_state.base_index = maxwell3d.regs.global_base_vertex_index;
        draw_state.index_buffer = maxwell3d.regs.index_buffer;
        draw_state.index_buffer.count = u32(draw_state.inline_index_draw_indexes.size() / 4);
        draw_state.index_buffer.format = Maxwell3D::Regs::IndexFormat::UnsignedInt;
        maxwell3d.dirty.flags[VideoCommon::Dirty::IndexBuffer] = true;
        ProcessDraw(maxwell3d, true, instance_count);
        draw_state.inline_index_draw_indexes.clear();
        break;
    }
}

void Maxwell3D::DrawManager::DrawIndexSmall(Maxwell3D& maxwell3d, u32 argument) {
    Maxwell3D::Regs::IndexBufferSmall index_small_params{argument};
    draw_state.base_instance = maxwell3d.regs.global_base_instance_index;
    draw_state.base_index = maxwell3d.regs.global_base_vertex_index;
    draw_state.index_buffer = maxwell3d.regs.index_buffer;
    draw_state.index_buffer.first = index_small_params.first;
    draw_state.index_buffer.count = index_small_params.count;
    draw_state.topology = index_small_params.topology;
    maxwell3d.dirty.flags[VideoCommon::Dirty::IndexBuffer] = true;
    ProcessDraw(maxwell3d, true, 1);
}

void Maxwell3D::DrawManager::DrawTexture(Maxwell3D& maxwell3d) {
    draw_texture_state.dst_x0 = f32(maxwell3d.regs.draw_texture.dst_x0) / 4096.f;
    draw_texture_state.dst_y0 = f32(maxwell3d.regs.draw_texture.dst_y0) / 4096.f;
    const auto dst_width = f32(maxwell3d.regs.draw_texture.dst_width) / 4096.f;
    const auto dst_height = f32(maxwell3d.regs.draw_texture.dst_height) / 4096.f;
    const bool lower_left{maxwell3d.regs.window_origin.mode != Maxwell3D::Regs::WindowOrigin::Mode::UpperLeft};
    if (lower_left) {
        draw_texture_state.dst_y0 = f32(maxwell3d.regs.surface_clip.height) - draw_texture_state.dst_y0;
    }
    draw_texture_state.dst_x1 = draw_texture_state.dst_x0 + dst_width;
    draw_texture_state.dst_y1 = draw_texture_state.dst_y0 + dst_height;
    draw_texture_state.src_x0 = f32(maxwell3d.regs.draw_texture.src_x0) / 4096.f;
    draw_texture_state.src_y0 = f32(maxwell3d.regs.draw_texture.src_y0) / 4096.f;
    draw_texture_state.src_x1 = (f32(maxwell3d.regs.draw_texture.dx_du) / 4294967296.f) * dst_width + draw_texture_state.src_x0;
    draw_texture_state.src_y1 = (f32(maxwell3d.regs.draw_texture.dy_dv) / 4294967296.f) * dst_height + draw_texture_state.src_y0;
    draw_texture_state.src_sampler = maxwell3d.regs.draw_texture.src_sampler;
    draw_texture_state.src_texture = maxwell3d.regs.draw_texture.src_texture;
    maxwell3d.rasterizer->DrawTexture();
}

void Maxwell3D::DrawManager::UpdateTopology(Maxwell3D& maxwell3d) {
    switch (maxwell3d.regs.primitive_topology_control) {
    case Maxwell3D::Regs::PrimitiveTopologyControl::UseInBeginMethods:
        break;
    case Maxwell3D::Regs::PrimitiveTopologyControl::UseSeparateState:
        switch (maxwell3d.regs.topology_override) {
        case Maxwell3D::Regs::PrimitiveTopologyOverride::None:
            break;
        case Maxwell3D::Regs::PrimitiveTopologyOverride::Points:
            draw_state.topology = Maxwell3D::Regs::PrimitiveTopology::Points;
            break;
        case Maxwell3D::Regs::PrimitiveTopologyOverride::Lines:
            draw_state.topology = Maxwell3D::Regs::PrimitiveTopology::Lines;
            break;
        case Maxwell3D::Regs::PrimitiveTopologyOverride::LineStrip:
            draw_state.topology = Maxwell3D::Regs::PrimitiveTopology::LineStrip;
            break;
        default:
            draw_state.topology = Maxwell3D::Regs::PrimitiveTopology(maxwell3d.regs.topology_override);
            break;
        }
        break;
    }
}

namespace {
// Empty direct draws, all threads together. Logged at the 1st, 2nd, 4th, 8th... empty draw.
struct EmptyDrawCounts {
    std::atomic<u64> draws{};      ///< Direct draws
    std::atomic<u64> no_vertices{}; ///< No vertices or indices
    std::atomic<u64> quads{};      ///< Quads or a quad strip of fewer than 4: no host vertices
    std::atomic<u64> no_instances{}; ///< No instances
    std::atomic<u64> feedback{};   ///< Of the above, drawn anyway: transform feedback was on
    std::atomic<u64> skipped{};    ///< Not handed to the rasterizer (EDEN_NX2_SKIP_EMPTY_DRAWS=1)
};
EmptyDrawCounts g_empty_draws;

bool SkipEmptyDraws() {
    static const bool skip = [] {
        const char* value = std::getenv("EDEN_NX2_SKIP_EMPTY_DRAWS");
        return value != nullptr && value[0] == '1';
    }();
    return skip;
}
} // Anonymous namespace

void Maxwell3D::DrawManager::ProcessDraw(Maxwell3D& maxwell3d, bool draw_indexed, u32 instance_count) {
    LOG_TRACE(HW_GPU, "called, topology={}, count={}", draw_state.topology, draw_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count);
    // The topology first: a separate state override can turn the draw into quads.
    UpdateTopology(maxwell3d);
    // A draw the host would record with no vertices or no instances draws nothing, but the
    // rasterizer still binds its buffers, textures and pipeline and records the draw.
    // EDEN_NX2_SKIP_EMPTY_DRAWS=1 skips it: what it would have applied (dirty state, query
    // counter toggles) stays pending for the next draw or clear. A quad list or strip of fewer
    // than 4 vertices has no host vertices (MakeDrawParams). With transform feedback on the draw
    // still goes through, as in Lichii cebd921fa0. The counts used to be taken before the
    // topology was known and missed the quads.
    // Counted and logged only with the switch or the GPU profile on: off, a draw does what it did
    // before. A plain add, not a locked one: this runs for every draw (from one thread).
    const bool counting = SkipEmptyDraws() || VideoCommon::Nx2Profile::Enabled();
    if (counting) {
        g_empty_draws.draws.store(g_empty_draws.draws.load(std::memory_order_relaxed) + 1,
                                  std::memory_order_relaxed);
    }
    const u32 count = draw_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count;
    const bool quad_topology = draw_state.topology == Maxwell3D::Regs::PrimitiveTopology::Quads ||
                               draw_state.topology == Maxwell3D::Regs::PrimitiveTopology::QuadStrip;
    const bool no_vertices = count == 0;
    const bool short_quads = !no_vertices && quad_topology && count < 4;
    if (counting && (no_vertices || short_quads || instance_count == 0)) [[unlikely]] {
        VideoCommon::Nx2Profile::Add(VideoCommon::Nx2Profile::Counter::EmptyDraws, 1);
        auto& counts = g_empty_draws;
        (no_vertices ? counts.no_vertices : short_quads ? counts.quads : counts.no_instances)
            .fetch_add(1, std::memory_order_relaxed);
        const bool feedback = maxwell3d.regs.transform_feedback_enabled != 0;
        if (feedback) {
            counts.feedback.fetch_add(1, std::memory_order_relaxed);
        }
        const bool skip = !feedback && SkipEmptyDraws();
        if (skip) {
            counts.skipped.fetch_add(1, std::memory_order_relaxed);
        }
        const u64 total = counts.no_vertices.load(std::memory_order_relaxed) +
                          counts.quads.load(std::memory_order_relaxed) +
                          counts.no_instances.load(std::memory_order_relaxed);
        if (std::has_single_bit(total)) {
            LOG_INFO(HW_GPU,
                     "Empty draws: {} of {} direct draws ({} no vertices, {} quads under 4, {} no "
                     "instances; {} with transform feedback), {} skipped (EDEN_NX2_SKIP_EMPTY_DRAWS={})",
                     total, counts.draws.load(std::memory_order_relaxed),
                     counts.no_vertices.load(std::memory_order_relaxed),
                     counts.quads.load(std::memory_order_relaxed),
                     counts.no_instances.load(std::memory_order_relaxed),
                     counts.feedback.load(std::memory_order_relaxed),
                     counts.skipped.load(std::memory_order_relaxed), SkipEmptyDraws() ? 1 : 0);
        }
        if (skip) {
            return;
        }
    }
    if (maxwell3d.ShouldExecute()) {
        maxwell3d.rasterizer->Draw(draw_indexed, instance_count);
    }
}

void Maxwell3D::DrawManager::ProcessDrawIndirect(Maxwell3D& maxwell3d) {
    LOG_TRACE(HW_GPU, "called, topology={}, is_indexed={}, includes_count={}, buffer_size={}, max_draw_count={}", draw_state.topology, indirect_state.is_indexed, indirect_state.include_count, indirect_state.buffer_size, indirect_state.max_draw_counts);
    UpdateTopology(maxwell3d);
    if (maxwell3d.ShouldExecute()) {
        maxwell3d.rasterizer->DrawIndirect();
    }
}
} // namespace Tegra::Engines
