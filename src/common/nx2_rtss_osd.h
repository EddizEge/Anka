// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "common/common_types.h"

// EDEN_NX2_OSD=1 (off by default): the game's frame rate, the display's and the game's speed in
// RivaTuner Statistics Server's on-screen display, the way MSI Afterburner puts its text there:
// an OSD slot in RTSS's shared memory "RTSSSharedMemoryV2" (RTSS SDK, RTSSSharedMemory.h and its
// RTSSSharedMemorySample). Only the fields used are laid out here, at the SDK's offsets. This
// part touches no OS API: it works on a block of memory (the mapped view, or a test's buffer);
// yuzu_cmd/nx2_osd.cpp maps the view and runs the timer thread.
namespace Common::Nx2RtssOsd {

/// EDEN_NX2_OSD's value: only "1..." turns it on.
constexpr bool Enabled(const char* value) {
    return value != nullptr && value[0] == '1';
}

/// The OSD slot's owner id ("EdenNX2"): a slot is ours if its owner reads this, free if empty.
constexpr std::string_view Owner = "EdenNX2";

/// 'RTSS' as the SDK's multi-character constant compares it: memory initialised, valid data.
constexpr u32 Signature = 0x52545353;
constexpr u32 Version2 = 0x00020000;        // v2.0: the slot array
constexpr u32 VersionTextEx = 0x00020007;   // v2.7: szOSDEx, 4096 characters
constexpr u32 VersionBusyLock = 0x0002000e; // v2.14: dwBusy

/// RTSS_SHARED_MEMORY's first fields (all DWORDs, then the LONG dwBusy).
struct Header {
    u32 signature;
    u32 version;
    u32 app_entry_size;
    u32 app_arr_offset;
    u32 app_arr_size;
    u32 osd_entry_size;
    u32 osd_arr_offset;
    u32 osd_arr_size;
    u32 osd_frame; // incremented to have RTSS redraw the OSD
    s32 busy;      // v2.14+: bit 0 set while a client writes (or the renderer reads)
};
static_assert(offsetof(Header, signature) == 0);
static_assert(offsetof(Header, version) == 4);
static_assert(offsetof(Header, osd_entry_size) == 20);
static_assert(offsetof(Header, osd_arr_offset) == 24);
static_assert(offsetof(Header, osd_arr_size) == 28);
static_assert(offsetof(Header, osd_frame) == 32);
static_assert(offsetof(Header, busy) == 36);
static_assert(sizeof(Header) == 40);

/// RTSS_SHARED_MEMORY_OSD_ENTRY's first fields; the rest (v2.12's buffer...) is not used.
struct OsdEntry {
    char text[256];    // szOSD
    char owner[256];   // szOSDOwner
    char text_ex[4096]; // szOSDEx, v2.7+
};
static_assert(offsetof(OsdEntry, text) == 0);
static_assert(offsetof(OsdEntry, owner) == 256);
static_assert(offsetof(OsdEntry, text_ex) == 512);
static_assert(sizeof(OsdEntry) == 4608);

enum class Status {
    Written,   // the text is in our slot, the OSD frame incremented
    Released,  // our slot (if any) emptied
    Busy,      // v2.14+ and the lock was taken: nothing written, try again
    NoServer,  // no 'RTSS' signature or a version below 2.0 (RTSS not running or closing)
    BadLayout, // the header's slot array does not fit the view
    NoSlot,    // every slot past the first has another owner
};

constexpr const char* StatusName(Status status) {
    switch (status) {
    case Status::Written:
        return "written";
    case Status::Released:
        return "released";
    case Status::Busy:
        return "busy";
    case Status::NoServer:
        return "no RTSS signature / version";
    case Status::BadLayout:
        return "slot array outside the shared memory";
    case Status::NoSlot:
        return "no free OSD slot";
    }
    return "?";
}

namespace Detail {

inline Header* HeaderOf(std::span<u8> memory) {
    if (memory.size() < sizeof(Header)) {
        return nullptr;
    }
    auto* const header = reinterpret_cast<Header*>(memory.data());
    if (header->signature != Signature || header->version < Version2) {
        return nullptr;
    }
    return header;
}

/// Bytes of a slot this code touches: up to szOSDEx's end from v2.7, else the owner's.
constexpr u64 UsedEntryBytes(u32 version) {
    return version >= VersionTextEx ? sizeof(OsdEntry) : offsetof(OsdEntry, text_ex);
}

/// The slot array fits the view: every slot's used bytes, and the slot's full size (cleared on
/// release), inside `size`. 64-bit arithmetic, no overflow from the header's 32-bit fields.
inline bool LayoutFits(const Header& header, u64 size) {
    const u64 entry = header.osd_entry_size;
    if (header.osd_arr_size < 2 || entry < UsedEntryBytes(header.version) ||
        header.osd_arr_offset < sizeof(Header)) {
        return false;
    }
    return u64{header.osd_arr_offset} + u64{header.osd_arr_size} * entry <= size;
}

inline OsdEntry* EntryAt(std::span<u8> memory, const Header& header, u32 index) {
    return reinterpret_cast<OsdEntry*>(memory.data() + header.osd_arr_offset +
                                       u64{index} * header.osd_entry_size);
}

inline bool OwnerIs(const OsdEntry& entry, std::string_view owner) {
    const size_t length = strnlen(entry.owner, sizeof(entry.owner));
    return std::string_view{entry.owner, length} == owner;
}

/// v2.14's lock, as the sample takes it (_interlockedbittestandset(&dwBusy, 0)): true if it was
/// free and is now ours.
inline bool TryLock(Header& header) {
    return (std::atomic_ref<s32>{header.busy}.fetch_or(1, std::memory_order_acquire) & 1) == 0;
}

inline void Unlock(Header& header) {
    // The sample stores 0 (dwBusy = 0).
    std::atomic_ref<s32>{header.busy}.store(0, std::memory_order_release);
}

inline void NextFrame(Header& header) {
    std::atomic_ref<u32>{header.osd_frame}.fetch_add(1, std::memory_order_release);
}

inline void CopyText(char* destination, size_t capacity, std::string_view text) {
    const size_t length = (std::min)(text.size(), capacity - 1);
    std::memcpy(destination, text.data(), length);
    destination[length] = '\0';
}

} // namespace Detail

/// The sample's UpdateOSD: from slot 1 on (slot 0 is left to the primary client, Afterburner),
/// first a slot we own, else the first free one, taken by writing our owner id. From v2.7 the
/// text goes to szOSDEx, before to szOSD; from v2.14 only under the busy lock (taken: Busy,
/// nothing written). Then the OSD frame is incremented. `memory` is the whole view.
inline Status Update(std::span<u8> memory, std::string_view text,
                     std::string_view owner = Owner) {
    Header* const header = Detail::HeaderOf(memory);
    if (header == nullptr) {
        return Status::NoServer;
    }
    if (!Detail::LayoutFits(*header, memory.size())) {
        return Status::BadLayout;
    }
    OsdEntry* slot = nullptr;
    for (int pass = 0; pass < 2 && slot == nullptr; ++pass) {
        for (u32 index = 1; index < header->osd_arr_size; ++index) {
            OsdEntry* const entry = Detail::EntryAt(memory, *header, index);
            if (pass == 1 && entry->owner[0] == '\0') {
                Detail::CopyText(entry->owner, sizeof(entry->owner), owner);
            }
            if (Detail::OwnerIs(*entry, owner)) {
                slot = entry;
                break;
            }
        }
    }
    if (slot == nullptr) {
        return Status::NoSlot;
    }
    if (header->version >= VersionTextEx) {
        if (header->version >= VersionBusyLock) {
            if (!Detail::TryLock(*header)) {
                return Status::Busy;
            }
            Detail::CopyText(slot->text_ex, sizeof(slot->text_ex), text);
            Detail::Unlock(*header);
        } else {
            Detail::CopyText(slot->text_ex, sizeof(slot->text_ex), text);
        }
    } else {
        Detail::CopyText(slot->text, sizeof(slot->text), text);
    }
    Detail::NextFrame(*header);
    return Status::Written;
}

/// The sample's ReleaseOSD: every slot we own is cleared whole (owner and text empty) and the
/// OSD frame incremented. From v2.14 under the busy lock unless `force` (the caller gave up
/// waiting for it: the sample clears without it).
inline Status Release(std::span<u8> memory, bool force, std::string_view owner = Owner) {
    Header* const header = Detail::HeaderOf(memory);
    if (header == nullptr) {
        return Status::NoServer;
    }
    if (!Detail::LayoutFits(*header, memory.size())) {
        return Status::BadLayout;
    }
    const bool lock = header->version >= VersionBusyLock && !force;
    if (lock && !Detail::TryLock(*header)) {
        return Status::Busy;
    }
    bool cleared = false;
    for (u32 index = 1; index < header->osd_arr_size; ++index) {
        OsdEntry* const entry = Detail::EntryAt(memory, *header, index);
        if (Detail::OwnerIs(*entry, owner)) {
            std::memset(entry, 0, header->osd_entry_size);
            cleared = true;
        }
    }
    if (lock) {
        Detail::Unlock(*header);
    }
    if (cleared) {
        Detail::NextFrame(*header);
    }
    return Status::Released;
}

/// One reading of the counters, taken every ~0.5 s by the OSD thread.
struct Sample {
    double seconds{};      // steady clock
    u64 game_frames{};     // Core::g_game_frames: guest frames composited
    u64 output_frames{};   // Core::g_output_frames: host presents, generated frames included
    u64 generated_frames{}; // Core::g_generated_frames: frame generation's images
    u64 speed_frame_us{};  // Service::Nx2OsdSpeed totals: the presents' intervals
    u64 speed_game_us{};   //   and the game time they stepped
    s64 system_us{};       // CoreTiming's clock (the title bar's speed)
};

struct Rates {
    double game_fps{};
    double display_fps{};
    std::optional<double> speed;      // nullopt: no reading
    std::optional<double> multiplier; // images per game frame; nullopt: none shown
};

/// Rates over two samples (`from` the older): frames per second, and the speed from the game
/// time the presents stepped (Nx2OsdSpeed). Once they have timed the game, a window without a
/// present is a game standing still: 0 (its interval is counted at the present that ends it).
/// Never timed by them: the emulated clock against the wall clock (single core), else none.
/// The multiplier, once frame generation has made an image: (game frames + generated images) /
/// game frames, both counted at each composite (not at the presents, which trail in the queue);
/// none in a window without a game frame (a stall).
constexpr Rates Compute(const Sample& from, const Sample& to) {
    Rates rates{};
    const double seconds = to.seconds - from.seconds;
    if (!(seconds > 0.0)) {
        return rates;
    }
    rates.game_fps = static_cast<double>(to.game_frames - from.game_frames) / seconds;
    rates.display_fps = static_cast<double>(to.output_frames - from.output_frames) / seconds;
    const u64 game_frames = to.game_frames - from.game_frames;
    if (to.generated_frames > 0 && game_frames > 0) {
        rates.multiplier =
            static_cast<double>(game_frames + (to.generated_frames - from.generated_frames)) /
            static_cast<double>(game_frames);
    }
    const u64 frame_us = to.speed_frame_us - from.speed_frame_us;
    if (frame_us > 0) {
        rates.speed = static_cast<double>(to.speed_game_us - from.speed_game_us) /
                      static_cast<double>(frame_us);
    } else if (to.speed_frame_us > 0) {
        rates.speed = 0.0;
    } else if (to.system_us != 0 || from.system_us != 0) {
        rates.speed = static_cast<double>(to.system_us - from.system_us) / (seconds * 1e6);
    }
    return rates;
}

/// Below this the speed is shown in red.
constexpr double SlowSpeed = 0.95;

/// "Game 34.5 FPS | Display 120 FPS | Speed 99%" (ASCII); the
/// speed in red (<C=FF5050>...<C>, RTSS's colour tags) below 95%, "Speed -" without a reading.
/// With frame generation the multiplier follows the display FPS: "Display 120 FPS (3.0x)".
inline std::string Format(const Rates& rates) {
    std::string text = fmt::format("Game {:.1f} FPS | Display {:.0f} FPS", rates.game_fps,
                                   rates.display_fps);
    if (rates.multiplier) {
        text += fmt::format(" ({:.1f}x)", *rates.multiplier);
    }
    text += " | ";
    if (!rates.speed) {
        text += "Speed -";
    } else if (*rates.speed < SlowSpeed) {
        text += fmt::format("<C=FF5050>Speed {:.0f}%<C>", *rates.speed * 100.0);
    } else {
        text += fmt::format("Speed {:.0f}%", *rates.speed * 100.0);
    }
    return text;
}

} // namespace Common::Nx2RtssOsd
