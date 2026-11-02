// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include "common/common_types.h"

namespace VideoCommon::Nx2ModePace {

// The decision part of EDEN_NX2_MODE_PACE (nx2_mode_pace.h), without clocks, settings or logging
// (src/tests/video_core/nx2_mode_pace.cpp).
//
// Donkey Kong Bananza switches between 60, 30 and 20 fps (1, 2, 3 vblanks a frame) on late frames,
// not on the GPU time it reads: at 60, frames of 20-33 ms never make it drop to 30 (minutes of slow
// motion), while a few frames of 34 ms or more between two of its presents do within 3-23 frames;
// at 30, frames of 34 ms or more take it back to 60, and at 20, frames of ~52-58 ms or more to 30.
// So Eden holds the game's present (after the vblank pacing) until hold_ms after the previous one
// for a few frames to move it where its own frame cost K fits:
//  - 60 -> 30 ("down") while from60_ms < K < self60_ms (from self60_ms on the game drops itself);
//  - 30 -> 60 ("up") while K <= to60_ms, after up_dwell_s of it;
//  - 20 -> 30 ("up20") while K <= to30_ms;
//  - 30 -> 20 ("to20", experimental, off): two frames of to20_ms when the game keeps escaping
//  to 60.
// K = max(median work, median busy) over the last 16 frames without a stall: the work is the frame
// period without Eden's pacing, the busy time the GPU pipeline's (at 30 the work misses the split's
// back thread: ~12 ms of work against ~22 ms busy). A push ends when the game changes its mode, or
// after its frames (then the next one of its kind waits a rest that doubles with each failure, and
// after `tries` failures none until the game changes its mode by itself), or at a stall (aborted:
// a rest that doubles too, but no block: a load's stalls would otherwise block the push the stuck
// 60 after it needs). An up20 push whose 30 the game leaves by itself within verify30_s failed as
// well, and that change keeps its tries (else 20 -> 30 -> 20 goes round with nothing growing).

enum class Push : u8 { None, Down, Up, Up20, To20 };
enum class Result : u8 { None, Done, Done20, GaveUp, Aborted, Other };

constexpr std::string_view PushName(Push push) {
    switch (push) {
    case Push::Down:
        return "down";
    case Push::Up:
        return "up";
    case Push::Up20:
        return "up20";
    case Push::To20:
        return "to20";
    default:
        return "-";
    }
}

constexpr std::string_view ResultName(Result result) {
    switch (result) {
    case Result::Done:
        return "done";
    case Result::Done20: // a down push that took the game to 20 (it skipped 30)
        return "done20";
    case Result::GaveUp:
        return "gave up";
    case Result::Aborted:
        return "aborted";
    case Result::Other:
        return "other";
    default:
        return "-";
    }
}

struct Params {
    bool auto_push = true;    ///< false: only the probe pushes
    bool allow60 = true;      ///< false: no up push (30 stays)
    double from60_ms = 17.5;  ///< at 60, K above this: down
    double self60_ms = 34.0;  ///< at 60, K at or above this: no push (the game drops itself)
    double down_dwell_s = 0.5;
    double to60_ms = 13.0;    ///< at 30, K at or below this: up
    double up_dwell_s = 5.0;  ///< base; doubles (once a push) when 60 does not last verify60_s,
                              ///< halves back every stable60_s without that
    double up_dwell_max_s = 60.0;
    double verify60_s = 10.0;
    double stable60_s = 60.0; ///< this long at 60: up_dwell_s back to its base
    double to30_ms = 25.0;    ///< at 20, K at or below this: up20 (O1: 30 kept at 20's 19.6-24.3)
    double up20_dwell_s = 2.0;
    double verify30_s = 10.0;   ///< the game leaving 30 by itself this soon after up20: it failed
    double after_up20_s = 10.0; ///< no up push this long after up20 succeeded
    double hold_ms = 42.0;      ///< down (first stage) and up
    double hold_late_ms = 46.0; ///< down, second stage
    double hold20_ms = 56.0;    ///< up20, climbing hold20_step_ms every hold20_step_frames
    double hold20_step_ms = 6.0;
    double hold20_max_ms = 68.0;
    u32 hold20_step_frames = 10;
    double up_single_ms = 0.0; ///< > 0: an up push holds only its first frame, this long
    u32 down_frames = 30;      ///< each of the two stages
    u32 up_frames = 90;
    u32 up20_frames = 40;
    double down_rest_s = 2.0;
    double down_rest_max_s = 60.0;
    u32 down_tries = 3;
    double up_rest_s = 20.0;
    double up_rest_max_s = 160.0;
    u32 up_tries = 3;
    double up20_rest_s = 5.0;
    double up20_rest_max_s = 60.0;
    u32 up20_tries = 3;
    double stall_ms = 100.0;     ///< work above this: a stall (not in the window, ends a push)
    double stall_quiet_s = 2.0;  ///< no new push this long after a stall
    double mode_quiet_s = 1.0;   ///< no decision this long after the game changed its mode
    double mode_quiet60_s = 0.5; ///< the same at 60 (it does not leave 60 by itself at 17.5-34)
    bool to20 = false;
    double to20_k_ms = 34.0;
    double to20_ms = 110.0;
    u32 to20_frames = 2;
    double to20_rest_s = 30.0;
};

/// EDEN_NX2_MODE_PACE_PROBE=<down|up|up20|to20|cycle>:<seconds>: a push every so often whatever K.
struct Probe {
    Push kind{Push::None};
    bool cycle{}; ///< the push the game's mode allows: down at 60, up at 30, up20 at 20
    double every_s{};
};

/// `text` without the spaces and tabs around it (a .bat's `set X=cycle:10 ` keeps the space).
constexpr std::string_view Trim(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// The probe's interval counts from the end of the previous push (of any kind).
inline std::optional<Probe> ParseProbe(std::string_view text) {
    text = Trim(text);
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view kind = Trim(text.substr(0, colon));
    const std::string_view number = Trim(text.substr(colon + 1));
    Probe probe{};
    if (kind == "down") {
        probe.kind = Push::Down;
    } else if (kind == "up") {
        probe.kind = Push::Up;
    } else if (kind == "up20") {
        probe.kind = Push::Up20;
    } else if (kind == "to20") {
        probe.kind = Push::To20;
    } else if (kind == "cycle") {
        probe.cycle = true;
    } else {
        return std::nullopt;
    }
    if (number.empty() || number.size() > 15) {
        return std::nullopt;
    }
    double value = 0.0;
    double scale = 0.0; // 0: before the point
    for (const char c : number) {
        if (c == '.' && scale == 0.0) {
            scale = 1.0;
            continue;
        }
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        if (scale == 0.0) {
            value = value * 10.0 + (c - '0');
        } else {
            scale /= 10.0;
            value += (c - '0') * scale;
        }
    }
    if (value < 1.0 || value > 3600.0) {
        return std::nullopt;
    }
    probe.every_s = value;
    return probe;
}

struct Input {
    u32 mode{};        ///< vblanks a frame the game asks for (1 = 60, 2 = 30, 3 = 20)
    double work_ms{};  ///< present to present without Eden's pacing and holding
    double busy_ms{};  ///< GPU pipeline busy time since the previous present
    double dt_ms{};    ///< present to present (paced)
};

/// One present's measures from its times (ms on one steady clock) and the GPU busy time so far.
struct Measured {
    Input input;
    double gap_ms{};  ///< request to request: the frame time the game sees (229 word 10)
    double pace_ms{}; ///< the vblank pacing and the hold
};

class Meter {
public:
    /// Nothing for the first present: it only records the times (the busy time since the start
    /// would enter the window).
    std::optional<Measured> Present(u32 mode, double entered_ms, double composited_ms,
                                    double paced_ms, double busy_total_ms) {
        const bool first = !have;
        const Measured m{
            .input = {.mode = mode,
                      .work_ms = (std::max)(paced_ms - last_paced - (paced_ms - composited_ms), 0.0),
                      .busy_ms = (std::max)(busy_total_ms - last_busy, 0.0),
                      .dt_ms = (std::max)(paced_ms - last_paced, 0.0)},
            .gap_ms = (std::max)(entered_ms - last_entered, 0.0),
            .pace_ms = (std::max)(paced_ms - composited_ms, 0.0),
        };
        have = true;
        last_paced = paced_ms;
        last_entered = entered_ms;
        last_busy = (std::max)(busy_total_ms, last_busy);
        if (first) {
            return std::nullopt;
        }
        return m;
    }

private:
    bool have{};
    double last_paced{};
    double last_entered{};
    double last_busy{};
};

/// Sleeps until `target`; with `spin_us`, wakes that long before it and yields until it (the
/// Windows timer overshoots a sleep by up to a few ms). 0: std::this_thread::sleep_until alone.
inline void SleepUntil(std::chrono::steady_clock::time_point target, u32 spin_us) {
    if (spin_us == 0) {
        std::this_thread::sleep_until(target);
        return;
    }
    const auto early = target - std::chrono::microseconds{spin_us};
    if (std::chrono::steady_clock::now() < early) {
        std::this_thread::sleep_until(early);
    }
    while (std::chrono::steady_clock::now() < target) {
        std::this_thread::yield();
    }
}

struct Output {
    double min_period_ms{}; ///< the hold of the next present in the push's mode (0: none)
    bool pushing{};
    Push started{Push::None};
    bool by_probe{};
    Push ended{Push::None};
    Result result{Result::None};
    u32 frames{}; ///< presents the ended push lasted (both stages of a down push)
    /// An up20 push that ended done, whose 30 the game left by itself within verify30_s: failed
    /// after all (rest_s, tries, aborts and blocked are its).
    Push unverified{Push::None};
    double rest_s{}; ///< at a failed end: the rest before the next push of its kind
    u32 tries{};     ///< failures in a row of that kind (not counting aborts)
    u32 aborts{};    ///< aborted pushes in a row of that kind (a rest, no block)
    bool blocked{};  ///< no push of that kind until the game changes its mode by itself
    bool mode_changed{};
    u32 old_mode{};
    Push by{Push::None};      ///< the push that was running when the mode changed
    double change_k_ms{NAN};  ///< K just before the mode change
    double k_ms{NAN};         ///< NaN: fewer than 8 frames in the window
    double work_med_ms{NAN};
    double busy_med_ms{NAN};
};

/// The median of up to 16 values (the mean of the middle two for an even count); NaN when empty.
inline double Median(std::span<const float> values) {
    std::array<float, 16> sorted{};
    const std::size_t n = (std::min)(values.size(), sorted.size());
    if (n == 0) {
        return NAN;
    }
    std::copy_n(values.begin(), n, sorted.begin());
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n));
    if (n % 2 == 1) {
        return sorted[n / 2];
    }
    return (static_cast<double>(sorted[n / 2 - 1]) + sorted[n / 2]) / 2.0;
}

