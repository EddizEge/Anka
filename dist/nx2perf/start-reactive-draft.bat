@echo off
setlocal
rem NX2Perf - REACTIVE (DRAFT): recommended + reactive flushing + RX_RT_FIT=1. Not measured as a whole.
rem NX2Perf launcher for eden-cli-nx2perf.exe (command line / SDL front end of PokeDinner-Eglantine).
rem Put this file next to eden-cli-nx2perf.exe, in your PokeDinner-Eglantine 2.0.0 folder.
rem Start: drag your game file onto this .bat, or write its path in GAME below.
rem Reactive flushing copies GPU-written data back to the game. Earlier builds: +13 to +23 percent
rem FPS in open areas, -23 to -30 percent at the game's opening. With RX_RT_FIT=1 (this release)
rem the opening was still -20 percent in the first runs; open areas and frame generation were
rem not measured with it (README, "Reactive flushing"). It is a config setting, not a switch:
rem make a copy of your sdl2-config.ini with use_reactive_flushing=true and
rem barrier_feedback_loops=true, and put its path in CONFIG below.
rem ---- your settings -------------------------------------------------------------------
rem GAME: the game to start (a drag-and-drop file wins over this line).
set "GAME="
rem CONFIG: a separate sdl2-config .ini to use; empty = eden-cli's own config (README).
set "CONFIG="
rem FULLSCREEN: -f starts in full screen; empty = window.
set "FULLSCREEN=-f"
rem DK_DYN: 1 = Bananza dynamic frame rate patch (in memory, README), 0 = off.
set "DK_DYN=1"
rem OSD: 1 = FPS / speed overlay through RivaTuner Statistics Server (if running).
set "OSD=1"
rem ---------------------------------------------------------------------------------------
if not "%~1"=="" set "GAME=%~1"
if not defined GAME goto nogame
if not exist "%~dp0eden-cli-nx2perf.exe" goto noexe
if defined CONFIG if not exist "%CONFIG%" goto noconfig
cd /d "%~dp0"
rem Opt-in switches this launcher does not use: cleared.
set EDEN_NX2_PRESENT_INTERVAL=
set EDEN_NX2_MODE_GOV=
set EDEN_NX2_DK_DYN_MIN_FPS=
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
set EDEN_NX2_MODE_DIAG_MAX=
set EDEN_NX2_PACE_ONE=
set EDEN_NX2_MEMBAR=
set EDEN_NX2_BACK_CORE=
set EDEN_NX2_FRONT_CORE=
set EDEN_NX2_FG_DIAG=
set EDEN_NX2_FLUSH_FIRST_READ=
set EDEN_NX2_READ_LATCH_AGE=
set EDEN_NX2_RX_ONE_SUBMIT=
set EDEN_NX2_RX_COALESCE=
set EDEN_NX2_RX_PITCH_FORCED=
set EDEN_NX2_RX_MASKED_TARGETS=
set EDEN_NX2_REACTIVE_STATS=
set EDEN_NX2_RX_NO_COPYBACK=
set EDEN_NX2_RX_NO_RT_LATCH=
rem The recommended switches (README_NX2PERF.md, "Launchers").
rem Split GPU thread: the back as stages (cut a), acquire peek, leaner front macro path.
set EDEN_NX2_BACK_STAGES=1
set EDEN_NX2_BACK_CUTS=a
set EDEN_NX2_ACQUIRE_PEEK=1
set EDEN_NX2_JIT_SAME=1
set EDEN_NX2_FRONT_MACRO_LEAN=1
rem Donkey Kong Bananza: follow the game's own 60/30/20 fps choice at the right speed.
set EDEN_NX2_PRESENT_INTERVAL_WORD=12:16:4
set EDEN_NX2_MODE_PACE=2
set EDEN_NX2_PACE_SPIN_US=1500
rem Donkey Kong Bananza dynamic frame rate (DK_DYN above; 0 = the game's own 60/30 rule).
set EDEN_NX2_DK_DYN=%DK_DYN%
rem FSR3 frame generation: generated frames blended for the time they are shown (only
rem does something when frame generation is on in the config, README).
set EDEN_NX2_FG_TIMED=1
rem Game FPS, display FPS and speed in RivaTuner Statistics Server's overlay (OSD above).
set EDEN_NX2_OSD=%OSD%
rem Reactive flushing: Bananza's opening renders into one image instead of a copy per draw
rem (needs barrier_feedback_loops=true in the CONFIG file). Do not add RX_MASKED_TARGETS
rem (known defect).
set EDEN_NX2_RX_RT_FIT=1
if defined CONFIG goto withconfig
"%~dp0eden-cli-nx2perf.exe" %FULLSCREEN% -g "%GAME%"
goto ran
:withconfig
"%~dp0eden-cli-nx2perf.exe" -c "%CONFIG%" %FULLSCREEN% -g "%GAME%"
:ran
rem Keep the window open when the emulator stopped with an error, so it can be read.
if errorlevel 1 pause
goto end
:nogame
echo.
echo  No game given. Drag your game file onto this .bat, or write its path
echo  in the GAME line at the top of it (right click, Edit).
echo.
pause
exit /b 1
:noconfig
echo.
echo  The CONFIG file in this .bat was not found:
echo  "%CONFIG%"
echo  eden-cli would make a new one with default settings. Fix the CONFIG line
echo  (right click, Edit) or empty it.
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
