// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/thread_worker.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/nx2_stage_draw.h"
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
    /// follows and adds the barrier). rebind_storage (EDEN_NX2_LIGHT_STORAGE): storage buffer
    /// descriptors in constant buffers changed; each stage's storage buffers are bound again from
    /// them as Configure binds them, and the words read kept (descriptor_values).
    bool ConfigureUniforms(bool is_indexed, bool rebind_storage) {
        return configure_uniforms_func(this, is_indexed, rebind_storage);
    }

    /// Kinds of the constant buffer words Configure reads descriptors from (DescriptorWord).
    static constexpr u32 HandleWords = 1;  ///< Texture handles (read_handle)
    static constexpr u32 StorageWords = 2; ///< Storage buffer descriptors (address and size)

    /// What CheckDescriptorWords found.
    struct LightWords {
        u32 rewritten{};        ///< Kinds of words the ranges overlap or in slots bound elsewhere
        u32 changed{};          ///< Kinds of those whose value differs (all, not compared)
        u32 rebound_changed{};  ///< Kinds of those that differ in a slot bound elsewhere
        // With scan_storage and the profile on, of the storage buffer descriptors that differ:
        u32 storage_buffers{};  ///< How many
        bool storage_address{}; ///< An address word among them (else size words only)
        bool storage_written{}; ///< One the pipeline writes through
        u32 storage_first{};    ///< The first: stage << 24 | constant buffer slot << 16 | offset
    };

    /// EDEN_NX2_LIGHT_UNIFORM: reads again the descriptor words that the GPU ranges overlap or
    /// that lie in a constant buffer slot bound elsewhere since (rebound: slot bits per stage)
    /// and compares them with what the last Configure read (with compare off, or no values,
    /// such a word counts as changed). Stops at the first word that changed; with scan_storage
    /// (EDEN_NX2_LIGHT_STORAGE, or profiling) storage buffer descriptor words that changed do
    /// not stop it, and what they were is noted (profiling).
    [[nodiscard]] LightWords CheckDescriptorWords(std::span<const std::pair<GPUVAddr, u32>> written,
                                                  std::span<const u32> rebound, bool compare,
                                                  bool scan_storage) const;

    /// The descriptor words' values the bound descriptors were made of (descriptor_words'
    /// order): what the last Configure read, with storage buffer descriptors ConfigureUniforms
    /// bound again since. EDEN_NX2_LIGHT_UNIFORM=2 compares them with what a full setup reads.
    [[nodiscard]] std::span<const u32> DescriptorValues() const noexcept {
        return descriptor_values;
    }

    /// Texture header and sampler pool indices the last Configure used (EDEN_NX2_LIGHT_UNIFORM).
    [[nodiscard]] std::span<const u32> UsedTextureHeaders() const noexcept {
        return used_tics;
    }
    [[nodiscard]] std::span<const u32> UsedSamplers() const noexcept {
        return used_tscs;
    }

    /// Whether the images the last Configure sampled or stored to need it again (see
    /// TextureCache::ViewsNeedRepeat); computed with EDEN_NX2_LIGHT_DRAW on.
    [[nodiscard]] bool ViewsNeedRepeat() const noexcept {
        return views_repeat;
    }

    /// EDEN_NX2_LIGHT_UNIFORM=2: payload size, the first entry from start on where two payloads
    /// differ (DescriptorEntryCount if none; uniform: whether it holds a uniform buffer) and what
    /// the last ConfigureDraw changed besides the descriptors (1 bound the pipeline, 2 rescaling
    /// push constants, 4 render area).
    [[nodiscard]] u32 DescriptorEntryCount() const noexcept {
        return num_descriptor_entries;
    }
    [[nodiscard]] u32 FirstPayloadDifference(const DescriptorUpdateEntry* a,
                                             const DescriptorUpdateEntry* b, u32 start,
                                             bool& uniform) const;
    [[nodiscard]] bool UsesDescriptorBuffer() const noexcept {
        return uses_descriptor_buffer;
    }
    [[nodiscard]] u32 LastDrawChanges() const noexcept {
        return last_draw_changes;
    }
    [[nodiscard]] u64 KeyHash() const noexcept {
        return key.Hash();
    }
    [[nodiscard]] const GraphicsPipelineCacheKey& Key() const noexcept {
        return key;
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
        return [](GraphicsPipeline* pl, bool is_indexed, bool rebind_storage) {
            return pl->ConfigureUniformsImpl<Spec>(is_indexed, rebind_storage);
        };
    }

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

    // Back stages, cut c (Kesim-2, EDEN_NX2_BACK_STAGES with 'c'; PLAN.md §4.3 T6, tasarim
    // §3.5): Configure in two halves on two threads. B (the memory stage) does the buffer half:
    // uniform buffer state, storage buffer binds, reading the texture handles, the buffer
    // updates and binds (its Vulkan commands into its record list, its descriptor entries into
    // its scratch queue: T3) and the copy barrier; it writes what C needs into the draw's ring 3
    // packet (nx2_stage_draw.h). C (the record stage) does the texture half from the packet:
    // descriptor tables, samplers and image views from B's handle values, the descriptor payload
    // (per stage B's buffer entries, then the images, the bytes in ConfigureImpl's order), render
    // targets and the draw's recording (ConfigureDraw). ConfigureImpl and ConfigureUniformsImpl
    // stay as they are: with the stages off nothing below is called.
    //
    // Which half owns which member (R11). B writes its members only in the B functions (and
    // SetEngine, CheckDescriptorWords reading), C its own only in the C functions; the old ones
    // (Configure, ConfigureUniforms) write both and run only with the stages off, or in a join
    // (DrawKind::Joined, =2's double setup) with the other stage parked.
    //   | Member                                                   | Owner                     |
    //   |----------------------------------------------------------|---------------------------|
    //   | key, device, the caches' and queues' references,         | read-only after the       |
    //   | stage_infos, enabled_uniform_buffer_masks,               | constructor: any stage    |
    //   | uniform_buffer_sizes, num_*, writes_memory,              |                           |
    //   | fragment_has_color0_output, descriptor_words,            |                           |
    //   | storage_word_begin, layouts, template, uses_*,           |                           |
    //   | configure funcs, split_funcs, spv_modules                |                           |
    //   | transition_keys, transitions (AddTransition, Next)       | B (pipeline cache lookup) |
    //   | maxwell3d, gpu_memory (SetEngine: B's replica)           | B                         |
    //   | descriptor_values, descriptor_values_valid, used_tics,   | B (read by B's light      |
    //   | used_tscs                                                | decision)                 |
    //   | descriptor_word_runs (EDEN_NX2_DESC_HULL; built by its   | B                         |
    //   | first CheckDescriptorWords)                              |                           |
    //   | views_repeat, last_draw_changes, last_rescaling,         | C                         |
    //   | last_render_area, last_uses_render_area                  |                           |
    //   | image_descriptor_memos (EDEN_NX2_DESC_REUSE)             | C                         |
    //   | last_descriptor_payload, last_descriptor_buffer_*,       | C                         |
    //   | descriptor_allocator (Commit on the Vulkan worker)       |                           |
    //   | last_handles                                             | C                         |
    //   | pipeline, is_built, build_mutex, build_condvar           | the build worker, then C  |
    //   |                                                          | (IsBuilt: any stage)      |
    // The caller holds the caches' locks as PrepareDraw does: B the buffer cache's (through
    // Nx2Record::CacheLock, K3), C the texture cache's; neither half takes the other's.

    enum class BufferHalf : u8 {
        Ready, ///< Set up; the writer holds the packet's descriptor entries and handles
        Join,  ///< The draw needs the old setup with C parked (DrawKind::Joined): a stage has
               ///< texture or image buffers (their binds read C's image views) or transform
               ///< feedback is on. Nothing was done.
    };

    /// B: whether ConfigureBuffers would say Join (from the members SetEngine set).
    [[nodiscard]] bool SplitJoins() const;

    /// B, inside a packet (BufferCacheRuntime::BeginRecords), after SetEngine with B's replica:
    /// ConfigureImpl's buffer half. Restarts the runtime's descriptor scratch queue in this
    /// pipeline's descriptor form, then as ConfigureImpl: uniform buffer state, per stage the
    /// storage buffer binds and the handle reads (to `out`, values kept for the light way as
    /// ConfigureImpl keeps them), the texture buffer unbinds, UpdateGraphicsBuffers,
    /// BindHostGeometryBuffers, per stage BindHostStageBuffers (its entries to `out`) and, after
    /// an upload, PostCopyBarrier (into the record list). Sets out's kind (Full), pipeline and
    /// is_indexed and its descriptor entries and handles (ClearSetup first); the rest is T7's.
    [[nodiscard]] BufferHalf ConfigureBuffers(bool is_indexed,
                                              VideoCommon::Nx2Stages::StageDrawWriter& out);

    /// C, after replaying the packet's record list: ConfigureImpl's texture half with B's
    /// handles and descriptor entries from `in`, `maxwell3d_c` being C's replica (the render
    /// area's registers). As ConfigureImpl: SynchronizeDescriptors, the samplers and image views
    /// of the handle values, FillImageViews, Acquire, per stage B's entries then the image
    /// descriptors, UpdateRenderTargets, CheckFeedbackLoop, ViewsNeedRepeat and ConfigureDraw.
    /// False as Configure's: no draw.
    [[nodiscard]] bool ConfigureTextures(const VideoCommon::Nx2Stages::StageDrawReader& in,
                                         Tegra::Engines::Maxwell3D& maxwell3d_c);

    /// B, inside a packet, the light way with uniforms (the anchor's SetEngine holds):
    /// ConfigureUniformsImpl's buffer half. As it: uniform buffer state, with rebind_storage the
    /// storage buffers bound again (descriptor_values kept), UpdateGraphicsBuffers,
    /// BindHostGeometryBuffers, per stage BindHostStageBuffers (its entries to `out`, ClearSetup
    /// first). Sets out's kind (LightUniform), pipeline and is_indexed. False after an upload,
    /// as ConfigureUniformsImpl (the full setup follows: ConfigureBuffers, which records the
    /// barrier). It does not check the image descriptor memos: they are C's (RecordUniformsFrom).
    [[nodiscard]] bool ConfigureUniformsBuffers(bool is_indexed, bool rebind_storage,
                                                VideoCommon::Nx2Stages::StageDrawWriter& out);

    /// C, after replaying the packet's record list: ConfigureUniformsImpl's texture half. False,
    /// with nothing recorded, where each stage's image descriptor memo does not hold or the
    /// render pass is not active (as ConfigureUniformsImpl): C then repairs (T7), e.g. with
    /// ConfigureTextures(in.WithHandles(LastHandles()), ...): B's light decision says the
    /// handles are the anchor's. Else as it: Acquire, per stage B's entries then the memo's,
    /// written storage images marked modified, RecordDescriptors.
    [[nodiscard]] bool RecordUniformsFrom(const VideoCommon::Nx2Stages::StageDrawReader& in);

    /// C: the handles of the last ConfigureTextures of this pipeline (kept with
    /// EDEN_NX2_LIGHT_UNIFORM on), for a light draw's repair.
    [[nodiscard]] std::span<const u32> LastHandles() const noexcept {
        return last_handles;
    }

    /// EDEN_NX2_BACK_STAGES=2, the double setup (T7): C sets a draw up the split way, takes a
    /// snapshot (with the packet's handles), sets it up again the old way (Configure, with A and
    /// B parked at the lockstep point and no SetEngine: maxwell3d and gpu_memory are B's, its
    /// replica equal to C's there), takes another (no handles: used_tics / used_tscs) and
    /// compares them (CompareSetups). Take each right after its setup: the next Acquire reuses
    /// the payload.
    struct SetupSnapshot {
        std::vector<DescriptorUpdateEntry> payload; ///< The descriptor payload (all entries)
        std::vector<u32> tics;   ///< Image view pool indices, in the views' order
        std::vector<u32> tscs;   ///< Sampler pool indices of the textures, in order
        std::vector<u32> values; ///< descriptor_values, when valid
        std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> rescaling{};
        std::array<f32, 4> render_area{};
        bool uses_render_area{};
        bool draw_state{};   ///< rescaling and render area kept (EDEN_NX2_LIGHT_UNIFORM=2)
        bool values_valid{};
        bool views_repeat{};
    };
    /// `handles`: the split setup's (the packet's); empty for the old one (its used_tics and
    /// used_tscs, kept with EDEN_NX2_LIGHT_UNIFORM on).
    void TakeSetupSnapshot(SetupSnapshot& out, std::span<const u32> handles) const;

    /// A uniform buffer entry's mapped bytes and range (RasterizerVulkan: the scheduler's staging
    /// pool or B's, StageStagingBufferPool::MappedBytes); empty if unknown.
    using MappedBytesFunc =
        std::function<std::span<const u8>(const DescriptorUpdateEntry& entry, VkDeviceSize& range)>;
    /// The differences of two setups of this pipeline, field by field, empty when none: the
    /// descriptor payload (a uniform buffer streamed to another slice counts as the same when
    /// `bytes` gives the same bytes, as CheckLightVerify), the handles, the descriptor words
    /// read, the push constants (when both kept them) and ViewsNeedRepeat.
    [[nodiscard]] std::string CompareSetups(const SetupSnapshot& split, const SetupSnapshot& full,
                                            const MappedBytesFunc& bytes) const;

    /// Cut c's halves of this pipeline's Spec, picked with configure_func (ConfigureFunc).
    struct SplitFuncs {
        BufferHalf (*buffers)(GraphicsPipeline*, bool, VideoCommon::Nx2Stages::StageDrawWriter&){};
        bool (*textures)(GraphicsPipeline*, const VideoCommon::Nx2Stages::StageDrawReader&,
                         Tegra::Engines::Maxwell3D&){};
        bool (*uniform_buffers)(GraphicsPipeline*, bool, bool,
                                VideoCommon::Nx2Stages::StageDrawWriter&){};
        bool (*record_uniforms)(GraphicsPipeline*, const VideoCommon::Nx2Stages::StageDrawReader&){};
        bool texel_buffers{}; ///< A stage has texture or image buffers: every draw joins
    };

    template <typename Spec>
    static SplitFuncs MakeSplitFuncs(bool texel_buffers);

