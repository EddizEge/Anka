// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <bitset>

#include "common/common_types.h"
#include "video_core/nx2_stages.h"

// Back stages (nx2_stages.h): what keeps a stage's replica of Maxwell3D equal to the primary A
// runs. Header-only, without Maxwell3D itself, so the unit tests take it alone. Owner: T2.
namespace VideoCommon::Nx2RegReplica {

/// Maxwell3D::Regs::NUM_REGS (asserted where both are visible, T2).
inline constexpr u32 NumRegs = 0xE00;
/// Maxwell3D::DirtyState::Flags and ::Tables.
using DirtyFlags = std::bitset<256>;
using DirtyTables = std::array<std::array<u8, NumRegs>, 2>;

/// A delta word:
///   bits 0-15  the register (method)
///   bit 16     Dirty: A's write set the register's dirty flags (ProcessDirtyRegisters)
///   bit 17     Changed: A's write was a light change (another value, or an executable method)
///   bits 18-31 zero
///   bits 32-63 the value (the last one A wrote)
inline constexpr u64 DeltaDirty = u64{1} << 16;
inline constexpr u64 DeltaChanged = u64{1} << 17;

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
class RegCapture {
public:
    /// ProcessDirtyRegisters' write.
    void Note(u32 method, u32 value, bool changed, bool executable) {}
    /// Sends what was captured as one Call record applying it to the replica, and starts over
    /// (Nx2Stages::FlushBeforeForward).
    void FlushTo(Nx2Stages::Ring ring) {}
};

/// Applies delta words to a replica's registers (B and C; T2): writes each value, sets the
/// register's dirty flags with both tables when Dirty or the replica held another value, and calls
/// `note_light(method)` when Changed or the replica held another value (the caller notes the light
/// change when LightTrackingEnabled). No DrawDeferred, no method runs.
template <typename NoteLight>
void ApplyDelta(u32* regs, DirtyFlags& flags, const DirtyTables& tables, const u64* words,
                u32 count, NoteLight&& note_light) {}

/// Side state (D4): what a draw reads of Maxwell3D outside its registers, sent when it changes or
/// with a draw (T5). Wire format: a header word (bits 0-15 the parts, bits 32-63 the words after
/// it), then the parts in the order of their bits; each part's layout is T2's.
enum SidePart : u32 {
    SideConstBuffers = 1 << 0,  ///< state.shader_stages[].const_buffers (binds)
    SideEngineState = 1 << 1,   ///< engine_state
    SideReplaceTable = 1 << 2,  ///< replace_table
    SideExecuteOn = 1 << 3,     ///< execute_on
    SideDrawState = 1 << 4,     ///< draw_manager.draw_state with its inline index data
    SideIndirectState = 1 << 5, ///< draw_manager.indirect_state
    SideDirtyFlags = 1 << 6,    ///< dirty.flags set outside ProcessDirtyRegisters: ORed in
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
