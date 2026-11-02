// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bitset>
#include <memory>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/nx2_reg_replica.h"

namespace {
using namespace VideoCommon::Nx2RegReplica;

// The capture only takes the engines' addresses: stand-ins for two Maxwell3D objects.
alignas(8) unsigned char g_engine_storage[2][8];
Tegra::Engines::Maxwell3D& Engine(size_t index) {
    return *reinterpret_cast<Tegra::Engines::Maxwell3D*>(g_engine_storage[index]);
}

// What the registers' dirty tables, light classes and executable methods are (made up).
struct Setup {
    DirtyTables tables{};
    std::array<u32, NumRegs> classes{};
    std::bitset<NumRegs> executable;
};

// An engine's registers, dirty flags and light changes, as ProcessDirtyRegisters keeps them
// (the reference, and A's primary) or as ApplyDelta does (a replica).
struct Model {
    std::vector<u32> regs = std::vector<u32>(NumRegs);
    DirtyFlags flags;
    u32 changes{};
};

// ProcessDirtyRegisters behind CallMethod / ConsumeSinkImpl: returns whether the write was
// made, and in `light` whether it noted a light change.
bool OldWrite(const Setup& setup, Model& model, u32 method, u32 value, bool skip_same,
              bool& light) {
    if (!setup.executable[method] && skip_same && model.regs[method] == value) {
        return false;
    }
    const bool changed = model.regs[method] != value;
    model.regs[method] = value;
    for (const auto& table : setup.tables) {
        model.flags[table[method]] = true;
    }
    light = changed || setup.executable[method];
    if (light) {
        model.changes |= setup.classes[method];
    }
    return true;
}

// Takes the capture's words and applies them to `replica`; `strip` clears these delta bits
// first (the check must then fail).
void Deliver(const Setup& setup, RegCapture& capture, Model& replica, u64 strip) {
    std::vector<u64> words(capture.PendingWords());
    capture.TakeWords(words.data());
    for (u64& word : words) {
        word &= ~strip;
    }
    ApplyDelta(replica.regs.data(), replica.flags, setup.tables, words.data(),
               static_cast<u32>(words.size()),
               [&](u32 method) { replica.changes |= setup.classes[method]; });
}

// Differences between the reference and the replica (constant buffer data registers aside:
// the capture leaves them out but at =2; what they set is NullEntry alone).
u32 Differences(const Model& reference, const Model& replica) {
    u32 differences = 0;
    for (u32 method = 0; method < NumRegs; ++method) {
        if (method - CbDataMethod < CbDataCount) {
            continue;
        }
        differences += reference.regs[method] != replica.regs[method] ? 1 : 0;
    }
    // Flag 0 is NullEntry (dirty_flags.h): set, read by nobody.
    DirtyFlags reference_flags = reference.flags;
    DirtyFlags replica_flags = replica.flags;
    reference_flags[0] = false;
    replica_flags[0] = false;
    differences += reference_flags != replica_flags ? 1 : 0;
    differences += reference.changes != replica.changes ? 1 : 0;
    return differences;
}

// A random stream over two engines: writes from a small register set (repeats within a
// record, A -> B -> A, executable methods written with the value they hold, constant buffer
// data), records at random points (a draw now and then clears flags and changes, as the
// rasterizer does), the engine switching now and then. Returns the differences seen at record
// boundaries.
u32 RunStream(bool skip_same, u64 strip, u32 seed) {
    std::mt19937 random{seed};
    const auto below = [&random](u32 bound) {
        return static_cast<u32>(random() % bound);
    };
    Setup setup;
    for (auto& table : setup.tables) {
        for (auto& flag : table) {
            flag = static_cast<u8>(below(256));
        }
    }
    for (auto& light_class : setup.classes) {
        light_class = 1U << below(10);
    }
    std::vector<u32> methods;
    for (u32 i = 0; i < 48; ++i) {
        const u32 method = below(NumRegs);
        methods.push_back(method);
        setup.executable[method] = below(8) == 0;
    }
    for (u32 i = 0; i < CbDataCount; ++i) {
        // As in Maxwell3D: no dirty flag but NullEntry, no light class (ProcessCBMultiData
        // notes the data's light change where it writes it).
        methods.push_back(CbDataMethod + i);
        setup.executable[CbDataMethod + i] = true;
        setup.tables[0][CbDataMethod + i] = 0;
        setup.tables[1][CbDataMethod + i] = 0;
        setup.classes[CbDataMethod + i] = 0;
    }

    std::array<Model, 2> reference;
    std::array<Model, 2> replica;
    RegCapture capture;
    capture.Reset();
    size_t engine = 0;
    u32 differences = 0;
    const auto record = [&](bool draw) {
        if (capture.Source() != nullptr) {
            const size_t source = capture.Source() == &Engine(0) ? 0 : 1;
            Deliver(setup, capture, replica[source], strip);
        }
        for (size_t e = 0; e < 2; ++e) {
            differences += Differences(reference[e], replica[e]);
            if (draw) {
                reference[e].flags.reset();
                reference[e].changes = 0;
                replica[e].flags.reset();
                replica[e].changes = 0;
            }
        }
    };
    for (u32 step = 0; step < 20000; ++step) {
        if (below(64) == 0) {
            engine ^= 1;
        }
        const u32 method = methods[below(static_cast<u32>(methods.size()))];
        const u32 value = below(3);
        const bool noted = method - CbDataMethod >= CbDataCount;
        if (noted && capture.Source() != nullptr && capture.Source() != &Engine(engine)) {
            // The Target record PostTarget sends takes what is pending, for the old engine.
            record(false);
        }
        bool light = false;
        if (OldWrite(setup, reference[engine], method, value, skip_same, light)) {
            capture.Note(Engine(engine), method, value, true, light);
            // Left out, constant buffer data does not make the engine the source.
            differences += noted && capture.Source() != &Engine(engine) ? 1 : 0;
        }
        if (below(8) == 0) {
            record(below(4) == 0);
        }
    }
    record(true);
    return differences;
}

} // Anonymous namespace

