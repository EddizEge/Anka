// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fmt/ranges.h>

#include "common/common_funcs.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/lz4_compression.h"
#include "common/zbic_compression.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nso.h"
#include "core/loader/nx2_dk_dynamic_fps.h"
#include "core/memory.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

namespace Loader {
namespace {
struct MODHeader {
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le eh_frame_hdr_start_offset;
    u32_le eh_frame_hdr_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(MODHeader) == 0x1c, "MODHeader has incorrect size.");

constexpr u32 PageAlignSize(u32 size) {
    return static_cast<u32>((size + Core::Memory::YUZU_PAGEMASK) & ~Core::Memory::YUZU_PAGEMASK);
}
} // Anonymous namespace

bool NSOHeader::IsSegmentCompressed(size_t segment_num) const {
    ASSERT_MSG(segment_num < 3, "Invalid segment {}", segment_num);
    return ((flags >> segment_num) & 1) != 0;
}

AppLoader_NSO::AppLoader_NSO(FileSys::VirtualFile file_) : AppLoader(std::move(file_)) {}

FileType AppLoader_NSO::IdentifyType(const FileSys::VirtualFile& in_file) {
    u32 magic = 0;
    if (in_file->ReadObject(&magic) != sizeof(magic)) {
        return FileType::Error;
    }

    if (Common::MakeMagic('N', 'S', 'O', '0') != magic) {
        return FileType::Error;
    }

    return FileType::NSO;
}

std::optional<VAddr> AppLoader_NSO::LoadModule(Kernel::KProcess& process, Core::System& system, const FileSys::VfsFile& nso_file, VAddr load_base, bool should_pass_arguments, bool load_into_process, std::optional<FileSys::PatchManager> pm, std::vector<Core::NCE::Patcher>* patches, s32 patch_index) {
    if (nso_file.GetSize() < sizeof(NSOHeader))
        return std::nullopt;
    NSOHeader nso_header{};
    if (sizeof(NSOHeader) != nso_file.ReadObject(&nso_header))
        return std::nullopt;
    if (nso_header.magic != Common::MakeMagic('N', 'S', 'O', '0'))
        return std::nullopt;
    if (nso_header.segments.empty())
        return std::nullopt;

    // Allocate some space at the beginning if we are patching in PreText mode.
    const size_t module_start = [&]() -> size_t {
#ifdef HAS_NCE
        if (patches && load_into_process) {
            auto* patch = &patches->operator[](patch_index);
            if (patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
                return patch->GetSectionSize();
            } else if (patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
                return patch->GetPreSectionSize();
            }
        }
#endif
        return 0;
    }();

    auto const last_segment_it = &nso_header.segments[nso_header.segments.size() - 1];
    // Build program image directly in codeset memory :)
    Kernel::CodeSet codeset;
    codeset.memory.resize(module_start + last_segment_it->location + last_segment_it->size);
    {
        std::vector<u8> compressed_data(*std::ranges::max_element(nso_header.segments_compressed_size));
        std::vector<u8> decompressed_size(std::ranges::max_element(nso_header.segments, [](auto const& a, auto const& b) {
            return a.size < b.size;
        })->size);
        for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
            nso_file.Read(compressed_data.data(), nso_header.segments_compressed_size[i], nso_header.segments[i].offset);
            if (nso_header.IsSegmentCompressed(i)) {
                if (nso_header.IsZBICCompressed()) {
                    // ZBIC compression
                    const int r = Common::Compression::DecompressDataZBIC(
                        std::span<u8>{decompressed_size}.first(nso_header.segments[i].size),
                        std::span<const u8>{compressed_data}.first(nso_header.segments_compressed_size[i])
                    );
                    ASSERT(r > 0);
                } else {
                    // LZ4 compression
                    int r = Common::Compression::DecompressDataLZ4(
                        decompressed_size.data(),
                        nso_header.segments[i].size,
                        compressed_data.data(),
                        nso_header.segments_compressed_size[i]
                    );
                    ASSERT(r == int(nso_header.segments[i].size));
                }

                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    decompressed_size.data(),
                    nso_header.segments[i].size
                );
            } else {
                // Not compressed
                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    compressed_data.data(),
                    nso_header.segments[i].size
                );
            }
            codeset.segments[i].addr = module_start + nso_header.segments[i].location;
            codeset.segments[i].offset = module_start + nso_header.segments[i].location;
            codeset.segments[i].size = nso_header.segments[i].size;
        }
    }

    if (should_pass_arguments && !Settings::values.program_args.GetValue().empty()) {
        const auto arg_data{Settings::values.program_args.GetValue()};

        codeset.DataSegment().size += NSO_ARGUMENT_DATA_ALLOCATION_SIZE;
        NSOArgumentHeader args_header{NSO_ARGUMENT_DATA_ALLOCATION_SIZE, static_cast<u32_le>(arg_data.size()), {}};
        const auto end_offset = codeset.memory.size();
        codeset.memory.resize(u32(codeset.memory.size()) + NSO_ARGUMENT_DATA_ALLOCATION_SIZE);
        std::memcpy(codeset.memory.data() + end_offset, &args_header, sizeof(NSOArgumentHeader));
        std::memcpy(codeset.memory.data() + end_offset + sizeof(NSOArgumentHeader), arg_data.data(), arg_data.size());
    }

    codeset.DataSegment().size += nso_header.segments[2].bss_size;
    u32 image_size = PageAlignSize(u32(codeset.memory.size()) + nso_header.segments[2].bss_size);
    codeset.memory.resize(image_size);

    for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
        codeset.segments[i].size = PageAlignSize(codeset.segments[i].size);
    }

    // Apply patches if necessary
    const auto name = nso_file.GetName();
    // What each user exefs mod changed (EDEN_NX2_DK_DYN checks them against its code below)
    std::vector<FileSys::NsoModChange> mod_changes;
    if (pm && (pm->HasNSOPatch(nso_header.build_id, name) || Settings::values.dump_nso)) {
        std::span<u8> patchable_section(codeset.memory.data() + module_start, codeset.memory.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(),
                    patchable_section.size());

        pi_header = pm->PatchNSO(pi_header, name, &mod_changes);

        std::copy(pi_header.begin() + sizeof(NSOHeader), pi_header.end(), patchable_section.data());
    }

    // EDEN_NX2_DK_DYN=1 (off by default): Donkey Kong Bananza's dynamic frame rate
    // (nx2_dk_dynamic_fps.h). Off, no module is touched.
    static const char* const dk_dyn_switch = std::getenv("EDEN_NX2_DK_DYN");
    // EDEN_NX2_DK_DYN_MIN_FPS=<40-60> (unset: the game's 40; 1-39 only with
    // EDEN_NX2_DK_DYN_MIN_FPS_LOW=1): below it the whole interval.
    static const Nx2DkDynamicFps::MinFpsChoice dk_dyn_min_fps = [] {
        const char* value = std::getenv("EDEN_NX2_DK_DYN_MIN_FPS");
        const auto parsed = Nx2DkDynamicFps::ParseMinFps(value);
        if (parsed.refused) {
            LOG_WARNING(Loader, "EDEN_NX2_DK_DYN_MIN_FPS=\"{}\": not a whole number of 1-60, the "
                                "game's 40 kept",
                        value);
        }
        // Below 40 only with EDEN_NX2_DK_DYN_MIN_FPS_LOW=1 (nx2_dk_dynamic_fps.h, ChooseMinFps);
        // the warning is written where the patch goes in (Bananza, DK_DYN on), below.
        auto chosen = Nx2DkDynamicFps::ChooseMinFps(parsed,
                                                    std::getenv("EDEN_NX2_DK_DYN_MIN_FPS_LOW"));
        if (chosen.held_low) {
            chosen.fps = parsed.fps; // only for the warning; 0 goes to Apply
        }
        return chosen;
    }();
    // EDEN_NX2_DK_DYN_MOD_GUARD (unset / "auto": a mod in the frame rate code or one named like a
    // frame rate mod, or a cheat named like one, turns the patch off; "all": any exefs mod
    // changing main; "name": the names only; "0": Anka 1.1.1's rule; nx2_dk_dynamic_fps.h)
    static const char* const dk_dyn_guard_value = std::getenv("EDEN_NX2_DK_DYN_MOD_GUARD");
    static const Nx2DkDynamicFps::ModGuardChoice dk_dyn_guard = [] {
        const auto choice = Nx2DkDynamicFps::ParseModGuard(dk_dyn_guard_value);
        if (choice.refused) {
            LOG_WARNING(Loader,
                        "EDEN_NX2_DK_DYN_MOD_GUARD=\"{}\": not auto / all / name / 0, auto used",
                        dk_dyn_guard_value);
        }
        return choice;
    }();
    static const std::string dk_dyn_guard_label =
        dk_dyn_guard_value == nullptr
            ? std::string{"EDEN_NX2_DK_DYN_MOD_GUARD unset: auto"}
            : fmt::format("EDEN_NX2_DK_DYN_MOD_GUARD={}",
                          Nx2DkDynamicFps::ModGuardName(dk_dyn_guard.guard));
    // The cheats of Bananza's main, read before the patch is decided: a "60 FPS" cheat with it
    // runs the game too fast (reused below, where they are registered).
    std::optional<std::vector<Core::Memory::CheatEntry>> early_cheats;
    const bool dk_dyn_target = Nx2DkDynamicFps::Enabled(dk_dyn_switch) && name == "main" &&
                               process.GetProgramId() == Nx2DkDynamicFps::ProgramId;
    if (pm && dk_dyn_target) {
        early_cheats = pm->CreateCheatList(nso_header.build_id);
    }
    {
        const auto& text = nso_header.segments[0];
        const size_t text_start = module_start + text.location;
        const std::span<u8> image{codeset.memory};
        const std::span<u8> text_image =
            text_start <= image.size() ? image.subspan(text_start) : std::span<u8>{};
        // Another exefs mod or a cheat that may change the frame rate: the patch stays out of
        // this boot (nx2_dk_dynamic_fps.h, Decide)
        bool dk_dyn_blocked = false;
        std::vector<std::string> cheat_names;
        if (early_cheats) {
            for (const auto& cheat : *early_cheats) {
                if (cheat.enabled) {
                    const auto& n = cheat.definition.readable_name;
                    const std::string_view full{n.data(), n.size()};
                    cheat_names.emplace_back(full.substr(0, full.find('\0')));
                }
            }
        }
        if (dk_dyn_target && (!mod_changes.empty() || !cheat_names.empty())) {
            const auto zones = Nx2DkDynamicFps::FpsZones(text_image, text.size);
            std::vector<Nx2DkDynamicFps::ModChange> mods;
            mods.reserve(mod_changes.size());
            for (const auto& change : mod_changes) {
                mods.push_back({change.mod, change.kind, change.file, change.ranges});
            }
            const std::vector<std::string_view> cheat_views(cheat_names.begin(),
                                                            cheat_names.end());
            const auto decision = Nx2DkDynamicFps::Decide(dk_dyn_guard.guard, zones,
                                                          text.location, mods, cheat_views);
            using Why = Nx2DkDynamicFps::Blocker::Why;
            for (const auto& blocker : decision.blockers) {
                switch (blocker.why) {
                case Why::Zone:
                    LOG_WARNING(Loader,
                                "Donkey Kong Bananza: dynamic FPS (EDEN_NX2_DK_DYN) off for this "
                                "boot: the {} mod \"{}\" changes its {} (main+{:#x}); both would "
                                "run the game too fast. Turn the mod off in Add-Ons to use it ({})",
                                blocker.kind, blocker.mod, blocker.what, blocker.offset,
                                dk_dyn_guard_label);
                    break;
                case Why::Name:
                case Why::AnyMod:
                    LOG_WARNING(Loader,
                                "Donkey Kong Bananza: dynamic FPS (EDEN_NX2_DK_DYN) off for this "
                                "boot: the {} mod \"{}\" ({}) changes main (first at main+{:#x}){}; "
                                "with a frame rate change both would run the game too fast above "
                                "30 fps. Turn the mod off in Add-Ons to use dynamic FPS ({})",
                                blocker.kind, blocker.mod, blocker.file, blocker.offset,
                                blocker.why == Why::Name
                                    ? " and its name reads like a frame rate mod"
                                    : "",
                                dk_dyn_guard_label);
                    break;
                case Why::Cheat:
                    LOG_WARNING(Loader,
                                "Donkey Kong Bananza: dynamic FPS (EDEN_NX2_DK_DYN) off for this "
                                "boot: the cheat \"{}\" reads like a frame rate cheat; both would "
                                "run the game too fast above 30 fps. Turn the cheat off to use "
                                "dynamic FPS ({})",
                                blocker.mod, dk_dyn_guard_label);
                    break;
                }
            }
            if (decision.Blocked()) {
                const auto& first = decision.blockers.front();
                Core::SetNx2DkDynConflict(first.why == Why::Cheat ? "cheat \"" + first.mod + "\""
                                                                  : first.mod);
                dk_dyn_blocked = true;
            }
            if (!decision.elsewhere.empty()) {
                LOG_WARNING(Loader,
                            "Donkey Kong Bananza: exefs mod(s) {} change main with EDEN_NX2_DK_DYN "
                            "on, outside the frame rate code it patches and not named like a "
                            "frame rate mod; if the game runs too fast above 30 fps, one of them "
                            "changes the frame rate too ({})",
                            fmt::join(decision.elsewhere, ", "), dk_dyn_guard_label);
            }
        }
        if (dk_dyn_blocked) {
            Core::g_nx2_dk_dyn_applied.store(false, std::memory_order_relaxed);
        }
        const auto dk_dyn = dk_dyn_blocked
                                ? std::optional<Nx2DkDynamicFps::Result>{}
                                : Nx2DkDynamicFps::ApplyIfEnabled(
                                      dk_dyn_switch, process.GetProgramId(), name, text_image,
                                      text.size, PageAlignSize(text.size),
                                      dk_dyn_min_fps.held_low ? 0 : dk_dyn_min_fps.fps);
        if (dk_dyn) {
            Core::g_nx2_dk_dyn_applied.store(dk_dyn->applied, std::memory_order_relaxed);
            if (dk_dyn->applied) {
                LOG_INFO(Loader,
                         "Donkey Kong Bananza: dynamic FPS on (main+{:#x}, main+{:#x}, main+{:#x}; "
                         "EDEN_NX2_DK_DYN)",
                         text.location + dk_dyn->controller * sizeof(u32),
                         text.location + dk_dyn->sleep * sizeof(u32),
                         text.location + dk_dyn->step * sizeof(u32));
                if (dk_dyn_min_fps.held_low) {
                    LOG_WARNING(Loader,
                                "EDEN_NX2_DK_DYN_MIN_FPS={}: the game's 40 kept. Its free frame "
                                "rates stop at 40, so below 40 it only held the game longer in "
                                "slow frames (EDEN_NX2_DK_DYN_MIN_FPS_LOW=1 puts it in anyway)",
                                dk_dyn_min_fps.fps);
                }
                if (dk_dyn->min_fps != 0) {
                    LOG_INFO(Loader,
                             "Donkey Kong Bananza: the whole interval below {} fps instead of 40 "
                             "(main+{:#x}; EDEN_NX2_DK_DYN_MIN_FPS)",
                             dk_dyn->min_fps,
                             text.location +
                                 (dk_dyn->controller + Nx2DkDynamicFps::LowTargetOffset) *
                                     sizeof(u32));
                } else if (dk_dyn->min_fps_missed) {
                    LOG_WARNING(Loader, "Donkey Kong Bananza: EDEN_NX2_DK_DYN_MIN_FPS not "
                                        "applied: the 40 fps check is not where expected");
                }
            } else {
                LOG_WARNING(Loader,
                            "Donkey Kong Bananza: dynamic FPS not applied ({}, {}, {} matches; "
                            "EDEN_NX2_DK_DYN)",
                            dk_dyn->controller_matches, dk_dyn->sleep_matches,
                            dk_dyn->step_matches);
            }
        }
    }

