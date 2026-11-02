// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/nx2_stages.h"

// Skeleton (T0): the stages never start, every Config field is off. The bodies are T1's.
namespace VideoCommon::Nx2Stages {

const Config& Get() {
    static const Config config{};
    return config;
}

u64* Reserve(Ring ring, Thunk thunk, u32 payload_words) {
    return nullptr;
}

void Commit(Ring ring) {}

void Publish(Ring ring) {}

void PostMark(Ring ring, u64 ring1_position) {}

void FlushBeforeForward() {}

void JoinFromA(FunctionRef op) {
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

void VerifyHook() {}

void Start(Core::System& system) {}

void Stop() {}

} // namespace VideoCommon::Nx2Stages
