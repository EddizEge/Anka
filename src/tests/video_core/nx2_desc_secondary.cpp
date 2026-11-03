// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/shader_info.h"
#include "video_core/buffer_cache/nx2_draw_memo.h"
#include "video_core/renderer_vulkan/nx2_desc_secondary.h"

namespace {

using Vulkan::AddHandleWords;
using Vulkan::HasSecondaryHandle;

constexpr u32 HANDLE_WORDS = 1;
constexpr u32 STAGES = 5;
constexpr u32 SLOTS = 18;

/// GraphicsPipeline::DescriptorWord.
struct Word {
    u8 stage;
    u8 slot;
    u8 kind;
    u32 offset;
};

/// One stage's texture descriptors, as Shader::Info holds them.
struct StageDescriptors {
    Shader::TextureBufferDescriptors texture_buffers;
    Shader::ImageBufferDescriptors image_buffers;
    Shader::TextureDescriptors textures;
    Shader::ImageDescriptors images;
};

/// GraphicsPipeline's constructor: the handle words of each stage in ConfigureImpl's order.
std::vector<Word> MakeWords(std::span<const StageDescriptors> stages, bool secondary,
                            u32* secondary_words = nullptr) {
    std::vector<Word> words;
    u32 secondary_count = 0;
    for (u32 stage = 0; stage < stages.size(); ++stage) {
        const auto add = [&](u32 slot, u32 offset) {
            words.push_back(
                {static_cast<u8>(stage), static_cast<u8>(slot), HANDLE_WORDS, offset});
        };
        secondary_count += AddHandleWords(stages[stage].texture_buffers, secondary, add);
        secondary_count += AddHandleWords(stages[stage].image_buffers, secondary, add);
        secondary_count += AddHandleWords(stages[stage].textures, secondary, add);
        secondary_count += AddHandleWords(stages[stage].images, secondary, add);
    }
    if (secondary_words != nullptr) {
        *secondary_words = secondary_count;
    }
    return words;
}

/// The constant buffer words ConfigureImpl's read_handle reads, in its order and as it captures
/// them (stage, slot, offset): a texture or texture buffer handle with a secondary reads two.
std::vector<Word> ReadHandleWords(std::span<const StageDescriptors> stages) {
    std::vector<Word> reads;
    for (u32 stage = 0; stage < stages.size(); ++stage) {
        const auto read = [&](const auto& descriptors, bool has_secondary_member) {
            for (const auto& desc : descriptors) {
                for (u32 index = 0; index < desc.count; ++index) {
                    const u32 index_offset = index << desc.size_shift;
                    reads.push_back({static_cast<u8>(stage), static_cast<u8>(desc.cbuf_index),
                                     HANDLE_WORDS, desc.cbuf_offset + index_offset});
                    if (has_secondary_member) {
                        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(desc)>,
                                                     Shader::TextureDescriptor> ||
                                      std::is_same_v<std::remove_cvref_t<decltype(desc)>,
                                                     Shader::TextureBufferDescriptor>) {
                            if (desc.has_secondary) {
                                reads.push_back({static_cast<u8>(stage),
                                                 static_cast<u8>(desc.secondary_cbuf_index),
                                                 HANDLE_WORDS,
                                                 desc.secondary_cbuf_offset + index_offset});
                            }
                        }
                    }
                }
            }
        };
        read(stages[stage].texture_buffers, true);
        read(stages[stage].image_buffers, false);
        read(stages[stage].textures, true);
        read(stages[stage].images, false);
    }
    return reads;
}

/// Constant buffer memory: per stage and slot an address, words by address.
struct Memory {
    std::array<std::array<u64, SLOTS>, STAGES> slot_address{};
    std::vector<u32> words = std::vector<u32>(1u << 16);

    Memory() {
        for (u32 stage = 0; stage < STAGES; ++stage) {
            for (u32 slot = 0; slot < SLOTS; ++slot) {
                slot_address[stage][slot] = (u64{stage} * SLOTS + slot) * 0x200;
            }
        }
        for (u32 i = 0; i < words.size(); ++i) {
            words[i] = i * 2654435761u;
        }
    }
    u64 Address(u32 stage, u32 slot, u32 offset) const {
        return slot_address[stage][slot] + offset;
    }
    u32 Read(u64 address) const {
        return words[address / 4];
    }
};