class Controller {
public:
    static constexpr std::size_t Window = 16;
    static constexpr std::size_t MinSamples = 8;
    static constexpr std::size_t MinSamplesTo20 = 4;
    static constexpr u32 To20WaitFrames = 10; ///< after its held frames, for the mode to change

    explicit Controller(const Params& params_ = {}, std::optional<Probe> probe_ = {})
        : params{params_}, probe{probe_}, up_dwell{params_.up_dwell_s} {}

    Output Step(const Input& in) {
        Output out{};
        const double dt = std::clamp(std::isfinite(in.dt_ms) ? in.dt_ms : 0.0, 0.0, 1000.0) / 1000.0;
        clock += dt;
        since_stall += dt;
        stint += dt;
        since_up20 += dt;
        since_probe += dt;
        down.Tick(dt);
        up.Tick(dt);
        up20.Tick(dt);
        to20_rest = (std::max)(to20_rest - dt, 0.0);
        if (up_dwell > params.up_dwell_s && clock - up_dwell_at >= params.stable60_s) {
            // Not raised for a while (also at 30, where it waits): halve it back.
            up_dwell = (std::max)(up_dwell / 2.0, params.up_dwell_s);
            up_dwell_at = clock;
        }
        if (verify30 && since_up20 >= params.verify30_s) {
            // The up20 push's 30 lasted: done after all.
            verify30 = false;
            up20.Reset();
        }

        // 1. The game changed its mode.
        if (in.mode != mode) {
            out.change_k_ms = Cost(MinSamples);
            const Push by = push;
            // The game leaves the 30 an up20 push took it to (the end below may start a trial).
            const bool on_trial = mode == 2 && verify30 && since_up20 < params.verify30_s;
            verify30 = false;
            if (push != Push::None) {
                End(out, ResultOnMode(push, in.mode));
            }
            if (by == Push::None) {
                if (mode == 2 && in.mode == 1) {
                    escapes[escape_next++ % escapes.size()] = clock;
                }
                if (mode == 1 && in.mode == 2 && clock - up_done_at < params.verify60_s) {
                    // 60 did not last after an up push: wait longer before the next one.
                    RaiseUpDwell();
                }
                bool keep_up20 = false;
                if (on_trial) {
                    // 30 did not last after an up20 push: that push failed, and this change
                    // leaves its tries alone.
                    up20.Fail(params.up20_rest_s, params.up20_rest_max_s, params.up20_tries, false);
                    out.unverified = Push::Up20;
                    Report(out, up20);
                    keep_up20 = true;
                }
                // Only the game's own change lifts the rests and blocks (not Eden's pushes).
                down.Reset();
                up.Reset();
                if (!keep_up20) {
                    up20.Reset();
                }
            }
            out.mode_changed = true;
            out.old_mode = mode;
            out.by = by;
            mode = in.mode;
            count = 0;
            next = 0;
            stint = 0.0;
            cond_down = cond_up = cond_up20 = 0.0;
        }

        // 2. A stall (loading, shader build): out of the window, ends a push.
        if (in.work_ms > params.stall_ms) {
            since_stall = 0.0;
            if (push != Push::None) {
                End(out, Result::Aborted);
            }
            cond_down = cond_up = cond_up20 = 0.0;
            Fill(out);
            return out;
        }

        // 3. The window.
        work[next] = static_cast<float>((std::max)(in.work_ms, 0.0));
        busy[next] = static_cast<float>((std::max)(in.busy_ms, 0.0));
        next = (next + 1) % Window;
        count = (std::min)(count + 1, Window);

        // 4. A push goes on until its frames are spent.
        if (push != Push::None) {
            ++frames;
            switch (push) {
            case Push::Down:
                // frames counts both stages (the log's "after N presents").
                if (frames >= params.down_frames * 2) {
                    End(out, Result::GaveUp);
                } else {
                    stage = frames >= params.down_frames ? 1 : 0;
                }
                break;
            case Push::Up:
                if (frames >= params.up_frames) {
                    End(out, Result::GaveUp);
                }
                break;
            case Push::Up20:
                if (frames >= params.up20_frames) {
                    End(out, Result::GaveUp);
                }
                break;
            case Push::To20:
                if (frames >= params.to20_frames + To20WaitFrames) {
                    End(out, Result::GaveUp);
                }
                break;
            default:
                break;
            }
            if (push != Push::None) {
                hold = CurrentHold();
                Fill(out);
                return out;
            }
        }

        // 5. Decisions.
        if (mode == 1 && stint >= params.stable60_s) {
            up_dwell = params.up_dwell_s;
        }
        const double k = Cost(MinSamples);
        const double quiet = mode == 1 ? params.mode_quiet60_s : params.mode_quiet_s;
        const bool settled = stint >= quiet && since_stall >= params.stall_quiet_s && !std::isnan(k);
        if (!params.auto_push || !settled || push != Push::None) {
            cond_down = cond_up = cond_up20 = 0.0;
        }
        if (params.auto_push && since_stall >= params.stall_quiet_s) {
            switch (mode) {
            case 1:
                if (settled && k > params.from60_ms && k < params.self60_ms && down.Ready()) {
                    cond_down += dt;
                } else {
                    cond_down = 0.0;
                }
                if (cond_down >= params.down_dwell_s && cond_down > 0.0) {
                    if (clock - up_done_at < params.verify60_s) {
                        RaiseUpDwell();
                    }
                    Start(out, Push::Down, false);
                }
                break;
            case 2:
                if (settled && params.allow60 && up.Ready() && since_up20 >= params.after_up20_s &&
                    k <= params.to60_ms) {
                    cond_up += dt;
                } else {
                    cond_up = 0.0;
                }
                if (cond_up >= up_dwell && cond_up > 0.0) {
                    Start(out, Push::Up, false);
                } else if (params.to20 && to20_rest == 0.0 && RecentEscapes(20.0) >= 2) {
                    // Without the mode quiet time (the game just came back from 60), from 4 frames.
                    const double k20 = Cost(MinSamplesTo20);
                    if (!std::isnan(k20) && k20 >= params.to20_k_ms) {
                        Start(out, Push::To20, false);
                    }
                }
                break;
            case 3:
                if (settled && up20.Ready() && k <= params.to30_ms) {
                    cond_up20 += dt;
                } else {
                    cond_up20 = 0.0;
                }
                if (cond_up20 >= params.up20_dwell_s && cond_up20 > 0.0) {
                    Start(out, Push::Up20, false);
                }
                break;
            default:
                break;
            }
        }

        // 7. The probe: whatever K, the rests and the dwells.
        if (probe && push == Push::None && since_stall >= params.stall_quiet_s &&
            since_probe >= probe->every_s) {
            Push kind = Push::None;
            if (probe->cycle) {
                kind = mode == 1 ? Push::Down : mode == 2 ? Push::Up : mode == 3 ? Push::Up20 : Push::None;
            } else if ((probe->kind == Push::Down && mode == 1) ||
                       (probe->kind == Push::Up && mode == 2) ||
                       (probe->kind == Push::Up20 && mode == 3) ||
                       (probe->kind == Push::To20 && mode == 2)) {
                kind = probe->kind;
            }
            if (kind != Push::None) {
                Start(out, kind, true);
                since_probe = 0.0;
            }
        }
        Fill(out);
        return out;
    }

