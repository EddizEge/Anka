// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/nx2_profile.h"
#include "video_core/nx2_stages.h"

// Skeleton (T0): the stages never start, every Config field is off. The bodies are T1's.
namespace VideoCommon::Nx2Stages {

// JoinFromA counts Counter::JoinsAEngine + reason.
static_assert(static_cast<size_t>(Nx2Profile::Counter::JoinsAOther) -
                  static_cast<size_t>(Nx2Profile::Counter::JoinsAEngine) + 1 ==
              static_cast<size_t>(JoinReason::Count));

const Config& Get() {
    static const Config config{};
    return config;
}

u64* Reserve(Ring ring, Thunk thunk, u32 payload_words) {
    return nullptr;
}

void Commit(Ring ring) {}

void Publish(Ring ring) {}

void Forward(Ring ring, Ring1Thunk thunk, const u64* payload, u32 payload_words) {}

void PostCbData(Ring ring, Tegra::Engines::Maxwell3D& primary, GPUVAddr address, const u32* data,
                u32 count) {}

void PostTarget(Ring ring, Tegra::Engines::Maxwell3D* primary) {}

void PostMark(Ring ring, u64 ring1_position) {}

void JoinFromA(JoinReason reason, FunctionRef op) {
    op();
}

void JoinFromB(FunctionRef op) {
    op();
}

void ParkB(FunctionRef op) {
    op();
}

void CheckStage(Stage stage, const char* where) {}

Stage RecordOwner() {
    return Stage::B;
}

void Start(Core::System& system) {}

void Stop() {}

} // namespace VideoCommon::Nx2Stages