#ifdef HAS_NCE
    // If we are computing the process code layout and using nce backend, patch.
    const auto& code = codeset.CodeSegment();
    auto* patch = patches ? &patches->operator[](patch_index) : nullptr;
    if (patch && !load_into_process) {
        //Set module ID using build_id from the NSO header
        patch->SetModuleID(nso_header.build_id);
        // Patch SVCs and MRS calls in the guest code
        while (!patch->PatchText(codeset.memory, code)) {
            patch = &patches->emplace_back();
            patch->SetModuleID(nso_header.build_id);  // In case the patcher is changed for big modules, the new patcher should also have the build_id
        }
    } else if (patch) {
        // Relocate code patch and copy to the program image.
        // Save size before RelocateAndCopy (which may resize)
        const size_t size_before_relocate = codeset.memory.size();
        if (patch->RelocateAndCopy(load_base, code, codeset.memory, &process.GetPostHandlers())) {
            // Update patch section.
            auto& patch_segment = codeset.PatchSegment();
            auto& post_patch_segment = codeset.PostPatchSegment();
            const auto patch_mode = patch->GetPatchMode();
            if (patch_mode == Core::NCE::PatchMode::PreText) {
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else if (patch_mode == Core::NCE::PatchMode::Split) {
                // For Split-mode, we are using pre-patch buffer at start, post-patch buffer at end
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetPreSectionSize());
                post_patch_segment.addr = size_before_relocate;
                post_patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else {
                patch_segment.addr = image_size;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            }
        }

        // Refresh image_size to take account the patch section if it was added by RelocateAndCopy
        image_size = static_cast<u32>(codeset.memory.size());
    }
#endif

    // If we aren't actually loading (i.e. just computing the process code layout), we are done
    if (!load_into_process) {
#ifdef HAS_NCE
        // Ok, so for Split mode, we need to account for pre-patch and post-patch space
        // which will be added during RelocateAndCopy in the second pass. Where it crashed
        // in Android Studio at PreText. May be a better way. Works for now.
        if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
            return load_base + patch->GetPreSectionSize() + image_size + patch->GetSectionSize();
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
            return load_base + patch->GetSectionSize() + image_size;
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PostData) {
            return load_base + image_size + patch->GetSectionSize();
        }
#endif
        return load_base + image_size;
    }

    // Apply cheats if they exist and the program has a valid title ID
    if (pm) {
        // TODO(Maufeat): Check if there is a better way to check
        if (name == "main")
            system.SetApplicationProcessBuildID(nso_header.build_id);

        const auto cheats =
            early_cheats ? std::move(*early_cheats) : pm->CreateCheatList(nso_header.build_id);
        if (!cheats.empty() && name == "main" && Nx2DkDynamicFps::Enabled(dk_dyn_switch) &&
            process.GetProgramId() == Nx2DkDynamicFps::ProgramId &&
            Core::g_nx2_dk_dyn_applied.load(std::memory_order_relaxed)) {
            LOG_WARNING(Loader,
                        "Donkey Kong Bananza: {} cheat(s) with dynamic FPS (EDEN_NX2_DK_DYN); if "
                        "one changes the frame rate, the game runs too fast above 30 fps",
                        cheats.size());
        }
        if (!cheats.empty()) {
            system.RegisterCheatList(cheats, nso_header.build_id, load_base, image_size);
        }
    }

    // Load codeset for current process
    LOG_INFO(Loader, "Module {} loaded at {:#x} (size {:#x})", nso_file.GetName(), load_base, image_size);
    const VAddr rodata = load_base + GetInteger(codeset.RODataSegment().addr);
    process.RegisterLoadedModule({
        .base = load_base,
        .end = load_base + image_size,
        .name = nso_file.GetName(),
        .dynsym = rodata + nso_header.dynsyn_extent.data_offset,
        .dynsym_size = nso_header.dynsyn_extent.size,
        .dynstr = rodata + nso_header.dynstr_extent.data_offset,
        .dynstr_size = nso_header.dynstr_extent.size,
    });
    process.LoadModule(system.Kernel(), std::move(codeset), load_base);
    return load_base + image_size;
}

AppLoader_NSO::LoadResult AppLoader_NSO::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    modules.clear();

    // Load module
    const VAddr base_address = GetInteger(process.GetEntryPoint());
    if (!LoadModule(process, system, *file, base_address, true, true)) {
        return {ResultStatus::ErrorLoadingNSO, {}};
    }

    modules.insert_or_assign(base_address, file->GetName());
    LOG_DEBUG(Loader, "loaded module {} @ {:#x}", file->GetName(), base_address);

    is_loaded = true;
    return {ResultStatus::Success, LoadParameters{Kernel::KThread::DefaultThreadPriority,
                                                  Core::Memory::DEFAULT_STACK_SIZE}};
}

ResultStatus AppLoader_NSO::ReadNSOModules(Modules& out_modules) {
    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