    /// The hold of a present the game made in `game_mode`: none once the game left the mode the
    /// push started in (that present would be a late frame of the new mode and could undo it).
    double HoldMs(u32 game_mode) const {
        return push != Push::None && game_mode == push_from ? hold : 0.0;
    }

    double UpDwell() const {
        return up_dwell;
    }
    /// The controller's clock: the sum of the present intervals, each at most 1 s.
    double Clock() const {
        return clock;
    }
    Push Pushing() const {
        return push;
    }
    u32 Tries(Push kind) const {
        const Backoff* b = Of(kind);
        return b ? b->fails : 0;
    }
    u32 Aborts(Push kind) const {
        const Backoff* b = Of(kind);
        return b ? b->aborts : 0;
    }
    bool Blocked(Push kind) const {
        const Backoff* b = Of(kind);
        return b && b->blocked;
    }

private:
    /// Failures in a row of a push kind: a rest of base * 2^(n - 1) (at most max) after the n-th
    /// (aborts too), none at all after `tries` failures that were not aborts until the game
    /// changes its mode by itself.
    struct Backoff {
        u32 fails{};
        u32 aborts{};
        bool blocked{};
        double rest{};

        void Tick(double dt) {
            rest = (std::max)(rest - dt, 0.0);
        }
        bool Ready() const {
            return !blocked && rest == 0.0;
        }
        void Fail(double base, double max, u32 tries, bool aborted) {
            ++(aborted ? aborts : fails);
            const u32 n = (std::min)(fails + aborts, 30U);
            rest = (std::min)(base * std::ldexp(1.0, static_cast<int>(n) - 1), max);
            blocked = fails >= tries;
        }
        void Reset() {
            *this = {};
        }
    };

