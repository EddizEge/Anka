@echo off
setlocal
rem NX2Perf - SAFE: every switch this fork added turned off (as close to 2.0.0 as it gets).
rem NX2Perf launcher for eden-cli-nx2perf.exe (command line / SDL front end of PokeDinner-Eglantine).
rem Put this file next to eden-cli-nx2perf.exe, in your PokeDinner-Eglantine 2.0.0 folder.
rem Start: drag your game file onto this .bat, or write its path in GAME below.
rem Use it to tell whether a problem comes from the new switches. Changes that have no
rem switch stay in (CHANGELOG_NX2PERF.md).
rem ---- your settings -------------------------------------------------------------------
rem GAME: the game to start (a drag-and-drop file wins over this line).
set "GAME="
rem CONFIG: a separate sdl2-config .ini to use; empty = eden-cli's own config (README).
set "CONFIG="
rem FULLSCREEN: -f starts in full screen; empty = window.
set "FULLSCREEN=-f"
rem ---------------------------------------------------------------------------------------
if not "%~1"=="" set "GAME=%~1"
if not defined GAME goto nogame
if not exist "%~dp0eden-cli-nx2perf.exe" goto noexe
cd /d "%~dp0"
rem Opt-in switches: cleared (off).
set EDEN_NX2_BACK_STAGES=
set EDEN_NX2_BACK_CUTS=
set EDEN_NX2_ACQUIRE_PEEK=
set EDEN_NX2_JIT_SAME=
set EDEN_NX2_FRONT_MACRO_LEAN=
set EDEN_NX2_PRESENT_INTERVAL=
set EDEN_NX2_PRESENT_INTERVAL_WORD=
set EDEN_NX2_MODE_PACE=
set EDEN_NX2_PACE_SPIN_US=
set EDEN_NX2_MODE_GOV=
set EDEN_NX2_DK_DYN=
set EDEN_NX2_DK_DYN_MIN_FPS=
set EDEN_NX2_FG_TIMED=
set EDEN_NX2_OSD=
set EDEN_NX2_BACK_CPU=
set EDEN_NX2_SEMEXEC_DEFER=
set EDEN_NX2_SKIP_EMPTY_DRAWS=
set EDEN_NX2_ASYNC_SKIP=
set EDEN_NX2_VBLANK_GRID=
set EDEN_NX2_VRR_VSYNC=
set EDEN_NX2_GPU_BUSY_CLOCK=
set EDEN_NX2_GPU_TICK_DIV=
set EDEN_NX2_PACE_GUEST_SLEEP=
set EDEN_NX2_PRESENT_RATE=
set EDEN_NX2_PRESENT_DIAG=
set EDEN_NX2_MODE_DIAG=
rem Shortcuts that are on by default: off (=0, the code path of 2.0.0).
set EDEN_NX2_LIGHT_DRAW=0
set EDEN_NX2_LIGHT_UNIFORM=0
set EDEN_NX2_LIGHT_UNIFORM_VALUES=0
set EDEN_NX2_LIGHT_STORAGE=0
set EDEN_NX2_UNIFORM_SYNC_SKIP=0
set EDEN_NX2_STORAGE_KEEP=0
set EDEN_NX2_UBO_REUSE=0
set EDEN_NX2_UBO_DIRECT=0
set EDEN_NX2_UBO_STAGE=0
set EDEN_NX2_UBO_BIND_SKIP=0
set EDEN_NX2_SSBO_FAST=0
set EDEN_NX2_SSBO_MEMO=0
set EDEN_NX2_SYNC_SETS=0
set EDEN_NX2_SYNC_SKIP=0
set EDEN_NX2_SYNC_EXACT=0
set EDEN_NX2_SYNC_MEMO=0
set EDEN_NX2_VERTEX_SKIP=0
set EDEN_NX2_VERTEX_FAST=0
set EDEN_NX2_VERTEX_CLEAN=0
set EDEN_NX2_LIGHT_SYNC_SKIP=0
set EDEN_NX2_INDEX_SKIP=0
set EDEN_NX2_INLINE_FIX=0
set EDEN_NX2_TOUCH_SKIP=0
set EDEN_NX2_TEX_TOUCH_SKIP=0
set EDEN_NX2_ALIAS_LAZY=0
set EDEN_NX2_USAGE_SKIP=0
set EDEN_NX2_USAGE_RESET_SKIP=0
set EDEN_NX2_GPU_WRITTEN_FILTER=0
set EDEN_NX2_MARK_WORDS=0
set EDEN_NX2_INVAL_HULL=0
set EDEN_NX2_INVAL_SKIP=0
set EDEN_NX2_SMALL_WRITE=0
set EDEN_NX2_RANGE_SKIP=0
set EDEN_NX2_SLOT_SKIP=0
set EDEN_NX2_QUERY_BOUNDS=0
set EDEN_NX2_SHADER_BOUNDS=0
set EDEN_NX2_DESC_REUSE=0
set EDEN_NX2_DESC_HULL=0
set EDEN_NX2_DESC_SECONDARY=0
set EDEN_NX2_DESC_XLAT=0
set EDEN_NX2_KEY_CMP=0
set EDEN_NX2_NOTIFY_LAZY=0
set EDEN_NX2_RESET_BATCH=0
set EDEN_NX2_RESET_LAST=0
set EDEN_NX2_RESET_RUNS=0
set EDEN_NX2_RESET_SORT=0
set EDEN_NX2_WRITE_BATCH=0
set EDEN_NX2_BATCH_DIRECT=0
set EDEN_NX2_CB_RUNS=0
set EDEN_NX2_TICK_SKIP=0
set EDEN_NX2_HLE_INDIRECT=0
set EDEN_NX2_INDIRECT_BATCH=0
set EDEN_NX2_LAYOUT_MEMO=0
set EDEN_NX2_SAME_SKIP=0
set EDEN_NX2_HSETP2_REG=0
set EDEN_NX2_DIRTY_THUNK_SKIP=0
set EDEN_NX2_FAST_PRIORITY_SCAN=0
set EDEN_NX2_PBFLUSH_WINDOW_KIB=0
set EDEN_NX2_UPSCALE_SCAN=0
set EDEN_NX2_UPSCALE_LEARN=0
set EDEN_NX2_UPSCALE_HEADER=0
if defined CONFIG goto withconfig
"%~dp0eden-cli-nx2perf.exe" %FULLSCREEN% -g "%GAME%"
goto end
:withconfig
"%~dp0eden-cli-nx2perf.exe" -c "%CONFIG%" %FULLSCREEN% -g "%GAME%"
goto end
:nogame
echo.
echo  No game given. Drag your game file onto this .bat, or write its path
echo  in the GAME line at the top of it (right click, Edit).
echo.
pause
exit /b 1
:noexe
echo.
echo  eden-cli-nx2perf.exe is not next to this .bat. Put both in your
echo  PokeDinner-Eglantine 2.0.0 folder (README_NX2PERF.md).
echo.
pause
exit /b 1
:end
endlocal
