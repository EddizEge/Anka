// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "yuzu_cmd/anka_profile.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#include "common/string_util.h"
#endif

// The profile's variables must be in the environment before the first switch is read, and some
// are read before main: namespace-scope constants such as video_core's g_nx2_sync_skip_mode or
// texture_cache's g_nx2_tex_touch_skip (EDEN_NX2_SYNC_SKIP, _TEX_TOUCH_SKIP and the others
// tools/baslatici112/onmain_tara.py lists in the yayin lane) are dynamic initializers in the CRT's
// .CRT$XCU table. This file's initializers go in the library table (.CRT$XCL, init_seg(lib)), which
// the CRT runs before every .CRT$XCU entry of the program: the EarlyEnvironment below reads the
// command line and sets the variables there, before any of them. MSVC and clang-cl both honor the
// pragma; other compilers get a high init_priority. Without --profile it changes nothing.
#if defined(_MSC_VER)
#pragma warning(disable : 4073) // initializers put in library initialization area
#pragma init_seg(lib)
#define ANKA_PROFILE_EARLY_INIT
#elif defined(__GNUC__)
#define ANKA_PROFILE_EARLY_INIT __attribute__((init_priority(101)))
#endif

namespace AnkaProfile {
namespace fs = std::filesystem;

namespace {
#ifdef _WIN32
// The standard output as this process got it, before SDL_AppInit attaches to the console: a
// file or pipe when the launcher's output was redirected. A duplicate: SDL_AppInit's
// freopen("CONOUT$") closes the inherited handle.
HANDLE g_first_stdout = INVALID_HANDLE_VALUE;
#endif

std::optional<std::string> ReadFile(const fs::path& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return std::nullopt;
    }
    std::string bytes{std::istreambuf_iterator<char>{file}, {}};
    if (file.bad()) {
        return std::nullopt;
    }
    return bytes;
}

/// Written next to it first and then moved over it: a stop half-way leaves the file whole.
bool WriteFileWhole(const fs::path& path, const std::string& bytes) {
    fs::path tmp = path;
    tmp += ".nx2tmp";
    {
        std::ofstream file{tmp, std::ios::binary | std::ios::trunc};
        if (!file) {
            return false;
        }
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

/// anka-config.ps1's "set": the pairs into [Renderer] of `path`.
bool SetKeys(const fs::path& path, std::span<const std::string_view> pairs) {
    const auto bytes = ReadFile(path);
    if (!bytes) {
        return false;
    }
    Ini ini = ParseIni(*bytes);
    SetRenderer(ini, pairs);
    return WriteFileWhole(path, WriteIni(ini));
}

std::string Utf8(const fs::path& path) {
    return Common::FS::PathToUTF8String(path);
}

#ifdef _WIN32
// Pre-main: no common/ code (its own initializers may not have run yet), only Win32 and the STL.
std::string WideToUtf8(const wchar_t* wide) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string text(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), size, nullptr, nullptr);
    return text;
}
#endif

bool g_applied_early = false;

/// The profile's variables from the process's own command line (the same one SDL_AppInit gets),
/// before the program's initializers. A bad option sets nothing (SDL_AppInit reports it).
void ApplyFromCommandLine() {
#ifdef _WIN32
    int argc = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wide == nullptr) {
        return;
    }
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.push_back(WideToUtf8(wide[i]));
    }
    LocalFree(wide);
    std::vector<const char*> argv;
    for (const std::string& arg : args) {
        argv.push_back(arg.c_str());
    }
    const TakenArgs taken = TakeArgs(argc, argv.data());
    if (taken.options.profile && taken.error.empty()) {
        ApplyEnvironment(Variables(taken.options));
        g_applied_early = true;
    }
#endif
}

struct EarlyEnvironment {
    EarlyEnvironment() {
        ApplyFromCommandLine();
    }
};
} // namespace

// Its dynamic initializer runs in the library table, after g_first_stdout's (see init_seg above).
ANKA_PROFILE_EARLY_INIT const EarlyEnvironment g_early_environment;

bool AppliedBeforeInitializers() {
    return g_applied_early;
}

void CaptureStandardOutput() {
#ifdef _WIN32
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        const DWORD type = GetFileType(handle);
        HANDLE own = INVALID_HANDLE_VALUE;
        if ((type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) &&
            DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &own, 0, FALSE,
                            DUPLICATE_SAME_ACCESS)) {
            g_first_stdout = own;
        }
    }
#endif
}

