// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bitset>
#include <cstdio>
#include <cstdlib>

#include "common/common_types.h"
#include "video_core/nx2_stage_ring.h"
#include "video_core/nx2_stages.h"

namespace Tegra::Engines {
class Maxwell3D;
}

// Back stages (nx2_stages.h): what keeps a stage's replica of Maxwell3D equal to the primary A
// runs. Header-only, without Maxwell3D itself, so the unit tests take it alone. Owner: T2.
namespace VideoCommon::Nx2RegReplica {

/// Maxwell3D::Regs::NUM_REGS (asserted where both are visible, T2).
inline constexpr u32 NumRegs = 0xE00;
static_assert(NumRegs <= Nx2Stages::MaxDeltaWords, "a flush of the capture fits one record (K9)");
/// Maxwell3D::DirtyState::Flags and ::Tables.
using DirtyFlags = std::bitset<256>;
using DirtyTables = std::array<std::array<u8, NumRegs>, 2>;

/// A delta word (in a record after its header, nx2_stage_ring.h):
///   bits 0-15  the register (method)
///   bit 16     Dirty: A's write set the register's dirty flags (ProcessDirtyRegisters)
///   bit 17     Changed: A's write was a light change (another value, or an executable method)
///   bits 18-31 zero
///   bits 32-63 the value (the last one A wrote)
inline constexpr u64 DeltaDirty = u64{1} << 16;
inline constexpr u64 DeltaChanged = u64{1} << 17;

/// MAXWELL3D_REG_INDEX(const_buffer.buffer): the 16 constant buffer data registers from here on
/// (asserted in maxwell_3d.cpp).
inline constexpr u32 CbDataMethod = 0x8e4;
inline constexpr u32 CbDataCount = 16;

constexpr u64 MakeDelta(u32 method, u32 value, bool dirty, bool changed) {
    return (method & 0xffff) | (dirty ? DeltaDirty : 0) | (changed ? DeltaChanged : 0) |
           (u64{value} << 32);
}
constexpr u32 DeltaMethod(u64 word) {
    return static_cast<u32>(word & 0xffff);
}
constexpr u32 DeltaValue(u64 word) {
    return static_cast<u32>(word >> 32);
}

/// A's register capture: the Maxwell3D writes since the last record A sent, one word per register
/// (its last value, Dirty and Changed ORed over its writes), in the order of their first writes.
/// Maxwell3D's stage_capture points at it while staged (Nx2Stages::Capture, nx2_stage_api.h);
/// Maxwell3D's hooks test stage_capture, not Nx2Stages::Active() (K1: =3 stages no channel).
class RegCapture {
public:
    /// ProcessDirtyRegisters' write, made after its DrawDeferred (S18). Writes
    /// ProcessDirtyRegisters does not make (shadow_scratch, ...) are noted with dirty false; not
    /// const_buffer.offset's advance by cb data: StageCbWrite makes it on the replica (K6).
    /// The constant buffer data registers (const_buffer.buffer[0..15]) are left out but at =2
    /// (S5/4: no stage reads them). A write to another engine than Source() first
    /// SelectTarget(engine).
    /// Inside a join (Nx2Stages::InJoin()) it only collects, no record can go (K4): a write to
    /// another engine there is an error (no Maxwell3D method runs in a join but
    /// ProcessQueryCondition's, which writes execute_on alone).
    void Note(Tegra::Engines::Maxwell3D& engine, u32 method, u32 value, bool dirty, bool changed) {
        if (method - CbDataMethod < CbDataCount && !keep_cb_data) {
            return;
        }
        if (&engine != source) [[unlikely]] {
            if (Nx2Stages::InJoin()) [[unlikely]] {
                // A join sends nothing: the Target record could not go before these words.
                std::fputs("Nx2RegReplica: a write to another Maxwell3D inside a join (K4)\n",
                           stderr);
                std::abort();
            }
            SelectTarget(engine);
        }
        const u64 bits = (dirty ? DeltaDirty : 0) | (changed ? DeltaChanged : 0);
        u16& slot = slots[method];
        if (slot == 0) {
            words[count] = MakeDelta(method, value, false, false) | bits;
            slot = static_cast<u16>(++count);
            return;
        }
        // Written again since the last record: its last value, Dirty and Changed ORed (an A -> B
        // -> A write still sets the dirty flags and notes the light change it made).
        u64& word = words[slot - 1];
        word = (word & (0xffff | DeltaDirty | DeltaChanged)) | bits | (u64{value} << 32);
    }
    /// The engine A's next writes and records are for: when another than Source(), sends
    /// Nx2Stages::PostTarget(Ring::ToB, &engine) (what is pending goes there, for the old one) and
    /// makes it Source(). Note calls it, and Nx2Stages::PostCbData (K6).
    void SelectTarget(Tegra::Engines::Maxwell3D& engine) {
        if (&engine == source) {
            return;
        }
        // The Target record takes the words pending (Nx2Stages::Reserve), for the old target.
        Nx2Stages::PostTarget(Nx2Stages::Ring::ToB, &engine);
        source = &engine;
    }

