// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_profile.h"
#include "video_core/rasterizer_interface.h"

// EDEN_NX2_GPU_PROFILE: how each NVN indexed indirect draw (Maxwell3D::BackDrawIndexedIndirect)
// differs from the one before it in its vertex streams, attribute formats, index buffer and the
// rest of the registers. Draws whose streams only start elsewhere in the same buffers, all by the
// same whole number of vertices, could share one binding with the difference added to each
// record's base vertex. Observes only; every 10 s logs one line. EDEN_NX2_INDIRECT_DIAG=0 turns
// it off (it copies and compares the register file per draw).
namespace VideoCommon::Nx2IndirectDiag {

using Maxwell3D = Tegra::Engines::Maxwell3D;
using Regs = Maxwell3D::Regs;

inline constexpr u32 NumStreams = static_cast<u32>(Regs::NumVertexArrays);
inline constexpr u32 IndexSlot = 32; ///< RasterizerInterface::BoundGeometryBuffer's index buffer
inline constexpr u32 StreamBase = offsetof(Regs, vertex_streams) / sizeof(u32);
inline constexpr u32 LimitBase = offsetof(Regs, vertex_stream_limits) / sizeof(u32);
inline constexpr u32 NativeSizeBase = 0x600 / sizeof(u32); ///< Regs::NativeVertexStreamSize
inline constexpr u32 FormatBase = offsetof(Regs, vertex_attrib_format) / sizeof(u32);
inline constexpr u32 IndexBase = offsetof(Regs, index_buffer) / sizeof(u32);
inline constexpr u32 InstancedBase = offsetof(Regs, vertex_stream_instances) / sizeof(u32);

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_INDIRECT_DIAG");
        return Nx2Profile::Enabled() && (value == nullptr || value[0] != '0');
    }();
    return enabled;
}

/// Values seen and how often.
struct Tally {
    std::array<std::pair<s64, u64>, 32> slots{};
    u32 used{};
    u64 dropped{}; ///< Notes of values that found no free slot

    void Note(s64 value) {
        for (u32 i = 0; i < used; ++i) {
            if (slots[i].first == value) {
                ++slots[i].second;
                return;
            }
        }
        if (used < slots.size()) {
            slots[used++] = {value, 1};
        } else {
            ++dropped;
        }
    }