    static Result ResultOnMode(Push kind, u32 new_mode) {
        switch (kind) {
        case Push::Down:
            return new_mode == 2 ? Result::Done : new_mode == 3 ? Result::Done20 : Result::Other;
        case Push::Up:
            return new_mode == 1 ? Result::Done : Result::Other;
        case Push::Up20:
            return new_mode == 2 ? Result::Done : Result::Other;
        case Push::To20:
            return new_mode == 3 ? Result::Done : Result::Other;
        default:
            return Result::None;
        }
    }

    Backoff* Of(Push kind) {
        return const_cast<Backoff*>(std::as_const(*this).Of(kind));
    }
    const Backoff* Of(Push kind) const {
        switch (kind) {
        case Push::Down:
            return &down;
        case Push::Up:
            return &up;
        case Push::Up20:
            return &up20;
        default:
            return nullptr;
        }
    }

    double Cost(std::size_t min_samples) const {
        if (count < min_samples) {
            return NAN;
        }
        return (std::max)(Median({work.data(), count}), Median({busy.data(), count}));
    }

    double CurrentHold() const {
        switch (push) {
        case Push::Down:
            return stage == 0 ? params.hold_ms : params.hold_late_ms;
        case Push::Up:
            if (params.up_single_ms > 0.0) {
                return frames == 0 ? params.up_single_ms : 0.0;
            }
            return params.hold_ms;
        case Push::Up20: {
            if (params.hold20_step_ms <= 0.0 || params.hold20_step_frames == 0) {
                return params.hold20_ms;
            }
            const double climbed =
                params.hold20_ms + params.hold20_step_ms * (frames / params.hold20_step_frames);
            return (std::min)(climbed, (std::max)(params.hold20_max_ms, params.hold20_ms));
        }
        case Push::To20:
            return frames < params.to20_frames ? params.to20_ms : 0.0;
        default:
            return 0.0;
        }
    }

