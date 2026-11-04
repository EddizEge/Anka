// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "video_core/renderer_vulkan/present/nx2_fg_refresh.h"

namespace Vulkan::Nx2FgTiming {

f32 QueryDisplayRefreshHz([[maybe_unused]] void* native_window) {
#ifdef _WIN32
    const HWND window = static_cast<HWND>(native_window);
    const HMONITOR monitor = window != nullptr ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                                               : MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    if (monitor == nullptr) {
        return 0.0f;
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return 0.0f;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        return 0.0f;
    }
    // 0 and 1 stand for the hardware default
    return mode.dmDisplayFrequency > 1 ? static_cast<f32>(mode.dmDisplayFrequency) : 0.0f;
#else
    return 0.0f;
#endif
}

} // namespace Vulkan::Nx2FgTiming