    /// The four most frequent, "value xN, ...".
    std::string Top(bool hex) const {
        auto sorted = slots;
        const auto end = sorted.begin() + used;
        std::sort(sorted.begin(), end,
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string text;
        for (auto it = sorted.begin(); it != end && it != sorted.begin() + 4; ++it) {
            text += fmt::format("{}{} x{}", text.empty() ? "" : ", ",
                                hex ? fmt::format("{:#x}", it->first)
                                    : fmt::format("{}", it->first),
                                it->second);
        }
        if (used > 4 || dropped != 0) {
            text += ", ...";
        }
        return text.empty() ? "-" : text;
    }
};

struct Window {
    u64 calls{};
    u64 records{};
    u64 compared{}; ///< Calls with a previous call on the same engine
    u64 topology_same{};
    u64 stream_set_same{};
    u64 stream_pairs{}; ///< Streams enabled in both calls
    u64 address_same{};
    u64 moved{};
    u64 moved_whole{}; ///< Of moved: by a multiple of the stride
    u64 moved_up{};    ///< Of moved: to a higher address
    u64 moved_same_buffer{};
    u64 end_same{};
    u64 config_same{}; ///< Stride, enable and frequency
    u64 buffer_same{};
    u64 calls_moved{};    ///< Calls with a moved stream
    u64 one_delta{};      ///< Of calls_moved: every per-vertex stream by the same whole vertices
    u64 formats_same{};
    u64 index_address_same{};
    u64 index_format_same{};
    u64 index_limit_same{};
    u64 index_count_same{};
    u64 index_buffer_same{};
    u64 other_differ{}; ///< Calls where another register differs
    u64 other_registers{};
    u64 runs{};
    u64 run_calls{};
    u64 strict_runs{};
    u64 strict_run_calls{};
    Tally steps;         ///< Address difference of moved streams, bytes
    Tally vertex_deltas; ///< Of one_delta: the difference in vertices
    Tally other;         ///< Other registers that differed (method index)
    Tally run_lengths;
    Tally strict_run_lengths;
};

struct State {
    const Maxwell3D* engine{};
    u32 topology{};
    u32 index_count{};
    std::vector<u32> regs;
    std::array<u32, IndexSlot + 1> buffers{};
    u32 run{};
    u32 strict_run{};
    u64 window_start{};
    Window window;
    /// Registers left out of the "other registers" comparison: the stream addresses and
    /// limits, the index buffer, and what NVN's indirect draw macro writes and resets.
    std::vector<bool> ignored;
};

inline thread_local std::unique_ptr<State> t_state;

struct Stream {
    u32 config;
    u32 frequency;
    GPUVAddr address;
    GPUVAddr end;
    u32 stride;
    bool enabled;
};

inline Stream ReadStream(const u32* r, u32 i, bool native) {
    const u32 base = StreamBase + i * 4;
    Stream stream{};
    stream.config = r[base];
    stream.address = (GPUVAddr{r[base + 1]} << 32) | r[base + 2];
    stream.frequency = r[base + 3];
    stream.stride = stream.config & 0xfff;
    stream.enabled = ((stream.config >> 12) & 1) != 0 && stream.address != 0;
    if (native) {
        const u32 size = NativeSizeBase + i * 2;
        stream.end = stream.address + ((u64{r[size] & 0xffU} << 32) | r[size + 1]);
    } else {
        const u32 limit = LimitBase + i * 2;
        stream.end = ((GPUVAddr{r[limit]} << 32) | r[limit + 1]) + 1;
    }
    return stream;
}

inline std::vector<bool> IgnoredRegisters() {
    std::vector<bool> ignored(Regs::NUM_REGS, false);
    const auto ignore = [&](u32 first, u32 count) {
        for (u32 i = first; i < first + count; ++i) {
            ignored[i] = true;
        }
    };
    for (u32 i = 0; i < NumStreams; ++i) {
        ignore(StreamBase + i * 4 + 1, 2);
    }
    ignore(LimitBase, NumStreams * 2);
    ignore(NativeSizeBase, NumStreams * 2);
    ignore(IndexBase, 7);
    for (const u32 method : {0x446U, 0x50dU, 0x50eU, 0x585U, 0x586U}) {
        ignore(method, 1);
    }
    ignore(0x8e3, 17); // const_buffer offset and data
    return ignored;
}

inline void EndRun(u32& run, u64& runs, u64& calls, Tally& lengths) {
    if (run != 0) {
        ++runs;
        calls += run;
        lengths.Note(run);
    }
    run = 0;
}

inline double Percent(u64 part, u64 whole) {
    return whole == 0 ? 0.0 : static_cast<double>(part) * 100.0 / static_cast<double>(whole);
}

inline void Report(State& s) {
    const u64 now = Nx2Profile::Now();
    if (s.window_start == 0) {
        s.window_start = now;
        return;
    }
    if (now - s.window_start < 10'000'000'000ULL) {
        return;
    }
    const Window& w = s.window;
    const auto mean = [](u64 calls, u64 runs) {
        return runs == 0 ? 0.0 : static_cast<double>(calls) / static_cast<double>(runs);
    };
    std::string line = fmt::format(
        "{} calls ({} records), {} compared with the previous call, topology same {:.1f}%, "
        "stream set same {:.1f}%",
        w.calls, w.records, w.compared, Percent(w.topology_same, w.compared),
        Percent(w.stream_set_same, w.compared));
    line += fmt::format(
        " | stream pairs {}: address same {:.1f}%, moved {:.1f}% (by whole strides {:.1f}%, up "
        "{:.1f}%, in the same buffer {:.1f}%; steps: {}), end same {:.1f}%, config same {:.1f}%, "
        "same buffer {:.1f}%",
        w.stream_pairs, Percent(w.address_same, w.stream_pairs), Percent(w.moved, w.stream_pairs),
        Percent(w.moved_whole, w.moved), Percent(w.moved_up, w.moved),
        Percent(w.moved_same_buffer, w.moved), w.steps.Top(false),
        Percent(w.end_same, w.stream_pairs), Percent(w.config_same, w.stream_pairs),
        Percent(w.buffer_same, w.stream_pairs));
    line += fmt::format(" | calls with moved streams {}: one vertex delta {:.1f}% (deltas: {})",
                        w.calls_moved, Percent(w.one_delta, w.calls_moved),
                        w.vertex_deltas.Top(false));
    line += fmt::format(" | attribute formats same {:.1f}%", Percent(w.formats_same, w.compared));
    line += fmt::format(" | index buffer: address same {:.1f}%, format same {:.1f}%, limit same "
                        "{:.1f}%, count same {:.1f}%, same buffer {:.1f}%",
                        Percent(w.index_address_same, w.compared),
                        Percent(w.index_format_same, w.compared),
                        Percent(w.index_limit_same, w.compared),
                        Percent(w.index_count_same, w.compared),
                        Percent(w.index_buffer_same, w.compared));
    line += fmt::format(" | other registers differ {:.1f}% ({:.1f} per call; {})",
                        Percent(w.other_differ, w.compared), mean(w.other_registers, w.compared),
                        w.other.Top(true));
    line += fmt::format(" | runs {} (mean {:.2f}; lengths: {}) | strict runs {} (mean {:.2f}; "
                        "lengths: {})",
                        w.runs, mean(w.run_calls, w.runs), w.run_lengths.Top(false),
                        w.strict_runs, mean(w.strict_run_calls, w.strict_runs),
                        w.strict_run_lengths.Top(false));
    LOG_INFO(HW_GPU, "Indirect draw layout in 10 s: {}", line);
    s.window = {};
    s.window_start = now;
}

/// After the draw: the buffer cache holds this call's bindings.
inline void Note(Maxwell3D& maxwell3d, u32 topology, GPUVAddr record, u32 count) {
    if (!Enabled()) {
        return;
    }
    if (!t_state) {
        t_state = std::make_unique<State>();
        t_state->ignored = IgnoredRegisters();
    }
    State& s = *t_state;
    Window& w = s.window;
    const u32* const cur = maxwell3d.regs.reg_array.data();
    const bool native = maxwell3d.GetNativeMacroEngine().native_turing;
    auto& rasterizer = maxwell3d.Rasterizer();

    std::array<u32, IndexSlot + 1> buffers{};
    for (u32 i = 0; i < NumStreams; ++i) {
        if (ReadStream(cur, i, native).enabled) {
            buffers[i] = rasterizer.BoundGeometryBuffer(i);
        }
    }
    buffers[IndexSlot] = rasterizer.BoundGeometryBuffer(IndexSlot);
    // The record's index count from guest memory (stale if the host GPU wrote the record).
    const u32 index_count = maxwell3d.GetMemoryManager().Read<u32>(record);

    ++w.calls;
    w.records += count;
    bool joins = false;
    bool strict = false;
    if (s.engine == &maxwell3d) {
        ++w.compared;
        const u32* const prev = s.regs.data();
        joins = true;
        if (topology == s.topology) {
            ++w.topology_same;
        } else {
            joins = false;
        }
        u32 cur_mask = 0;
        u32 prev_mask = 0;
        bool any_moved = false;
        bool one_delta = true;
        bool delta_known = false;
        s64 delta = 0;
        for (u32 i = 0; i < NumStreams; ++i) {
            const Stream c = ReadStream(cur, i, native);
            const Stream p = ReadStream(prev, i, native);
            cur_mask |= c.enabled ? 1U << i : 0;
            prev_mask |= p.enabled ? 1U << i : 0;
            if (!c.enabled || !p.enabled) {
                continue;
            }
            ++w.stream_pairs;
            const bool same_buffer = buffers[i] != 0 && buffers[i] == s.buffers[i];
            if (c.config == p.config && c.frequency == p.frequency) {
                ++w.config_same;
            } else {
                joins = false;
            }
            if (same_buffer) {
                ++w.buffer_same;
            } else {
                joins = false;
            }
            if (c.end == p.end) {
                ++w.end_same;
            }
            const s64 step = static_cast<s64>(c.address - p.address);
            const bool instanced = cur[InstancedBase + i] != 0;
            if (step == 0) {
                ++w.address_same;
            } else {
                any_moved = true;
                ++w.moved;
                w.steps.Note(step);
                if (step > 0) {
                    ++w.moved_up;
                }
                if (same_buffer) {
                    ++w.moved_same_buffer;
                }
                if (c.stride != 0 && step % c.stride == 0) {
                    ++w.moved_whole;
                } else {
                    one_delta = false;
                }
                if (instanced) {
                    // Would need its own first instance change.
                    joins = false;
                }
            }
            if (!instanced && one_delta) {
                const s64 vertices = step == 0 ? 0 : step / static_cast<s64>(c.stride);
                if (delta_known && vertices != delta) {
                    one_delta = false;
                }
                delta = vertices;
                delta_known = true;
            }
        }
        if (cur_mask == prev_mask) {
            ++w.stream_set_same;
        } else {
            joins = false;
        }
        if (any_moved) {
            ++w.calls_moved;
            if (one_delta) {
                ++w.one_delta;
                w.vertex_deltas.Note(delta);
            }
        }
        if (!one_delta) {
            joins = false;
        }
        if (std::memcmp(cur + FormatBase, prev + FormatBase,
                        Regs::NumVertexAttributes * sizeof(u32)) == 0) {
            ++w.formats_same;
        } else {
            joins = false;
        }
        const bool index_address = cur[IndexBase] == prev[IndexBase] &&
                                   cur[IndexBase + 1] == prev[IndexBase + 1];
        const bool index_format = cur[IndexBase + 4] == prev[IndexBase + 4];
        const bool index_buffer = buffers[IndexSlot] != 0 &&
                                  buffers[IndexSlot] == s.buffers[IndexSlot];
        w.index_address_same += index_address ? 1 : 0;
        w.index_format_same += index_format ? 1 : 0;
        w.index_limit_same += cur[IndexBase + 2] == prev[IndexBase + 2] &&
                                      cur[IndexBase + 3] == prev[IndexBase + 3]
                                  ? 1
                                  : 0;
        w.index_count_same += index_count == s.index_count ? 1 : 0;
        w.index_buffer_same += index_buffer ? 1 : 0;
        if (!index_address || !index_format || !index_buffer) {
            joins = false;
        }
        // Whole blocks first: most of the register file is the same from draw to draw.
        constexpr u32 Block = 64;
        u32 differing = 0;
        for (u32 block = 0; block < Regs::NUM_REGS; block += Block) {
            if (std::memcmp(cur + block, prev + block, Block * sizeof(u32)) == 0) {
                continue;
            }
            for (u32 i = block; i < block + Block; ++i) {
                if (cur[i] != prev[i] && !s.ignored[i]) {
                    ++differing;
                    w.other.Note(i);
                }
            }
        }
        w.other_registers += differing;
        if (differing != 0) {
            ++w.other_differ;
        }
        strict = joins && differing == 0;
    }
    if (joins) {
        ++s.run;
    } else {
        EndRun(s.run, w.runs, w.run_calls, w.run_lengths);
        s.run = 1;
    }
    if (strict) {
        ++s.strict_run;
    } else {
        EndRun(s.strict_run, w.strict_runs, w.strict_run_calls, w.strict_run_lengths);
        s.strict_run = 1;
    }

    s.engine = &maxwell3d;
    s.topology = topology;
    s.index_count = index_count;
    s.buffers = buffers;
    s.regs.assign(cur, cur + Regs::NUM_REGS);
    Report(s);
}

} // namespace VideoCommon::Nx2IndirectDiag