/// ConfigureImpl's capture: descriptor_values sized as the words, filled with what read_handle
/// reads; whether descriptor_values_valid.
bool Capture(const Memory& memory, std::span<const Word> words,
             std::span<const StageDescriptors> stages, std::vector<u32>& values) {
    values.assign(words.size(), 0);
    size_t it = 0;
    bool fit = true;
    for (const Word& read : ReadHandleWords(stages)) {
        if (it != values.size()) {
            values[it++] = memory.Read(memory.Address(read.stage, read.slot, read.offset));
        } else {
            fit = false;
        }
    }
    return fit && it == values.size();
}

/// CheckDescriptorWords' handle part: whether a word written over (or in a rebound slot) holds a
/// value other than the one captured (any written over with the values invalid). Both scans
/// (every word, and EDEN_NX2_DESC_HULL's runs) must agree.
bool Changed(const Memory& memory, std::span<const Word> words, std::span<const u32> values,
             bool values_valid, std::span<const std::pair<u64, u32>> written,
             const std::array<u32, STAGES>& rebound) {
    const auto visit = [&](size_t i) {
        const Word& word = words[i];
        const u64 address = memory.Address(word.stage, word.slot, word.offset);
        bool rewritten = ((rebound[word.stage] >> word.slot) & 1) != 0;
        for (const auto& [begin, size] : written) {
            rewritten |= address < begin + size && begin < address + 4;
        }
        if (!rewritten) {
            return false;
        }
        return !(values_valid && memory.Read(address) == values[i]);
    };
    bool all = false;
    for (size_t i = 0; i < words.size() && !all; ++i) {
        all = visit(i);
    }
    const auto runs = VideoCommon::MakeDescriptorWordRuns(words);
    bool by_runs = false;
    VideoCommon::ForEachCandidateWord(
        std::span<const VideoCommon::DescriptorWordRun>{runs}, written,
        [&](u8 stage, u8 slot) { return memory.slot_address[stage][slot]; },
        [&](u8 stage, u8 slot) { return ((rebound[stage] >> slot) & 1) != 0; },
        [&](u32 i) { return by_runs = visit(i); });
    REQUIRE(all == by_runs);
    return all;
}

Shader::TextureDescriptor Texture(u32 slot, u32 offset, u32 count, bool secondary,
                                  u32 secondary_slot = 0, u32 secondary_offset = 0) {
    Shader::TextureDescriptor desc{};
    desc.has_secondary = secondary;
    desc.cbuf_index = slot;
    desc.cbuf_offset = offset;
    desc.secondary_cbuf_index = secondary_slot;
    desc.secondary_cbuf_offset = secondary_offset;
    desc.count = count;
    desc.size_shift = 3;
    return desc;
}

Shader::TextureBufferDescriptor TextureBuffer(u32 slot, u32 offset, bool secondary,
                                              u32 secondary_slot = 0, u32 secondary_offset = 0) {
    Shader::TextureBufferDescriptor desc{};
    desc.has_secondary = secondary;
    desc.cbuf_index = slot;
    desc.cbuf_offset = offset;
    desc.secondary_cbuf_index = secondary_slot;
    desc.secondary_cbuf_offset = secondary_offset;
    desc.count = 1;
    desc.size_shift = 2;
    return desc;
}

/// A fragment stage: a texture buffer with a secondary handle in another slot, an image buffer,
/// two textures with secondary handles (count 2), one without, an image.
std::vector<StageDescriptors> Example() {
    std::vector<StageDescriptors> stages(STAGES);
    StageDescriptors& fragment = stages[4];
    fragment.texture_buffers.push_back(TextureBuffer(2, 0x10, true, 3, 0x40));
    Shader::ImageBufferDescriptor image_buffer{};
    image_buffer.cbuf_index = 2;
    image_buffer.cbuf_offset = 0x20;
    image_buffer.count = 1;
    fragment.image_buffers.push_back(image_buffer);
    fragment.textures.push_back(Texture(2, 0x30, 2, true, 5, 0x80));
    fragment.textures.push_back(Texture(2, 0x60, 1, false));
    Shader::ImageDescriptor image{};
    image.cbuf_index = 2;
    image.cbuf_offset = 0x70;
    image.count = 1;
    fragment.images.push_back(image);
    return stages;
}

} // Anonymous namespace

