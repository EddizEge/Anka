// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>

#include "common/common_types.h"
#include "video_core/nx2_mode_pace_logic.h"

namespace VideoCommon::Nx2ModePace {

// The decision part of EDEN_NX2_MODE_PACE=2 (v2.1; nx2_mode_pace.h), beside v2's Controller, which
// =1 keeps as it is (src/tests/video_core/nx2_mode_pace21.cpp).
//
// What v2 showed in Bananza (02.11 runs 73 and 79, 60 / 30 / 20 = 1 / 2 / 3 vblanks):
//  - a present held 42 ms after the previous one takes the game to 20 wherever it is: v2's down
//    pushes from 60 (4 of 4 "done20" in 3 presents) and its up pushes from 30 (14 of 14). Frames of
//    34-41.7 ms at 60 took it to 30 (S14, S15). 80% of 79's time at 20 followed those pushes.
//  - late frames at 20 do not take it up: up20's 56-68 ms holds worked once in 20 tries. The game
//    leaves 20 after a frame whose request came 36.6 ms or less after the previous one (all 19 of
//    its 20 -> 30 changes in both runs, each after a hitch the vblank pacing absorbed).
// So v2.1 holds presents only inside 30's window, releases them early to take 20 up, and decides by
// the emulator's own capacity C = max(median busy, median work) per frame (busy: the GPU pipeline's
// busy time; work: present to present without the pacing and the holds), not by the game's frames:
//  - 60 -> 30 ("down") while cap60_ms < C < self60_ms for down_dwell_s: presents held down_hold_ms
//    after the previous one (to20_hold_ms when the 30 before this 60 was full, see Target20);
//  - 30 -> 20 ("to20") only while C > cap20_ms for to20_dwell_s: presents held to20_hold_ms;
//  - 20 -> 30 ("rel20") while C <= cap30_ms for rel_dwell_s: rel_frames presents rel_ms after the
//    previous one instead of 50; failing if the game leaves that 30 for 20 within verify30_s;
//  - 30 -> 60 is the game's own; "rel60" (off by default, up_rel) releases presents up_rel_ms apart
//    while C <= cap_up_ms for up_dwell_s.
// A push ends when the game changes its mode, after its frames (gave up) or at a stall (aborted);
// a failed push rests its kind rest * 2^(n - 1) (at most rest_max) after its n-th failure in a row,
// never blocks, and a success resets that. rel20 and rel60 succeed only once their mode lasted
// verify30_s / verify60_s: until then a done one keeps its failures, and leaving the mode within
// that time (whatever took the game there) is its next failure, so a 20 <-> 30 round slows down.

enum class Push21 : u8 { None, Down, To20, Rel20, Rel60 };

constexpr std::string_view Push21Name(Push21 push) {
    switch (push) {
    case Push21::Down:
        return "down";
    case Push21::To20:
        return "to20";
    case Push21::Rel20:
        return "rel20";
    case Push21::Rel60:
        return "rel60";
    default:
        return "-";
    }
}

/// Why the controller does what it does at a present (the 10 s line's tally).
enum class Why : u8 {
    NoWindow,  ///< fewer than MinSamples frames since the mode change or the stall
    Stall,     ///< within stall_quiet_s of a stall
    ModeQuiet, ///< within the mode quiet time of the game's mode change
    Pushing,
    Fits60,    ///< at 60, C <= cap60: 60 fits
    Short60,   ///< at 60, cap60 < C < self60: down
    Self60,    ///< at 60, C >= self60: the game drops by itself
    Fits30,    ///< at 30, C <= cap20: 30 fits
    Fits60At30, ///< at 30, C <= cap_up with rel60 on: rel60
    Short30,   ///< at 30, C > cap20: to20
    Short30At20, ///< at 20, C > cap30: 20 stays
    Fits30At20,  ///< at 20, C <= cap30: rel20
    Resting,     ///< the push C asks for rests
    OtherMode,   ///< the game asks for another interval than 1-3
    Count,
};

constexpr std::string_view WhyName(Why why) {
    switch (why) {
    case Why::NoWindow:
        return "no window";
    case Why::Stall:
        return "stall";
    case Why::ModeQuiet:
        return "mode quiet";
    case Why::Pushing:
        return "pushing";
    case Why::Fits60:
        return "60 fits";
    case Why::Short60:
        return "60 short: down";
    case Why::Self60:
        return "60 short: the game drops";
    case Why::Fits30:
        return "30 fits";
    case Why::Fits60At30:
        return "60 fits: rel60";
    case Why::Short30:
        return "30 short: to20";
    case Why::Short30At20:
        return "30 short: 20 stays";
    case Why::Fits30At20:
        return "30 fits: rel20";
    case Why::Resting:
        return "resting";
    case Why::OtherMode:
        return "other mode";
    default:
        return "-";
    }
}

struct Params21 {
    bool auto_push = true;      ///< false: only the probe pushes
    double cap60_ms = 15.0;     ///< at 60, C above this: down
    double self60_ms = 34.0;    ///< at 60, C at or above this: no push (the game drops itself)
    double down_dwell_s = 0.5;
    double down_hold_ms = 37.0; ///< inside 30's window (34-41.7 took 60 to 30, 42 to 20)
    u32 down_frames = 40;
    double down_rest_s = 2.0;
    double down_rest_max_s = 30.0;
    double cap20_ms = 31.0;     ///< at 30, C above this: to20 (30 does not fit)
    double to20_dwell_s = 3.0;
    double to20_hold_ms = 42.0; ///< 42 took 30 and 60 to 20 every time
    u32 to20_frames = 10;
    double to20_rest_s = 10.0;
    double to20_rest_max_s = 60.0;
    u32 full30_escapes = 2;     ///< this many 30 -> 60 escapes within escape_s with C > cap30 ...
    double escape_s = 30.0;     ///< ... or one with C > cap20: the next down push takes 60 to 20
    double cap30_ms = 28.0;     ///< at 20, C at or below this: rel20 (30 fits)
    double rel_dwell_s = 2.0;
    double rel_ms = 30.0;       ///< a released present comes this long after the previous one
    u32 rel_frames = 6;         ///< presents released, then
    u32 rel_wait = 6;           ///< presents waited for the game's change
    double rel_rest_s = 3.0;
    double rel_rest_max_s = 60.0;
    double verify30_s = 10.0;   ///< the game leaving rel20's 30 for 20 this soon: rel20 failed
    bool up_rel = false;        ///< rel60 on
    double cap_up_ms = 13.0;    ///< at 30, C at or below this: rel60
    double up_dwell_s = 5.0;
    double up_rel_ms = 14.0;
    double up_rest_s = 20.0;
    double up_rest_max_s = 160.0;
    double verify60_s = 10.0;   ///< the game leaving rel60's 60 this soon: rel60 failed
    double stall_ms = 100.0;    ///< work above this: a stall (not in the window, ends a push)
    double stall_quiet_s = 2.0;
    double mode_quiet_s = 1.0;
    double mode_quiet60_s = 0.5;
};

/// EDEN_NX2_MODE_PACE_PROBE with =2: <down|to20|rel20|rel60|cycle>:<1-3600 s> (cycle: down at 60,
/// rel20 at 20).
struct Probe21 {
    Push21 kind{Push21::None};
    bool cycle{};
    double every_s{};
};

inline std::optional<Probe21> ParseProbe21(std::string_view text) {
    text = Trim(text);
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view kind = Trim(text.substr(0, colon));
    Probe21 probe{};
    if (kind == "down") {
        probe.kind = Push21::Down;
    } else if (kind == "to20") {
        probe.kind = Push21::To20;
    } else if (kind == "rel20") {
        probe.kind = Push21::Rel20;
    } else if (kind == "rel60") {
        probe.kind = Push21::Rel60;
    } else if (kind == "cycle") {
        probe.cycle = true;
    } else {
        return std::nullopt;
    }
    const std::string_view number = Trim(text.substr(colon + 1));
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

struct Output21 {
    double hold_ms{};    ///< the hold of the next present in the push's mode (0: none)
    double release_ms{}; ///< > 0: the next present at most this long after this one
    Push21 started{Push21::None};
    bool by_probe{};
    bool down_to20{};  ///< the started down push holds to20_hold_ms (60 -> 20)
    Push21 ended{Push21::None};
    Result result{Result::None};
    u32 frames{};
    /// A rel20 / rel60 push that ended done whose mode the game left within its verify time.
    Push21 unverified{Push21::None};
    double rest_s{}; ///< at a failed end: the rest before the next push of its kind
    u32 fails{};     ///< failures in a row of that kind
    bool mode_changed{};
    u32 old_mode{};
    Push21 by{Push21::None};
    double change_cap_ms{NAN};
    double cap_ms{NAN}; ///< NaN: fewer than MinSamples frames in the window
    double work_med_ms{NAN};
    double busy_med_ms{NAN};
    Why why{Why::NoWindow};
};

class Controller21 {
public:
    static constexpr std::size_t Window = 16;
    static constexpr std::size_t MinSamples = 8;

    explicit Controller21(const Params21& params_ = {}, std::optional<Probe21> probe_ = {})
        : params{params_}, probe{probe_} {}

    Output21 Step(const Input& in) {
        Output21 out{};
        const double dt =
            std::clamp(std::isfinite(in.dt_ms) ? in.dt_ms : 0.0, 0.0, 1000.0) / 1000.0;
        clock += dt;
        since_stall += dt;
        stint += dt;
        since_probe += dt;
        for (Backoff& b : backoff) {
            b.rest = (std::max)(b.rest - dt, 0.0);
        }
        // A released mode that lasted until this present (Verify forgets one the game left): its
        // push worked.
        if (rel20_done_at != Never && clock - rel20_done_at >= params.verify30_s) {
            Of(Push21::Rel20) = {};
            rel20_done_at = Never;
        }
        if (rel60_done_at != Never && clock - rel60_done_at >= params.verify60_s) {
            Of(Push21::Rel60) = {};
            rel60_done_at = Never;
        }

        // 1. The game changed its mode.
        if (in.mode != mode) {
            out.change_cap_ms = Cap();
            const Push21 by = push;
            if (push != Push21::None) {
                End(out, ResultOnMode(push, in.mode));
            }
            Verify(out, in.mode);
            if (mode == 2 && in.mode == 1) {
                // An escape from 30: how full that 30 was decides where the next down push goes.
                escapes[escape_next++ % escapes.size()] = {clock, out.change_cap_ms};
            }
            out.mode_changed = true;
            out.old_mode = mode;
            out.by = by;
            mode = in.mode;
            count = 0;
            next = 0;
            stint = 0.0;
            cond = 0.0;
            cond_kind = Push21::None;
        }

        // 2. A stall (loading, shader build): out of the window, ends a push.
        if (in.work_ms > params.stall_ms) {
            since_stall = 0.0;
            if (push != Push21::None) {
                End(out, Result::Aborted);
            }
            cond = 0.0;
            cond_kind = Push21::None;
            out.why = Why::Stall;
            Fill(out);
            return out;
        }

        // 3. The window.
        work[next] = static_cast<float>((std::max)(in.work_ms, 0.0));
        busy[next] = static_cast<float>((std::max)(in.busy_ms, 0.0));
        next = (next + 1) % Window;
        count = (std::min)(count + 1, Window);

        // 4. A push goes on until its frames are spent.
        if (push != Push21::None) {
            ++frames;
            if (frames >= PushFrames(push)) {
                End(out, Result::GaveUp);
            } else {
                out.why = Why::Pushing;
                Fill(out);
                return out;
            }
        }

        // 5. Decisions.
        const double c = Cap();
        const double quiet = mode == 1 ? params.mode_quiet60_s : params.mode_quiet_s;
        Push21 want = Push21::None;
        double dwell = 0.0;
        if (mode < 1 || mode > 3) {
            out.why = Why::OtherMode;
        } else if (std::isnan(c)) {
            out.why = Why::NoWindow;
        } else if (since_stall < params.stall_quiet_s) {
            out.why = Why::Stall;
        } else if (stint < quiet) {
            out.why = Why::ModeQuiet;
        } else if (mode == 1) {
            if (c <= params.cap60_ms) {
                out.why = Why::Fits60;
            } else if (c >= params.self60_ms) {
                out.why = Why::Self60;
            } else {
                out.why = Why::Short60;
                want = Push21::Down;
                dwell = params.down_dwell_s;
            }
        } else if (mode == 2) {
            if (c > params.cap20_ms) {
                out.why = Why::Short30;
                want = Push21::To20;
                dwell = params.to20_dwell_s;
            } else if (params.up_rel && c <= params.cap_up_ms) {
                out.why = Why::Fits60At30;
                want = Push21::Rel60;
                dwell = params.up_dwell_s;
            } else {
                out.why = Why::Fits30;
            }
        } else {
            if (c <= params.cap30_ms) {
                out.why = Why::Fits30At20;
                want = Push21::Rel20;
                dwell = params.rel_dwell_s;
            } else {
                out.why = Why::Short30At20;
            }
        }
        if (want != Push21::None && !Of(want).Ready()) {
            out.why = Why::Resting;
            want = Push21::None;
        }
        if (want == Push21::None || !params.auto_push) {
            cond = 0.0;
            cond_kind = Push21::None;
        } else {
            cond = want == cond_kind ? cond + dt : dt;
            cond_kind = want;
            if (cond >= dwell) {
                Start(out, want, false);
            }
        }

        // 6. The probe: whatever C, the rests and the dwells.
        if (probe && push == Push21::None && since_stall >= params.stall_quiet_s &&
            since_probe >= probe->every_s) {
            Push21 kind = Push21::None;
            if (probe->cycle) {
                kind = mode == 1 ? Push21::Down : mode == 3 ? Push21::Rel20 : Push21::None;
            } else if (Allowed(probe->kind, mode)) {
                kind = probe->kind;
            }
            if (kind != Push21::None) {
                Start(out, kind, true);
                since_probe = 0.0;
            }
        }
        Fill(out);
        return out;
    }

    /// The hold of a present the game made in `game_mode`: none once the game left the mode the
    /// push started in.
    double HoldMs(u32 game_mode) const {
        return push != Push21::None && game_mode == push_from ? hold : 0.0;
    }
    /// The longest time from the present `game_mode` was just made in to the next one (0: no
    /// release).
    double ReleaseMs(u32 game_mode) const {
        return push != Push21::None && game_mode == push_from ? release : 0.0;
    }
    Push21 Pushing() const {
        return push;
    }
    u32 Fails(Push21 kind) const {
        return Of(kind).fails;
    }
    double Rest(Push21 kind) const {
        return Of(kind).rest;
    }
    double Clock() const {
        return clock;
    }

private:
    struct Backoff {
        u32 fails{};
        double rest{};

        bool Ready() const {
            return rest == 0.0;
        }
        void Fail(double base, double max) {
            ++fails;
            const u32 n = (std::min)(fails, 30U);
            rest = (std::min)(base * std::ldexp(1.0, static_cast<int>(n) - 1), max);
        }
    };
    struct Escape {
        double at{-1.0};
        double cap{NAN};
    };

    static constexpr bool Allowed(Push21 kind, u32 mode) {
        switch (kind) {
        case Push21::Down:
            return mode == 1;
        case Push21::To20:
        case Push21::Rel60:
            return mode == 2;
        case Push21::Rel20:
            return mode == 3;
        default:
            return false;
        }
    }

    static Result ResultOnMode(Push21 kind, u32 new_mode) {
        switch (kind) {
        case Push21::Down:
            return new_mode == 2 ? Result::Done : new_mode == 3 ? Result::Done20 : Result::Other;
        case Push21::To20:
            return new_mode == 3 ? Result::Done : Result::Other;
        case Push21::Rel20:
            // 20 -> 60 is up as well; Verify still watches that it keeps out of 20.
            return new_mode == 2 || new_mode == 1 ? Result::Done : Result::Other;
        case Push21::Rel60:
            return new_mode == 1 ? Result::Done : Result::Other;
        default:
            return Result::None;
        }
    }

    Backoff& Of(Push21 kind) {
        return backoff[static_cast<std::size_t>(kind)];
    }
    const Backoff& Of(Push21 kind) const {
        return backoff[static_cast<std::size_t>(kind)];
    }

    u32 PushFrames(Push21 kind) const {
        switch (kind) {
        case Push21::Down:
            return params.down_frames;
        case Push21::To20:
            return params.to20_frames;
        case Push21::Rel20:
        case Push21::Rel60:
            return params.rel_frames + params.rel_wait;
        default:
            return 0;
        }
    }

    double Cap() const {
        if (count < MinSamples) {
            return NAN;
        }
        return (std::max)(Median({work.data(), count}), Median({busy.data(), count}));
    }

    /// The down push takes 60 to 20 when the 30 before it was full: one escape from 30 with C
    /// above cap20, or full30_escapes within escape_s with C above cap30.
    bool Target20() const {
        u32 full = 0;
        for (const Escape& e : escapes) {
            if (e.at < 0.0 || clock - e.at > params.escape_s || std::isnan(e.cap)) {
                continue;
            }
            if (e.cap > params.cap20_ms) {
                return true;
            }
            full += e.cap > params.cap30_ms ? 1 : 0;
        }
        return full >= params.full30_escapes;
    }

    /// The game changed its mode (by itself or by a push): a released mode it did not keep for its
    /// verify time fails its push. rel20's 30 is kept while the game stays out of 20 (a 30 -> 60
    /// escape followed by 20 is no 30 that lasted).
    void Verify(Output21& out, u32 new_mode) {
        if (new_mode == 3 && rel20_done_at != Never &&
            clock - rel20_done_at < params.verify30_s) {
            Fail(out, Push21::Rel20);
            out.unverified = Push21::Rel20;
        }
        if (new_mode == 3) {
            rel20_done_at = Never;
        }
        if (mode == 1 && rel60_done_at != Never && clock - rel60_done_at < params.verify60_s) {
            Fail(out, Push21::Rel60);
            out.unverified = Push21::Rel60;
        }
        if (mode == 1) {
            rel60_done_at = Never;
        }
    }

    void Fail(Output21& out, Push21 kind) {
        Backoff& b = Of(kind);
        switch (kind) {
        case Push21::Down:
            b.Fail(params.down_rest_s, params.down_rest_max_s);
            break;
        case Push21::To20:
            b.Fail(params.to20_rest_s, params.to20_rest_max_s);
            break;
        case Push21::Rel20:
            b.Fail(params.rel_rest_s, params.rel_rest_max_s);
            break;
        case Push21::Rel60:
            b.Fail(params.up_rest_s, params.up_rest_max_s);
            break;
        default:
            return;
        }
        out.rest_s = b.rest;
        out.fails = b.fails;
    }

    double CurrentHold() const {
        switch (push) {
        case Push21::Down:
            return push_to20 ? params.to20_hold_ms : params.down_hold_ms;
        case Push21::To20:
            return params.to20_hold_ms;
        default:
            return 0.0;
        }
    }
    double CurrentRelease() const {
        if (frames >= params.rel_frames) {
            return 0.0;
        }
        switch (push) {
        case Push21::Rel20:
            return params.rel_ms;
        case Push21::Rel60:
            return params.up_rel_ms;
        default:
            return 0.0;
        }
    }

    void Start(Output21& out, Push21 kind, bool by_probe) {
        push = kind;
        push_from = mode;
        push_probe = by_probe;
        push_to20 = kind == Push21::Down && Target20();
        frames = 0;
        cond = 0.0;
        cond_kind = Push21::None;
        out.started = kind;
        out.by_probe = by_probe;
        out.down_to20 = push_to20;
        out.why = Why::Pushing;
    }

    void End(Output21& out, Result result) {
        out.ended = push;
        out.result = result;
        out.frames = frames;
        const bool done = result == Result::Done || result == Result::Done20;
        const bool released = push == Push21::Rel20 || push == Push21::Rel60;
        if (!push_probe) {
            if (!done) {
                Fail(out, push);
            } else if (released) {
                // Its mode has yet to last (Verify): the failures stay until it does.
                Of(push).rest = 0.0;
            } else {
                Of(push) = {};
            }
        }
        if (result == Result::Done && push == Push21::Rel20) {
            rel20_done_at = clock;
        }
        if (result == Result::Done && push == Push21::Rel60) {
            rel60_done_at = clock;
        }
        if (done && (push == Push21::To20 || push_to20)) {
            // A 20 Eden chose: the escapes that chose it are spent.
            escapes = {};
        }
        push = Push21::None;
        push_to20 = false;
        frames = 0;
        since_probe = 0.0;
    }

    void Fill(Output21& out) {
        const bool on = push != Push21::None && mode == push_from;
        hold = on ? CurrentHold() : 0.0;
        release = on ? CurrentRelease() : 0.0;
        out.hold_ms = hold;
        out.release_ms = release;
        if (count >= MinSamples) {
            out.work_med_ms = Median({work.data(), count});
            out.busy_med_ms = Median({busy.data(), count});
            out.cap_ms = (std::max)(out.work_med_ms, out.busy_med_ms);
        }
    }

    Params21 params;
    std::optional<Probe21> probe;

    u32 mode{}; ///< 0 before the first present: the first one counts as a change
    double clock{};
    double since_stall{1e9};
    double stint{};
    double since_probe{};
    static constexpr double Never = -1e9;
    double rel20_done_at{Never}; ///< Never: no done rel20 whose 30 has yet to last
    double rel60_done_at{Never};
    std::array<Escape, 4> escapes{};
    std::size_t escape_next{};

    std::array<float, Window> work{};
    std::array<float, Window> busy{};
    std::size_t count{};
    std::size_t next{};

    double cond{};
    Push21 cond_kind{Push21::None};

    Push21 push{Push21::None};
    u32 push_from{};
    bool push_probe{};
    bool push_to20{};
    u32 frames{};
    double hold{};
    double release{};

    std::array<Backoff, 5> backoff{}; ///< by Push21
};

} // namespace VideoCommon::Nx2ModePace