TEST_CASE("Nx2RegReplica: a delta word keeps its fields apart", "[video_core]") {
    const u64 word = MakeDelta(0xdff, 0x89abcdef, true, false);
    REQUIRE(DeltaMethod(word) == 0xdff);
    REQUIRE(DeltaValue(word) == 0x89abcdef);
    REQUIRE((word & DeltaDirty) != 0);
    REQUIRE((word & DeltaChanged) == 0);
    REQUIRE((word & 0xfffc0000u) == 0);
    const u64 side = MakeSideHeader(SideConstBuffers | SideDirtyFlags, 77);
    REQUIRE(SideParts(side) == (SideConstBuffers | SideDirtyFlags));
    REQUIRE(SideWords(side) == 77);
}

// T2: a random register stream (SKIP_SAME on and off, A -> B -> A within one range, executable
// methods) through ProcessDirtyRegisters and through RegCapture + ApplyDelta gives the same
// registers, dirty flags and light changes; it fails with Dirty / Changed left out.
TEST_CASE("Nx2RegReplica: a replica follows ProcessDirtyRegisters through the deltas",
          "[video_core]") {
    for (u32 seed = 1; seed <= 4; ++seed) {
        REQUIRE(RunStream(true, 0, seed) == 0);
        REQUIRE(RunStream(false, 0, seed) == 0);
    }
    // Without Dirty: same-value writes (SKIP_SAME off) and A -> B -> A lose their dirty flags.
    REQUIRE(RunStream(false, DeltaDirty, 1) > 0);
    REQUIRE(RunStream(true, DeltaDirty, 1) > 0);
    // Without Changed: executable methods written with the value they hold, and A -> B -> A,
    // lose their light change.
    REQUIRE(RunStream(true, DeltaChanged, 1) > 0);
    REQUIRE(RunStream(false, DeltaChanged, 1) > 0);
}