private:
    template <typename Spec>
    bool ConfigureImpl(bool is_indexed);

    template <typename Spec>
    bool ConfigureUniformsImpl(bool is_indexed, bool rebind_storage);

    // Cut c's halves (see ConfigureBuffers).
    template <typename Spec>
    BufferHalf ConfigureBuffersImpl(bool is_indexed, VideoCommon::Nx2Stages::StageDrawWriter& out);
    template <typename Spec>
    bool ConfigureTexturesImpl(const VideoCommon::Nx2Stages::StageDrawReader& in,
                               Tegra::Engines::Maxwell3D& maxwell3d_c);
    template <typename Spec>
    bool ConfigureUniformsBuffersImpl(bool is_indexed, bool rebind_storage,
                                      VideoCommon::Nx2Stages::StageDrawWriter& out);
    template <typename Spec>
    bool RecordUniformsFromImpl(const VideoCommon::Nx2Stages::StageDrawReader& in);

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
    bool (*configure_uniforms_func)(GraphicsPipeline*, bool, bool){};
    SplitFuncs split_funcs{}; ///< Cut c (ConfigureBuffers ...)

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

    /// A constant buffer word Configure reads descriptors from, in the order ConfigureImpl reads
    /// them: per stage the storage buffer descriptors (4 words each: address and size, see
    /// BufferCache::StorageBufferBinding), then texture handles (texture buffers, image buffers,
    /// textures, images; a secondary handle right after its primary). descriptor_values holds
    /// what the last ConfigureImpl read (with EDEN_NX2_LIGHT_UNIFORM on), used_tics and used_tscs
    /// the pool indices those handles gave.
    struct DescriptorWord {
        u8 stage;
        u8 slot;
        u8 kind; ///< HandleWords or StorageWords
        u32 offset;
    };
    std::vector<DescriptorWord> descriptor_words;
    std::vector<u32> descriptor_values;
    /// Index of each stage's first storage buffer descriptor word in descriptor_words (its 4 words
    /// per descriptor follow in the order of storage_buffers_descriptors).
    std::array<u32, NUM_STAGES> storage_word_begin{};
    bool descriptor_values_valid{};
    std::vector<u32> used_tics;
    std::vector<u32> used_tscs;
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

    /// Cut c, C: the handles of the last ConfigureTextures (LastHandles).
    std::vector<u32> last_handles;

    /// EDEN_NX2_DESC_HULL: descriptor_words in runs of one slot (CheckDescriptorWords, built at
    /// its first call).
    mutable std::vector<VideoCommon::DescriptorWordRun> descriptor_word_runs;

    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    bool uses_push_descriptor{false};
    bool uses_descriptor_buffer{false};
};

} // namespace Vulkan
