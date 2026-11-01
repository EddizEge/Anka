// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    FixedPipelineState state;

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    size_t Size() const noexcept {
        return sizeof(unique_hashes) + state.Size();
    }
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::GraphicsPipelineCacheKey> {
    size_t operator()(const Vulkan::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace Vulkan {

/// SPIR-V modules of the graphics stages, null for unused stages. Shared by the pipelines whose
/// keys differ only in state the shaders do not see (ShaderReuseCache).
using GraphicsShaderModules =
    std::array<vk::ShaderModule, Tegra::Engines::Maxwell3D::Regs::MaxShaderStage>;

class Device;
class PipelineStatistics;
class RenderPassCache;
class RescalingPushConstant;
class RenderAreaPushConstant;
class Scheduler;

/// EDEN_NX2_LIGHT_DRAW (on unless =0) and EDEN_NX2_LIGHT_UNIFORM (1 unless =0; =2 decides light
/// draws with uniforms but sets them up in full and compares): 0 off, 1 on, 2 verify.
[[nodiscard]] bool LightDrawOn();
[[nodiscard]] u32 LightUniformMode();

class GraphicsPipeline {
    static constexpr size_t NUM_STAGES = Tegra::Engines::Maxwell3D::Regs::MaxShaderStage;

public:
    explicit GraphicsPipeline(
        Scheduler& scheduler, BufferCache& buffer_cache, TextureCache& texture_cache,
        vk::PipelineCache& pipeline_cache, VideoCore::ShaderNotify* shader_notify,
        const Device& device, DescriptorPool& descriptor_pool,
        GuestDescriptorQueue& guest_descriptor_queue,
        DescriptorBufferRing& descriptor_buffer_ring, Common::ThreadWorker* worker_thread,
        PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
        const GraphicsPipelineCacheKey& key,
        std::shared_ptr<const GraphicsShaderModules> modules,
        const std::array<const Shader::Info*, NUM_STAGES>& infos);

    bool HasDynamicVertexInput() const noexcept { return key.state.dynamic_vertex_input; }
    bool SupportsAlphaToCoverage() const noexcept {
        return fragment_has_color0_output;
    }

    bool SupportsAlphaToOne() const noexcept {
        return fragment_has_color0_output;
    }

    bool UsesExtendedDynamicState() const noexcept {
        return key.state.extended_dynamic_state != 0;
    }

    /// Whether a stage writes memory other than its attachments (storage buffers, images,
    /// global memory): EDEN_NX2_LIGHT_DRAW sets up every draw of such a pipeline in full.
    bool WritesMemory() const noexcept {
        return writes_memory;
    }
    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = delete;
    GraphicsPipeline(GraphicsPipeline&&) noexcept = delete;

    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(const GraphicsPipeline&) = delete;

    void AddTransition(GraphicsPipeline* transition);

    bool Configure(bool is_indexed) {
        return configure_func(this, is_indexed);
    }

    /// EDEN_NX2_LIGHT_UNIFORM (RasterizerVulkan::TryLightDraw): what Configure does again for a
    /// draw after one it set up whose registers and memory changed only in vertex/index buffers
    /// and constant buffer data or binds: every buffer binding and upload as Configure makes them,
    /// each stage's image descriptors as it last pushed them (EDEN_NX2_DESC_REUSE), and the
    /// descriptor set; no pipeline, image, render target or push constant work. False, with
    /// nothing recorded, where that does not hold or buffers were uploaded (Configure then
    /// follows and adds the barrier).
    bool ConfigureUniforms(bool is_indexed) {
        return configure_uniforms_func(this, is_indexed);
    }

    /// Constant buffer slots of a stage Configure reads texture handles or storage buffer
    /// descriptors from.
    [[nodiscard]] u32 DescriptorSlots(size_t stage) const noexcept {
        return descriptor_slots[stage];
    }

    /// Whether none of the GPU ranges overlaps what Configure reads from constant buffers
    /// (handles and storage buffer descriptors, at the bound constant buffers' addresses).
    [[nodiscard]] bool DescriptorReadsMiss(
        std::span<const std::pair<GPUVAddr, u32>> written) const;

    /// Whether the images the last Configure sampled or stored to need it again (see
    /// TextureCache::ViewsNeedRepeat); computed with EDEN_NX2_LIGHT_DRAW on.
    [[nodiscard]] bool ViewsNeedRepeat() const noexcept {
        return views_repeat;
    }

    /// EDEN_NX2_LIGHT_UNIFORM=2: payload size, the first entry where two payloads differ
    /// (DescriptorEntryCount if none) and what the last ConfigureDraw changed besides the
    /// descriptors (1 bound the pipeline, 2 rescaling push constants, 4 render area).
    [[nodiscard]] u32 DescriptorEntryCount() const noexcept {
        return num_descriptor_entries;
    }
    [[nodiscard]] u32 FirstPayloadDifference(const DescriptorUpdateEntry* a,
                                             const DescriptorUpdateEntry* b) const;
    [[nodiscard]] u32 LastDrawChanges() const noexcept {
        return last_draw_changes;
    }
    [[nodiscard]] u64 KeyHash() const noexcept {
        return key.Hash();
    }

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key) {
            return this;
        }
        const auto it{std::find(transition_keys.begin(), transition_keys.end(), current_key)};
        return it != transition_keys.end() ? transitions[std::distance(transition_keys.begin(), it)]
                                           : nullptr;
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::relaxed);
    }

    template <typename Spec>
    static auto MakeConfigureSpecFunc() {
        return [](GraphicsPipeline* pl, bool is_indexed) { return pl->ConfigureImpl<Spec>(is_indexed); };
    }

    template <typename Spec>
    static auto MakeConfigureUniformsSpecFunc() {
        return [](GraphicsPipeline* pl, bool is_indexed) {
            return pl->ConfigureUniformsImpl<Spec>(is_indexed);
        };
    }

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

private:
    template <typename Spec>
    bool ConfigureImpl(bool is_indexed);

    template <typename Spec>
    bool ConfigureUniformsImpl(bool is_indexed);

    bool ConfigureDraw(const RescalingPushConstant& rescaling,
                       const RenderAreaPushConstant& render_are);

    /// ConfigureDraw's descriptor steps: the descriptor buffer allocation (reused for the same
    /// payload) and whether a descriptor set needs writing.
    bool PrepareDescriptorBuffer(const DescriptorUpdateEntry* entries, VkDeviceSize& offset,
                                 u32& chunk);
    bool UpdateDescriptorPayload(const DescriptorUpdateEntry* entries, bool bind_pipeline);

    /// ConfigureUniforms: records the descriptors of the payload just built, nothing else.
    bool RecordDescriptors();

    void PushStageImageDescriptors(size_t stage, RescalingPushConstant& rescaling,
                                   const VideoCommon::SamplerId*& samplers,
                                   const VideoCommon::ImageViewInOut*& views);

    void MakePipeline(VkRenderPass render_pass);

    void Validate();

    const GraphicsPipelineCacheKey key;
    Tegra::Engines::Maxwell3D* maxwell3d;
    Tegra::MemoryManager* gpu_memory;
    const Device& device;
    TextureCache& texture_cache;
    BufferCache& buffer_cache;
    vk::PipelineCache& pipeline_cache;
    Scheduler& scheduler;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;

    bool (*configure_func)(GraphicsPipeline*, bool){};
    bool (*configure_uniforms_func)(GraphicsPipeline*, bool){};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;

    std::shared_ptr<const GraphicsShaderModules> spv_modules;

    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::array<u32, 5> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    u32 num_descriptor_entries{};
    size_t num_image_elements{};
    u32 num_textures{};
    bool fragment_has_color0_output{};
    bool writes_memory{};

    /// What Configure reads from constant buffers per stage and slot: texture handles and
    /// storage buffer descriptors, as byte offsets [begin, end) (see DescriptorReadsMiss).
    struct DescriptorRead {
        u32 stage;
        u32 slot;
        u32 begin;
        u32 end;
    };
    std::vector<DescriptorRead> descriptor_reads;
    std::array<u32, NUM_STAGES> descriptor_slots{};
    bool views_repeat{};
    u32 last_draw_changes{};
    std::remove_cvref_t<decltype(std::declval<const RescalingPushConstant&>().Data())>
        last_rescaling{};
    std::array<f32, 4> last_render_area{};
    bool last_uses_render_area{};

    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;

    DescriptorBufferLayout descriptor_buffer_layout;
    std::vector<DescriptorUpdateEntry> last_descriptor_payload;
    VkDeviceSize last_descriptor_buffer_offset{};
    u32 last_descriptor_buffer_chunk{};
    u64 last_descriptor_buffer_generation{};

    /// EDEN_NX2_DESC_REUSE: the image descriptors a stage wrote last and what they were made of.
    struct ImageDescriptorMemo {
        std::vector<u32> ids; ///< Texture and image view ids, then sampler ids
        std::vector<DescriptorUpdateEntry> entries; ///< What PushImageDescriptors wrote
        std::vector<u8> rescaled; ///< Rescale bit of each texture, then image descriptor
        u64 texture_epoch{};
        u64 sampler_generation{};
        bool valid{};
    };
    std::array<ImageDescriptorMemo, NUM_STAGES> image_descriptor_memos;

    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};
};

} // namespace Vulkan