TEST_CASE("Nx2DescSecondary: the trait finds the descriptors with a secondary handle",
          "[video_core][nx2]") {
    static_assert(HasSecondaryHandle<Shader::TextureDescriptor>);
    static_assert(HasSecondaryHandle<Shader::TextureBufferDescriptor>);
    static_assert(!HasSecondaryHandle<Shader::ImageDescriptor>);
    static_assert(!HasSecondaryHandle<Shader::ImageBufferDescriptor>);
    static_assert(!HasSecondaryHandle<Shader::StorageBufferDescriptor>);
    REQUIRE(HasSecondaryHandle<Shader::TextureDescriptor>);
    REQUIRE(!HasSecondaryHandle<Shader::ImageDescriptor>);
    // Through a generic lambda's parameter, as the constructor's add_handles passes them.
    const auto through_lambda = [](const auto& descriptors) {
        using Descriptor = std::remove_cvref_t<decltype(*descriptors.begin())>;
        return HasSecondaryHandle<Descriptor>;
    };
    REQUIRE(through_lambda(Shader::TextureDescriptors{}));
    REQUIRE(through_lambda(Shader::TextureBufferDescriptors{}));
    REQUIRE(!through_lambda(Shader::ImageDescriptors{}));
    REQUIRE(!through_lambda(Shader::ImageBufferDescriptors{}));
}

TEST_CASE("Nx2DescSecondary: EDEN_NX2_DESC_SECONDARY values", "[video_core][nx2]") {
    REQUIRE(Vulkan::ParseDescSecondary(nullptr));
    REQUIRE(Vulkan::ParseDescSecondary(""));
    REQUIRE(Vulkan::ParseDescSecondary("1"));
    REQUIRE(!Vulkan::ParseDescSecondary("0"));
}

TEST_CASE("Nx2DescSecondary: the words list a secondary handle right after its handle",
          "[video_core][nx2]") {
    const std::vector<StageDescriptors> stages = Example();
    u32 secondary_words = 0;
    const std::vector<Word> words = MakeWords(stages, true, &secondary_words);
    const std::vector<Word> reads = ReadHandleWords(stages);
    REQUIRE(secondary_words == 3);
    REQUIRE(words.size() == 9);
    REQUIRE(words.size() == reads.size());
    for (size_t i = 0; i < words.size(); ++i) {
        REQUIRE(words[i].stage == reads[i].stage);
        REQUIRE(words[i].slot == reads[i].slot);
        REQUIRE(words[i].offset == reads[i].offset);
        REQUIRE(words[i].kind == HANDLE_WORDS);
    }
    // texture buffer, its secondary, image buffer, texture 0, its secondary, texture 1 (at
    // 0x30 + 8), its secondary (0x80 + 8), the third texture, the image.
    REQUIRE(words[1].slot == 3);
    REQUIRE(words[1].offset == 0x40);
    REQUIRE(words[4].slot == 5);
    REQUIRE(words[4].offset == 0x80);
    REQUIRE(words[5].offset == 0x38);
    REQUIRE(words[6].slot == 5);
    REQUIRE(words[6].offset == 0x88);

    // =0: no secondary word, the count the same.
    u32 secondary_off = 0;
    const std::vector<Word> old_words = MakeWords(stages, false, &secondary_off);
    REQUIRE(secondary_off == 3);
    REQUIRE(old_words.size() == 6);
    for (const Word& word : old_words) {
        REQUIRE(word.slot == 2);
    }
}

TEST_CASE("Nx2DescSecondary: the captured values fit only with the secondary words",
          "[video_core][nx2]") {
    const Memory memory;
    const std::vector<StageDescriptors> stages = Example();
    std::vector<u32> values;
    REQUIRE(Capture(memory, MakeWords(stages, true), stages, values));
    // The old words: read_handle captures two values for a handle with a secondary, more than
    // there are words: descriptor_values_valid false.
    REQUIRE(!Capture(memory, MakeWords(stages, false), stages, values));
    // Without secondary handles both ways fit.
    std::vector<StageDescriptors> plain(STAGES);
    plain[0].textures.push_back(Texture(1, 0x10, 3, false));
    REQUIRE(Capture(memory, MakeWords(plain, true), plain, values));
    REQUIRE(Capture(memory, MakeWords(plain, false), plain, values));
    REQUIRE(MakeWords(plain, true).size() == MakeWords(plain, false).size());
}

