@echo off
setlocal DisableDelayedExpansion
rem Anka - RECOMMENDED: the settings measured best on our PC (Donkey Kong Bananza).
rem Anka launcher for anka-cli.exe (Anka's program without menus).
rem Put this file next to anka-cli.exe.
rem Start: put your game (.nsp .xci .nsz .xcz .nup) in the Games folder next to this file and double-click
rem this file; or drag a game file onto it; or write the game's path in GAME below.
rem This file only finds the game and starts anka-cli.exe --profile recommended; anka-cli itself sets the
rem switches of this setup and prepares its settings (README_ANKA.md, "Launchers"). First start: when
rem the user folder has no config\sdl2-config.ini yet, anka-cli makes it as a copy of
rem config\qt-config.ini (the controller and settings of the menu program, anka.exe). An existing
rem sdl2-config.ini is never changed. Quit the game with Alt+F4.
rem Bananza-specific settings: DK_DYN below and the frame pacing; for other games use start-safe.bat.
rem ---- your settings -------------------------------------------------------------------
rem GAME: the game to start (a drag-and-drop file wins over this line, this line over Games).
set "GAME="
rem CONFIG: a separate sdl2-config .ini to use; empty = automatic (above, README).
set "CONFIG="
rem FULLSCREEN: -f starts in full screen; empty = window.
set "FULLSCREEN=-f"
rem DK_DYN: 1 = Bananza dynamic frame rate patch (in memory, README), 0 = off.
set "DK_DYN=1"
rem OSD: 1 = FPS / speed overlay through RivaTuner Statistics Server (if running).
set "OSD=1"
rem ---------------------------------------------------------------------------------------
set "HERE=%~dp0"
if not exist "%HERE%anka-cli.exe" goto noexe
if not "%~1"=="" set "GAME=%~f1"
cd /d "%HERE%"
if defined GAME goto havegame
call :findgames
if "%NGAMES%"=="0" goto nogame
if "%NGAMES%"=="1" goto havegame
goto choose
:havegame
if not exist "%GAME%" goto gamemissing
if not defined CONFIG goto configdone
if not exist "%CONFIG%" goto noconfig
:configdone
if defined CONFIG goto withconfig
"%HERE%anka-cli.exe" --profile recommended --dk-dyn "%DK_DYN%" --osd "%OSD%" %FULLSCREEN% -g "%GAME%"
goto ran
:withconfig
"%HERE%anka-cli.exe" --profile recommended --dk-dyn "%DK_DYN%" --osd "%OSD%" -c "%CONFIG%" %FULLSCREEN% -g "%GAME%"
:ran
rem Keep the window open when the emulator stopped with an error (any non-zero code, a crash's
rem is negative, 1 also when there were no settings yet), so it can be read.
if not "%errorlevel%"=="0" pause
goto end
:nogame
echo.
echo  No game found. Put your game file (.nsp .xci .nsz .xcz .nup) in the Games folder next to
echo  this file, or drag it onto this .bat.
echo.
pause
exit /b 1
:gamemissing
echo.
echo  The game file was not found:
echo  "%GAME%"
echo.
pause
exit /b 1
:noconfig
echo.
echo  The CONFIG file in this .bat was not found:
echo  "%CONFIG%"
echo  anka-cli would make a new one with default settings. Fix the CONFIG line
echo  (right click, Edit) or empty it.
echo.
pause
exit /b 1
:noexe
echo.
echo  anka-cli.exe is not next to this .bat. Put both in the Anka folder (README_ANKA.md).
echo.
pause
exit /b 1

rem ---- the game list: the Games folder next to this file, subfolders too ----------------
:findgames
set "NGAMES=0"
if not exist "%HERE%Games\" exit /b 0
for /r "%HERE%Games" %%F in (*.nsp *.xci *.nsz *.xcz *.nup) do (
  set "F=%%~fF"
  call :addgame
)
exit /b 0
:addgame
set /a NGAMES+=1
if "%NGAMES%"=="1" set "GAME=%F%"
exit /b 0
:choose
echo.
echo  Games in the Games folder:
set "I=0"
for /r "%HERE%Games" %%F in (*.nsp *.xci *.nsz *.xcz *.nup) do (
  set "F=%%~nxF"
  call :showgame
)
echo.
:ask
set "SEL="
set /p "SEL= Number (1-%NGAMES%; Enter = quit): "
if not defined SEL goto end
set "SEL=%SEL:"=%"
set "PICK="
for /l %%I in (1,1,%NGAMES%) do if "%SEL%"=="%%I" set "PICK=%%I"
if not defined PICK goto ask
set "I=0"
for /r "%HERE%Games" %%F in (*.nsp *.xci *.nsz *.xcz *.nup) do (
  set "F=%%~fF"
  call :pickgame
)
goto havegame
:showgame
set /a I+=1
setlocal EnableDelayedExpansion
echo   !I!. !F!
endlocal
exit /b 0
:pickgame
set /a I+=1
if "%I%"=="%PICK%" set "GAME=%F%"
exit /b 0

:end
endlocal