void Say(std::string_view line) {
    std::string text{line};
#ifdef _WIN32
    text += "\r\n";
    DWORD written = 0;
    if (g_first_stdout != INVALID_HANDLE_VALUE) {
        WriteFile(g_first_stdout, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    // The console SDL_AppInit attached to (stdout reopened on CONOUT$), as the log's console lines.
    const intptr_t crt = _get_osfhandle(_fileno(stdout));
    const HANDLE console = crt == -1 || crt == -2 ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(crt);
    DWORD mode = 0;
    const std::wstring wide = Common::UTF8ToUTF16W(text);
    if (console != INVALID_HANDLE_VALUE && GetConsoleMode(console, &mode)) {
        WriteConsoleW(console, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
        return;
    }
    // Attached (AttachToConsole) but stdout not reopened yet.
    const HANDLE conout = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                                      nullptr);
    if (conout != INVALID_HANDLE_VALUE) {
        WriteConsoleW(conout, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
        CloseHandle(conout);
        return;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
#else
    text += '\n';
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
#endif
}

void SayAll(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        Say(line);
    }
}

void AttachToConsole() {
#ifdef _WIN32
    if (g_first_stdout == INVALID_HANDLE_VALUE) {
        AttachConsole(ATTACH_PARENT_PROCESS);
    }
#endif
}

void ApplyEnvironment(const std::vector<Var>& vars) {
    for (const Var& var : vars) {
        const std::string name{var.name};
#ifdef _WIN32
        // An empty value removes the variable, as cmd's "set NAME=" did.
        _putenv_s(name.c_str(), var.value.c_str());
#else
        if (var.value.empty()) {
            unsetenv(name.c_str());
        } else {
            setenv(name.c_str(), var.value.c_str(), 1);
        }
#endif
    }
}

fs::path UserConfigDir() {
#ifdef _WIN32
    // As the program finds its user folder (common/fs/path_util.cpp): user\ next to it, else
    // %APPDATA%\eden. Nothing is made here.
    fs::path user = Common::FS::GetExeDirectory() / PORTABLE_DIR;
    std::error_code ec;
    if (!fs::is_directory(user, ec)) {
        user = Common::FS::GetAppDataRoamingDirectory() / EDEN_DIR;
    }
    return user / CONFIG_DIR;
#else
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
#endif
}

ConfigResult PrepareConfig(Profile profile, const fs::path& config_dir) {
    ConfigResult result;
    const auto say = [&result](std::string line) { result.said.push_back(std::move(line)); };
    const fs::path sdl = config_dir / "sdl2-config.ini";
    const fs::path qt = config_dir / "qt-config.ini";
    std::error_code ec;
    bool fresh = false;
    if (!fs::exists(sdl, ec)) {
        bool copied = fs::exists(qt, ec);
        if (copied) {
            copied = fs::copy_file(qt, sdl, fs::copy_options::overwrite_existing, ec) && !ec;
        }
        if (!copied) {
            say("");
            say(" [Anka] No settings yet: neither config\\sdl2-config.ini nor config\\qt-config.ini in");
            say("           \"" + Utf8(config_dir) + "\".");
            say("           Start anka.exe once and set up your controller there, then start this again.");
            say("");
            result.go = false;
            return result;
        }
        fresh = true;
        say(" [Anka] config\\sdl2-config.ini was made from qt-config.ini: the controller and settings of");
        say("           the menu program (anka.exe) are used.");
    }
    if (profile == Profile::Safe) {
        return result;
    }
    if (profile == Profile::Recommended) {
        if (fresh) {
            if (SetKeys(sdl, ReactiveOff)) {
                say(" [Anka] Reactive flushing off in the new copy (the measured setup).");
                return result;
            }
            say(" [Anka] Could not edit \"" + Utf8(sdl) + "\".");
        }
        // Only read: a file that cannot be read gives no note.
        if (const auto bytes = ReadFile(sdl); bytes && ReactiveFlushingOn(ParseIni(*bytes))) {
            say(" [Anka] Note: reactive flushing is on in your sdl2-config.ini; this launcher was measured with");
            say("           it off (README_ANKA.md, \"Reactive flushing\"). The file was not changed.");
        }
        return result;
    }
    // Reactive.
    if (fresh && !SetKeys(sdl, ReactiveOff)) {
        say(" [Anka] Could not edit \"" + Utf8(sdl) + "\".");
    }
    const fs::path reactive = config_dir / "sdl2-config-reactive.ini";
    if (!fs::exists(reactive, ec)) {
        const bool made = fs::copy_file(sdl, reactive, fs::copy_options::overwrite_existing, ec) &&
                          !ec;
        bool edited = false;
        if (made) {
            edited = SetKeys(reactive, ReactiveOn);
            if (!edited) {
                say(" [Anka] Could not edit \"" + Utf8(reactive) + "\".");
            }
        }
        if (!edited) {
            // The half-made copy is this program's own (it did not exist above): removed, tried
            // again next time. The start stops (exit 1, the launcher keeps its window open): a
            // game started with sdl2-config.ini instead would not be the reactive setup.
            fs::remove(reactive, ec);
            say(" [Anka] config\\sdl2-config-reactive.ini could not be made: the game was not started.");
            say("           Check that the user folder can be written, or set CONFIG in this .bat.");
            result.go = false;
            return result;
        }
        say(" [Anka] config\\sdl2-config-reactive.ini made (reactive flushing on).");
    }
    result.config = reactive;
    return result;
}

} // namespace AnkaProfile
