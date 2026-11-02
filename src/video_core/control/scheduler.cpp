// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>
#include <vector>

#include "common/assert.h"
#include "core/core.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/scheduler.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_stage_api.h"
#include "video_core/nx2_stage_proxy.h"
#include "video_core/nx2_stages.h"

namespace Tegra::Control {

namespace {
Scheduler* g_current{};
} // Anonymous namespace

Scheduler::Scheduler() {
    g_current = this;
}

Scheduler::~Scheduler() {
    if (g_current == this) {
        g_current = nullptr;
    }
}

Scheduler* Scheduler::Current() {
    return g_current;
}

std::vector<std::shared_ptr<ChannelState>> Scheduler::Channels() {
    std::scoped_lock lk{scheduling_guard};
    std::vector<std::shared_ptr<ChannelState>> result;
    result.reserve(channels.size());
    for (const auto& [id, channel] : channels) {
        result.push_back(channel);
    }
    return result;
}

void Scheduler::BindChannelStaged(GPU& gpu, s32 bind_id) {
    namespace Stages = VideoCommon::Nx2Stages;
    auto& split = VideoCommon::GpuSplit::Get();
    ChannelState* channel;
    {
        std::scoped_lock lk{scheduling_guard};
        const auto it = channels.find(bind_id);
        ASSERT(it != channels.end());
        channel = it->second.get();
    }
    if (!staged_bind.Join(split.Session(), bind_id, channel->memory_manager,
                          Stages::ChannelStaged(*channel))) {
        split.Defer([this, gpu_ptr = &gpu, bind_id] {
            std::scoped_lock lk{scheduling_guard};
            gpu_ptr->BindChannel(bind_id);
        });
        return;
    }
    // Tagged OnA to join with its own reason (DeferTarget::Join counts as Tagged). Where ring 1
    // runs elsewhere (=3), it binds as before.
    split.DeferTo<VideoCommon::DeferTarget::OnA>([this, gpu_ptr = &gpu, channel, bind_id] {
        const auto bind = [&] {
            Stages::StageBoundChannel(*channel);
            std::scoped_lock lk{scheduling_guard};
            gpu_ptr->BindChannel(bind_id);
        };
        if (Stages::Forwarding()) {
            Stages::JoinFromA(Stages::JoinReason::Channel, bind);
        } else {
            bind();
        }
    });
}

void Scheduler::BindChannel(GPU& gpu, s32 bind_id) {
    if (VideoCommon::Nx2Stages::Active() && VideoCommon::Nx2Stages::Proxy() != nullptr)
        [[unlikely]] {
        BindChannelStaged(gpu, bind_id);
        return;
    }
    // With the GPU thread split the rasterizer's channel switches with the methods around it, on
    // the back thread (deferred outside the lock: the back takes it to bind).
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer([this, gpu_ptr = &gpu, bind_id] {
            std::scoped_lock lk{scheduling_guard};
            gpu_ptr->BindChannel(bind_id);
        });
        return;
    }
    std::scoped_lock lk{scheduling_guard};
    gpu.BindChannel(bind_id);
}

void Scheduler::Push(GPU& gpu, s32 channel, CommandList&& entries) {
    std::shared_ptr<ChannelState> channel_state;
    {
        std::unique_lock lk(scheduling_guard);
        auto it = channels.find(channel);
        ASSERT(it != channels.end());
        channel_state = it->second;
    }
    BindChannel(gpu, channel_state->bind_id);
    // Process commands outside the lock to reduce contention.
    // Multiple channels can prepare their commands in parallel.
    channel_state->payload->dma_pusher.Push(std::move(entries));
    channel_state->payload->dma_pusher.DispatchCalls();
    if (channel_state->payload->dma_pusher.HasPendingAcquire() &&
        std::find(pending_channels.begin(), pending_channels.end(), channel_state) ==
            pending_channels.end()) {
        pending_channels.push_back(channel_state);
    }
    if (!pending_channels.empty()) {
        ResumePending(gpu);
    }
}

void Scheduler::ResumePending(GPU& gpu) {
    // Running one channel can release the semaphore another waits for: retry until none moves.
    for (bool progress = true; progress;) {
        progress = false;
        for (const auto& state : pending_channels) {
            auto& dma_pusher = state->payload->dma_pusher;
            // Powered off (shutting down), DispatchCalls returns at once and the acquire stays
            // pending: resuming it again counted as progress forever and the GPU thread never
            // reached its stop request (closing the game hung).
            if (!dma_pusher.system.IsPoweredOn()) {
                return;
            }
            if (!dma_pusher.HasPendingAcquire() || !dma_pusher.CanResumeAcquire()) {
                continue;
            }
            BindChannel(gpu, state->bind_id);
            dma_pusher.DispatchCalls();
            progress = true;
        }
    }
    std::erase_if(pending_channels, [](const auto& state) {
        return !state->payload->dma_pusher.HasPendingAcquire();
    });
}

void Scheduler::DeclareChannel(std::shared_ptr<ChannelState> new_channel) {
    s32 channel = new_channel->bind_id;
    std::unique_lock lk(scheduling_guard);
    channels.emplace(channel, new_channel);
}

} // namespace Tegra::Control
