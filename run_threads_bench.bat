@echo off
rem Thread scaling of the search: the same positions with 1, 2, 4, 8 threads
rem (and all cores, if more). Each search runs a fixed time; compare the
rem expansions and the completed depth on the first line of each result.
rem
rem   run_threads_bench.bat [seconds] [bot]
rem   default: 30 s, goal-sel-run-geo-cover-timed-b8-t90
rem
rem Results: build\threads_bench\results.txt
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "SSBOT=build\ssbot.exe"
if not exist "%SSBOT%" (
    echo [ERROR] %SSBOT% not found. Build first: Ctrl+Shift+B or build_msvc.bat
    exit /b 1
)
set "SECONDS=%~1"
if "%SECONDS%"=="" set "SECONDS=30"
set "BOT=%~2"
if "%BOT%"=="" set "BOT=goal-sel-run-geo-cover-timed-b8-t90"

set "THREADS=1 2 4 8"
if %NUMBER_OF_PROCESSORS% GTR 8 set "THREADS=1 2 4 8 %NUMBER_OF_PROCESSORS%"

if not exist build\threads_bench mkdir build\threads_bench
set "OUT=build\threads_bench\results.txt"
echo Thread scaling: %BOT%, %SECONDS% s per search, threads %THREADS%, %NUMBER_OF_PROCESSORS% logical processors> "%OUT%"
echo Started %DATE% %TIME%>> "%OUT%"

rem Three positions of the v4 suite (tests\bot_suite_cases_v4.json).
set "P1=b 3 0-0 e12/f11 d13/d11 f13/h11 e8/f7 f12 h13/i12 c12 f16 @W4 t:W"
set "N1=g9_04b_defence"
set "P2=w 12 0-0 j16/j14 h18/g16 h13/g13 e14/e12 a7 i15/j13 f14/f13 g16/g15 @W2 t:W"
set "N2=tree_13w_pass"
set "P3=b 46 0-0 e11 e10/d10 i8 d9/c9 j10/i10 h11/i12 e12/d11 c13/b12 @B2 t:B"
set "N3=g8_47b_midfield"

for %%i in (1 2 3) do (
    for %%t in (%THREADS%) do (
        echo Running !N%%i!, %%t threads, %SECONDS% s ...
        echo.>> "%OUT%"
        echo === !N%%i!, %%t threads>> "%OUT%"
        "%SSBOT%" search "!P%%i!" --bot %BOT% --time %SECONDS% --threads %%t >> "%OUT%" 2>&1
    )
)
echo.>> "%OUT%"
echo Finished %DATE% %TIME%>> "%OUT%"
echo.
echo Done. Results: %OUT%
endlocal
