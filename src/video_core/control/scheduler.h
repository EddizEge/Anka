// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include "common/container/unordered_map.h"

#include "video_core/dma_pusher.h"
#include "video_core/nx2_stage_bind.h"

namespace Tegra {

class GPU;

namespace Control {

struct ChannelState;

class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    void Push(GPU& gpu, s32 channel, CommandList&& entries);

    /// Runs channels whose pending semaphore acquire is now met.
    void ResumePending(GPU& gpu);
    [[nodiscard]] bool HasPendingChannels() const {
        return !pending_channels.empty();
    }

    void DeclareChannel(std::shared_ptr<ChannelState> new_channel);

    /// Back stages (nx2_stage_proxy.cpp): the scheduler of the GPU there is (one at a time), or
    /// null.
    [[nodiscard]] static Scheduler* Current();
    /// The channels declared so far, initialized or not (InstallProxy stages them all, S3).
    [[nodiscard]] std::vector<std::shared_ptr<ChannelState>> Channels();

private:
    void BindChannel(GPU& gpu, s32 bind_id);
    /// BindChannel while the back stages run (nx2_stages.h): Owner or a channel join on A, which
    /// stages the channel first (StagedBind).
    void BindChannelStaged(GPU& gpu, s32 bind_id);

    ::Common::unordered_map<s32, std::shared_ptr<ChannelState>> channels;
    std::mutex scheduling_guard;
    std::vector<std::shared_ptr<ChannelState>> pending_channels; ///< Suspended on an acquire
    VideoCommon::Nx2Stages::StagedBind staged_bind; ///< BindChannelStaged (front)
};

} // namespace Control

} // namespace Tegra