    u32 RecentEscapes(double within_s) const {
        u32 n = 0;
        for (const double at : escapes) {
            n += at >= 0.0 && clock - at <= within_s ? 1 : 0;
        }
        return n;
    }

    void Start(Output& out, Push kind, bool by_probe) {
        push = kind;
        push_from = mode;
        push_probe = by_probe;
        stage = 0;
        frames = 0;
        cond_down = cond_up = cond_up20 = 0.0;
        hold = CurrentHold();
        out.started = kind;
        out.by_probe = by_probe;
    }

    void End(Output& out, Result result) {
        out.ended = push;
        out.result = result;
        out.frames = frames;
        // A stall (Aborted) rests too: a loading screen with stalls would push again and again.
        const bool failed =
            result == Result::GaveUp || result == Result::Other || result == Result::Aborted;
        const bool aborted = result == Result::Aborted;
        if (push == Push::To20) {
            to20_rest = params.to20_rest_s;
            out.rest_s = to20_rest;
        } else if (Backoff* b = Of(push); b != nullptr && !push_probe) {
            // Probe pushes leave the backoff alone.
            if (failed) {
                switch (push) {
                case Push::Down:
                    b->Fail(params.down_rest_s, params.down_rest_max_s, params.down_tries, aborted);
                    break;
                case Push::Up:
                    b->Fail(params.up_rest_s, params.up_rest_max_s, params.up_tries, aborted);
                    break;
                default:
                    b->Fail(params.up20_rest_s, params.up20_rest_max_s, params.up20_tries, aborted);
                    break;
                }
                Report(out, *b);
            } else if (result == Result::Done20 || (result == Result::Done && push != Push::Up20)) {
                // An up20 push's tries go once its 30 has lasted verify30_s (step 5).
                b->Reset();
            }
        }
        if (push == Push::Up && result == Result::Done) {
            up_done_at = clock;
            up_unverified = true;
        }
        if (push == Push::Up20 && result == Result::Done) {
            since_up20 = 0.0;
            verify30 = !push_probe;
        }
        push = Push::None;
        hold = 0.0;
        frames = 0;
        stage = 0;
        since_probe = 0.0;
    }

