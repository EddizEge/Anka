// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// LoadPipelines on damaged pipeline cache files: the complete records before a damaged one are
// loaded and the file is cut back to them (Lichii #815, adapted).

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stop_token>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/stage.h"
#include "video_core/shader_environment.h"

namespace {

constexpr u32 CacheVersion = 0x7e57;
constexpr size_t KeySize = 8;

template <typename T>
void Put(std::string& out, const T& value) {
    out.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

/// One environment as GenericEnvironment::Serialize writes it, with no texture or constant
/// buffer values and `code_size` bytes of zero code.
void PutEnvironment(std::string& out, Shader::Stage stage, u64 code_size = 16) {
    Put(out, code_size);
    for (int i = 0; i < 4; ++i) {
        Put(out, u64{0}); // texture types, pixel formats, cbuf values, cbuf replacements
    }
    Put(out, u32{0});                                       // local_memory_size
    Put(out, u32{0});                                       // texture_bound
    Put(out, u32{0});                                       // start_address
    Put(out, u32{0});                                       // read_lowest
    Put(out, static_cast<u32>(code_size > 8 ? code_size - 8 : 0)); // read_highest
    Put(out, u32{1});                                       // viewport_transform_state
    Put(out, stage);
    out.append(static_cast<size_t>(code_size), '\0');
    if (stage == Shader::Stage::Compute) {
        Put(out, std::array<u32, 3>{1, 1, 1}); // workgroup_size
        Put(out, u32{0});                      // shared_memory_size
    } else {
        out.append(sizeof(Shader::ProgramHeader), '\0');
        if (stage == Shader::Stage::Geometry) {
            Put(out, std::array<u32, 8>{});
        }
    }
}

std::string ComputeRecord() {
    std::string out;
    Put(out, u32{1});
    PutEnvironment(out, Shader::Stage::Compute);
    out.append(KeySize, 'k');
    return out;
}

std::string GraphicsRecord(const std::vector<Shader::Stage>& stages) {
    std::string out;
    Put(out, static_cast<u32>(stages.size()));
    for (const Shader::Stage stage : stages) {
        PutEnvironment(out, stage);
    }
    out.append(KeySize, 'g');
    return out;
}

std::string Header() {
    std::string out{"yuzucach"};
    Put(out, CacheVersion);
    return out;
}

struct Loaded {
    size_t compute{};
    std::vector<size_t> graphics; ///< Environments of each graphics record
};

struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::string& contents) {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("eden_pipeline_cache_test_" + std::to_string(++counter) + ".bin");
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    u64 Size() const {
        return static_cast<u64>(std::filesystem::file_size(path));
    }
};

Loaded Load(const std::filesystem::path& path) {
    Loaded loaded;
    std::stop_source stop;
    VideoCommon::LoadPipelines(
        stop.get_token(), path, CacheVersion,
        [&loaded](std::ifstream& file, VideoCommon::FileEnvironment) {
            std::array<char, KeySize> key;
            file.read(key.data(), key.size());
            ++loaded.compute;
        },
        [&loaded](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> envs) {
            std::array<char, KeySize> key;
            file.read(key.data(), key.size());
            loaded.graphics.push_back(envs.size());
        });
    return loaded;
}

} // namespace

TEST_CASE("A record cut short is dropped and the file cut back", "[pipeline_cache]") {
    const std::string good = Header() + ComputeRecord() + ComputeRecord();
    const std::string third = ComputeRecord();
    const TempFile file{good + third.substr(0, third.size() / 2)};
    const Loaded loaded = Load(file.path);
    CHECK(loaded.compute == 2);
    CHECK(file.Size() == good.size());
}

TEST_CASE("Zero bytes after the records (no environments) are cut off", "[pipeline_cache]") {
    const std::string good = Header() + ComputeRecord();
    const TempFile file{good + std::string(64, '\0')};
    CHECK(Load(file.path).compute == 1);
    CHECK(file.Size() == good.size());
}

TEST_CASE("A record naming seven environments is cut off", "[pipeline_cache]") {
    const std::string good = Header() + ComputeRecord();
    std::string bad;
    Put(bad, u32{7});
    for (int i = 0; i < 7; ++i) {
        PutEnvironment(bad, Shader::Stage::Fragment);
    }
    bad.append(KeySize, 'g');
    const TempFile file{good + bad};
    const Loaded loaded = Load(file.path);
    CHECK(loaded.compute == 1);
    CHECK(loaded.graphics.empty());
    CHECK(file.Size() == good.size());
}

TEST_CASE("A graphics record with all six environments loads", "[pipeline_cache]") {
    const std::string contents =
        Header() + GraphicsRecord({Shader::Stage::VertexA, Shader::Stage::VertexB,
                                   Shader::Stage::TessellationControl,
                                   Shader::Stage::TessellationEval, Shader::Stage::Geometry,
                                   Shader::Stage::Fragment});
    const TempFile file{contents};
    const Loaded loaded = Load(file.path);
    REQUIRE(loaded.graphics.size() == 1);
    CHECK(loaded.graphics[0] == 6);
    CHECK(file.Size() == contents.size());
}

TEST_CASE("A graphics record holding a compute environment is cut off", "[pipeline_cache]") {
    const std::string good = Header() + GraphicsRecord({Shader::Stage::VertexB, Shader::Stage::Fragment});
    const TempFile file{good + GraphicsRecord({Shader::Stage::VertexB, Shader::Stage::Compute})};
    const Loaded loaded = Load(file.path);
    CHECK(loaded.graphics.size() == 1);
    CHECK(file.Size() == good.size());
}

TEST_CASE("Code claimed past the end of the file is not allocated", "[pipeline_cache]") {
    const std::string good = Header() + ComputeRecord();
    std::string bad;
    Put(bad, u32{1});
    PutEnvironment(bad, Shader::Stage::Compute, 16);
    // The code size field is the record's first u64 after num_envs: claim 1 TiB.
    const u64 huge = u64{1} << 40;
    std::memcpy(bad.data() + sizeof(u32), &huge, sizeof(huge));
    const TempFile file{good + bad};
    CHECK(Load(file.path).compute == 1);
    CHECK(file.Size() == good.size());
}