TEST_CASE("Nx2DescSecondary: a write to a secondary handle only is seen", "[video_core][nx2]") {
    Memory memory;
    const std::vector<StageDescriptors> stages = Example();
    const std::vector<Word> words = MakeWords(stages, true);
    const std::vector<Word> old_words = MakeWords(stages, false);
    std::vector<u32> values;
    std::vector<u32> old_values;
    const bool valid = Capture(memory, words, stages, values);
    const bool old_valid = Capture(memory, old_words, stages, old_values);
    REQUIRE(valid);
    REQUIRE(!old_valid);
    const std::array<u32, STAGES> none{};

    // The texture's secondary handle (slot 5, 0x80) written with a new value.
    const u64 secondary = memory.Address(4, 5, 0x80);
    const std::array<std::pair<u64, u32>, 1> written{{{secondary, 4}}};
    memory.words[secondary / 4] ^= 0x100;
    REQUIRE(Changed(memory, words, values, valid, written, none));
    // =0 (and before the fix): no word there, the write is not seen.
    REQUIRE(!Changed(memory, old_words, old_values, old_valid, written, none));
    memory.words[secondary / 4] ^= 0x100;

    // Written with the value it held: the same handle (with the values valid).
    REQUIRE(!Changed(memory, words, values, valid, written, none));
    // The old words compare no values: any word written over counts as changed.
    const u64 primary = memory.Address(4, 2, 0x30);
    const std::array<std::pair<u64, u32>, 1> written_primary{{{primary, 4}}};
    REQUIRE(Changed(memory, old_words, old_values, old_valid, written_primary, none));
    REQUIRE(!Changed(memory, words, values, valid, written_primary, none));

    // The secondary handles' slot bound elsewhere (rebound) with other values there.
    std::array<u32, STAGES> rebound{};
    rebound[4] = 1u << 5;
    memory.slot_address[4][5] += 0x1000;
    REQUIRE(Changed(memory, words, values, valid, {}, rebound));
    REQUIRE(!Changed(memory, old_words, old_values, old_valid, {}, rebound));
}

TEST_CASE("Nx2DescSecondary: random descriptors, every word read_handle reads is checked",
          "[video_core][nx2]") {
    std::mt19937 rng{0x5ec0d};
    const auto pick = [&](u32 n) { return static_cast<u32>(rng() % n); };
    for (int round = 0; round < 2000; ++round) {
        std::vector<StageDescriptors> stages(STAGES);
        for (StageDescriptors& stage : stages) {
            for (u32 i = pick(3); i > 0; --i) {
                stage.texture_buffers.push_back(TextureBuffer(pick(SLOTS), pick(64) * 4,
                                                              pick(2) != 0, pick(SLOTS),
                                                              pick(64) * 4));
            }
            for (u32 i = pick(3); i > 0; --i) {
                stage.textures.push_back(Texture(pick(SLOTS), pick(32) * 4, 1 + pick(3),
                                                 pick(2) != 0, pick(SLOTS), pick(32) * 4));
            }
            for (u32 i = pick(2); i > 0; --i) {
                Shader::ImageDescriptor image{};
                image.cbuf_index = pick(SLOTS);
                image.cbuf_offset = pick(64) * 4;
                image.count = 1 + pick(2);
                image.size_shift = 2;
                stage.images.push_back(image);
            }
        }
        Memory memory;
        const std::vector<Word> words = MakeWords(stages, true);
        const std::vector<Word> reads = ReadHandleWords(stages);
        REQUIRE(words.size() == reads.size());
        std::vector<u32> values;
        const bool valid = Capture(memory, words, stages, values);
        REQUIRE(valid);
        if (reads.empty()) {
            continue;
        }
        // Any word read_handle reads, given a new value, is seen; the same value is not.
        const Word& read = reads[pick(static_cast<u32>(reads.size()))];
        const u64 address = memory.Address(read.stage, read.slot, read.offset);
        const std::array<std::pair<u64, u32>, 1> written{{{address, 4}}};
        const std::array<u32, STAGES> none{};
        REQUIRE(!Changed(memory, words, values, valid, written, none));
        memory.words[address / 4] += 1;
        REQUIRE(Changed(memory, words, values, valid, written, none));
    }
}