    static void Report(Output& out, const Backoff& b) {
        out.rest_s = b.rest;
        out.tries = b.fails;
        out.aborts = b.aborts;
        out.blocked = b.blocked;
    }

    /// Once an up push: doubles the dwell before the next one (at most up_dwell_max_s).
    void RaiseUpDwell() {
        if (!up_unverified) {
            return;
        }
        up_unverified = false;
        up_dwell = (std::min)(up_dwell * 2.0, params.up_dwell_max_s);
        up_dwell_at = clock;
    }

    void Fill(Output& out) const {
        out.min_period_ms = push != Push::None ? hold : 0.0;
        out.pushing = push != Push::None;
        if (count >= MinSamples) {
            out.work_med_ms = Median({work.data(), count});
            out.busy_med_ms = Median({busy.data(), count});
            out.k_ms = (std::max)(out.work_med_ms, out.busy_med_ms);
        }
    }

    Params params;
    std::optional<Probe> probe;

    u32 mode{}; ///< 0 before the first present: the first one counts as a change
    double clock{};
    double since_stall{1e9};
    double stint{};
    double since_up20{1e9};
    double since_probe{};
    double up_done_at{-1e9};
    bool up_unverified{}; ///< the last up push's 60 has not raised the dwell yet
    double up_dwell{};
    double up_dwell_at{}; ///< clock of the last raise or halving
    bool verify30{};      ///< the last up20 push's 30 is still on trial
    std::array<double, 4> escapes{-1.0, -1.0, -1.0, -1.0}; ///< clock of the game's own 30 -> 60
    std::size_t escape_next{};

    std::array<float, Window> work{};
    std::array<float, Window> busy{};
    std::size_t count{};
    std::size_t next{};

    double cond_down{};
    double cond_up{};
    double cond_up20{};

    Push push{Push::None};
    u32 push_from{};
    bool push_probe{};
    u32 stage{};
    u32 frames{};
    double hold{};

    Backoff down;
    Backoff up;
    Backoff up20;
    double to20_rest{};
};

} // namespace VideoCommon::Nx2ModePace