    /// Delta words pending (each record from A takes them, Nx2Stages::Reserve).
    [[nodiscard]] u32 PendingWords() const {
        return count;
    }
    /// The engine they were written to.
    [[nodiscard]] Tegra::Engines::Maxwell3D* Source() const {
        return source;
    }
    /// Writes the PendingWords() words at `out` and starts over.
    void TakeWords(u64* out) {
        for (u32 i = 0; i < count; ++i) {
            out[i] = words[i];
            slots[DeltaMethod(words[i])] = 0;
        }
        count = 0;
    }
    /// Drops everything (Start, Stop).
    void Reset() {
        for (u32 i = 0; i < count; ++i) {
            slots[DeltaMethod(words[i])] = 0;
        }
        count = 0;
        source = nullptr;
        keep_cb_data = Nx2Stages::Get().mode == 2;
    }

private:
    /// Per register: its word's index plus 1 (0: not written since the last record). The written
    /// set and the first write order in one (one load per write).
    std::array<u16, NumRegs> slots{};
    std::array<u64, NumRegs> words{};
    u32 count{};
    Tegra::Engines::Maxwell3D* source{};
    bool keep_cb_data{}; ///< =2: the constant buffer data registers too (VerifyReplica)
};

/// Applies delta words to a replica's registers (B and C; T2): writes each value, sets the
/// register's dirty flags with both tables when Dirty or the replica held another value, and calls
/// `note_light(method)` when Changed or the replica held another value (the caller notes the light
/// change when LightTrackingEnabled). No DrawDeferred, no method runs.
template <typename NoteLight>
void ApplyDelta(u32* regs, DirtyFlags& flags, const DirtyTables& tables, const u64* words,
                u32 count, NoteLight&& note_light) {
    for (u32 i = 0; i < count; ++i) {
        const u64 word = words[i];
        const u32 method = DeltaMethod(word);
        const u32 value = DeltaValue(word);
        const bool differs = regs[method] != value;
        regs[method] = value;
        if ((word & DeltaDirty) != 0 || differs) {
            flags[tables[0][method]] = true;
            flags[tables[1][method]] = true;
        }
        if ((word & DeltaChanged) != 0 || differs) {
            note_light(method);
        }
    }
}

/// Side state (D4): what a draw reads of Maxwell3D outside its registers. A sends the parts that
/// changed since they were last sent (generation counters, S5/2) before the draw reading them
/// (T5, Nx2Stages::WriteSideState). Wire format: a header word (bits 0-15 the parts, bits 32-63
/// the words after it), then the parts in the order of their bits; each part's layout is T2's.
enum SidePart : u32 {
    SideConstBuffers = 1 << 0,  ///< state.shader_stages[].const_buffers (binds)
    SideRebound = 1 << 1,       ///< light.rebound set since the last send: ORed in (S2)
    SideEngineState = 1 << 2,   ///< engine_state
    SideReplaceTable = 1 << 3,  ///< replace_table
    SideExecuteOn = 1 << 4,     ///< execute_on
    SideDrawState = 1 << 5,     ///< draw_manager.draw_state with its inline index data
    SideIndirectState = 1 << 6, ///< draw_manager.indirect_state
    SideDrawTexture = 1 << 7,   ///< draw_manager.draw_texture_state (S18)
    SideDirtyFlags = 1 << 8,    ///< dirty.flags set outside ProcessDirtyRegisters: ORed in
};

constexpr u64 MakeSideHeader(u32 parts, u32 words) {
    return (parts & 0xffff) | (u64{words} << 32);
}
constexpr u32 SideParts(u64 header) {
    return static_cast<u32>(header & 0xffff);
}
constexpr u32 SideWords(u64 header) {
    return static_cast<u32>(header >> 32);
}

} // namespace VideoCommon::Nx2RegReplica
