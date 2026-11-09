@echo off
rem Builds Strategic Soccer with MSVC (Visual Studio / Build Tools):
rem   build\soccer.exe       - the game
rem   build\engine_tests.exe - engine tests (no Windows dependencies)
rem   build\bot_tests.exe    - bot (search) tests, only with --bot-tests
rem   build\ball_race_tests.exe - free-ball race solver tests (run: build\ball_race_tests.exe)
rem   build\two_vs_one_tests.exe - 2 v 1 lower bound against an exact search (run: build\two_vs_one_tests.exe)
rem   build\ssbot.exe        - bot command-line tool (optimized build)
rem The compiler output is kept in build\build.log.
setlocal EnableDelayedExpansion
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto novs

:build
if not exist build\obj\game mkdir build\obj\game
if not exist build\obj\tests mkdir build\obj\tests
if not exist build\obj\bottests mkdir build\obj\bottests
if not exist build\obj\tool mkdir build\obj\tool
if not exist build\obj\racetests mkdir build\obj\racetests
if not exist build\obj\twotests mkdir build\obj\twotests
if not exist build\obj\gamebot mkdir build\obj\gamebot
if not exist build\obj\enginedbg mkdir build\obj\enginedbg
if not exist build\obj\engineopt mkdir build\obj\engineopt
set FLAGS=/nologo /std:c++17 /EHsc /permissive- /W4 /Zi /Od /utf-8 /Isrc /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /MP2 /FS

rem The search is slow without optimization: the bot code is always built with /O2.
set RFLAGS=%FLAGS:/Od=/O2% /DNDEBUG

set "BOT_OBJS="
for %%f in (src\bot\*.cpp) do set "BOT_OBJS=!BOT_OBJS! build\obj\gamebot\%%~nf.obj"
set "ENGINE_DBG_OBJS="
set "ENGINE_OPT_OBJS="
for %%f in (src\engine\*.cpp) do (
    set "ENGINE_DBG_OBJS=!ENGINE_DBG_OBJS! build\obj\enginedbg\%%~nf.obj"
    set "ENGINE_OPT_OBJS=!ENGINE_OPT_OBJS! build\obj\engineopt\%%~nf.obj"
)

echo === shared objects > build\build.log
cl %RFLAGS% /c src\bot\*.cpp /Fobuild\obj\gamebot\ /Fdbuild\obj\gamebot\ >> build\build.log 2>&1
if errorlevel 1 goto failed
cl %FLAGS% /c src\engine\*.cpp /Fobuild\obj\enginedbg\ /Fdbuild\obj\enginedbg\ >> build\build.log 2>&1
if errorlevel 1 goto failed
cl %RFLAGS% /c src\engine\*.cpp /Fobuild\obj\engineopt\ /Fdbuild\obj\engineopt\ >> build\build.log 2>&1
if errorlevel 1 goto failed

echo === game >> build\build.log
cl %FLAGS% src\ui\*.cpp !ENGINE_DBG_OBJS! !BOT_OBJS! /Fobuild\obj\game\ /Fdbuild\obj\game\ /Febuild\soccer.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comdlg32.lib >> build\build.log 2>&1
if errorlevel 1 goto failed

echo === engine tests >> build\build.log
cl %FLAGS% tests\engine_tests.cpp !ENGINE_DBG_OBJS! /Fobuild\obj\tests\ /Fdbuild\obj\tests\ /Febuild\engine_tests.exe >> build\build.log 2>&1
if errorlevel 1 goto failed

echo === ball race tests >> build\build.log
cl %RFLAGS% tests\ball_race_tests.cpp !ENGINE_OPT_OBJS! !BOT_OBJS! /Fobuild\obj\racetests\ /Fdbuild\obj\racetests\ /Febuild\ball_race_tests.exe >> build\build.log 2>&1
if errorlevel 1 goto failed

echo === 2 v 1 tests >> build\build.log
cl %RFLAGS% tests\two_vs_one_tests.cpp !ENGINE_OPT_OBJS! !BOT_OBJS! /Fobuild\obj\twotests\ /Fdbuild\obj\twotests\ /Febuild\two_vs_one_tests.exe >> build\build.log 2>&1
if errorlevel 1 goto failed

if /I "%~1"=="--bot-tests" (
    echo === bot tests >> build\build.log
    cl %RFLAGS% tests\bot_tests.cpp !ENGINE_OPT_OBJS! !BOT_OBJS! /Fobuild\obj\bottests\ /Fdbuild\obj\bottests\ /Febuild\bot_tests.exe >> build\build.log 2>&1
    if errorlevel 1 goto failed
) else (
    echo === bot tests skipped >> build\build.log
)

echo === ssbot >> build\build.log
cl %RFLAGS% tools\ssbot.cpp !ENGINE_OPT_OBJS! !BOT_OBJS! /Fobuild\obj\tool\ /Fdbuild\obj\tool\ /Febuild\ssbot.exe >> build\build.log 2>&1
if errorlevel 1 goto failed

type build\build.log
exit /b 0

:failed
type build\build.log
exit /b 1

:novs
echo [ERROR] MSVC C++ compiler cl.exe not found.
echo The VS Code C/C++ extension does NOT include a compiler - it must be installed separately:
echo   winget install --id Microsoft.VisualStudio.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
echo Then restart VS Code.
exit /b 1