TEST_CASE("Nx2RegReplica: A -> B -> A within a record", "[video_core]") {
    Setup setup;
    constexpr u32 method = 0x123;
    setup.tables[0][method] = 7;
    setup.tables[1][method] = 9;
    setup.classes[method] = 1U << 3;
    Model reference;
    Model replica;
    reference.regs[method] = 5;
    replica.regs[method] = 5;
    RegCapture capture;
    capture.Reset();
    for (const u32 value : {7U, 5U}) {
        bool light = false;
        REQUIRE(OldWrite(setup, reference, method, value, true, light));
        capture.Note(Engine(0), method, value, true, light);
    }
    REQUIRE(capture.PendingWords() == 1);
    Model stripped = replica;
    RegCapture again;
    again.Reset();
    again.Note(Engine(0), method, 5, true, true);
    Deliver(setup, capture, replica, 0);
    Deliver(setup, again, stripped, DeltaDirty | DeltaChanged);
    REQUIRE(Differences(reference, replica) == 0);
    REQUIRE(replica.flags[7] && replica.flags[9] && replica.changes == (1U << 3));
    REQUIRE(Differences(reference, stripped) == 2);
}

TEST_CASE("Nx2RegReplica: the capture's words, engines and constant buffer data",
          "[video_core]") {
    RegCapture capture;
    capture.Reset();
    REQUIRE(capture.PendingWords() == 0);
    REQUIRE(capture.Source() == nullptr);
    // Constant buffer data registers are left out (but at =2).
    capture.Note(Engine(0), CbDataMethod + 3, 1, true, true);
    REQUIRE(capture.PendingWords() == 0);
    // Words in the order of their first writes, the last value, Dirty and Changed ORed.
    capture.Note(Engine(0), 0x40, 1, false, false);
    capture.Note(Engine(0), 0x20, 2, true, false);
    capture.Note(Engine(0), 0x40, 3, false, true);
    REQUIRE(capture.Source() == &Engine(0));
    REQUIRE(capture.PendingWords() == 2);
    std::array<u64, 2> words{};
    capture.TakeWords(words.data());
    REQUIRE(words[0] == MakeDelta(0x40, 3, false, true));
    REQUIRE(words[1] == MakeDelta(0x20, 2, true, false));
    REQUIRE(capture.PendingWords() == 0);
    // Taken words start over: the register is new again.
    capture.Note(Engine(0), 0x40, 4, false, false);
    capture.TakeWords(words.data());
    REQUIRE(words[0] == MakeDelta(0x40, 4, false, false));
    // Another engine becomes the source; Reset forgets it and what is pending.
    capture.Note(Engine(1), 0x41, 5, true, true);
    REQUIRE(capture.Source() == &Engine(1));
    REQUIRE(capture.PendingWords() == 1);
    capture.Reset();
    REQUIRE(capture.Source() == nullptr);
    REQUIRE(capture.PendingWords() == 0);
    capture.Note(Engine(1), 0x41, 6, false, false);
    capture.TakeWords(words.data());
    REQUIRE(words[0] == MakeDelta(0x41, 6, false, false));
    // SelectTarget (PostCbData's, K6): the source changes without a word; for the source it is
    // a no-op that leaves what is pending.
    capture.SelectTarget(Engine(0));
    REQUIRE(capture.Source() == &Engine(0));
    REQUIRE(capture.PendingWords() == 0);
    capture.Note(Engine(0), 0x42, 7, true, false);
    capture.SelectTarget(Engine(0));
    REQUIRE(capture.Source() == &Engine(0));
    REQUIRE(capture.PendingWords() == 1);
}

TEST_CASE("Nx2RegReplica: a write outside ProcessDirtyRegisters reaches the replica",
          "[video_core]") {
    // Noted with dirty false (shadow_scratch[0] = 1): the replica
    // takes the value; holding another one, it sets the dirty flags and notes a light change
    // (more than the primary did: extra work, never less).
    Setup setup;
    constexpr u32 method = 0xd00;
    setup.tables[0][method] = 3;
    setup.classes[method] = 1U << 8;
    Model replica;
    RegCapture capture;
    capture.Reset();
    capture.Note(Engine(0), method, 1, false, false);
    Deliver(setup, capture, replica, 0);
    REQUIRE(replica.regs[method] == 1);
    REQUIRE(replica.flags[3]);
    REQUIRE(replica.changes == (1U << 8));
    // The same value again: nothing but the value.
    replica.flags.reset();
    replica.changes = 0;
    capture.Note(Engine(0), method, 1, false, false);
    Deliver(setup, capture, replica, 0);
    REQUIRE(replica.flags.none());
    REQUIRE(replica.changes == 0);
}
