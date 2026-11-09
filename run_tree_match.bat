@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem Match: goal-sel-tree-fb-b8-t120 (a best-first tree over the candidates and
rem static estimate of goal-sel-run-fb-b8, the ball team's candidates by the
rem ball's category; docs/bot_design.md 3.3) against goal-sel-run-fb-b8-t120
rem (the fixed-depth beam with the same candidates and static). 120 s per team half-turn, one thread each.
rem Two episodes from the 0-0 kickoff, the colours swapped, 22 team half-turns
rem (11 minutes of play) each: at most about 45 minutes per episode, 1.5 hours in all.
rem Re-run the file to resume: finished episodes are skipped.
rem   run_tree_match.bat --list    print the episode names only
rem   run_tree_match.bat --smoke   one very short episode in a separate folder
set "ROOT=%~dp0"
set "SSBOT=%ROOT%build\ssbot.exe"
set "OUT=%ROOT%games\tree_fb_match"
set "NEW=goal-sel-tree-fb-b8-t120"
set "OLD=goal-sel-run-fb-b8-t120"
set "EPISODE_TURNS=22"
set "TIME_OVERRIDE="
set "PAUSE_AT_END=1"
set "LIST_ONLY=0"

if /I "%~1"=="--list" (
    set "LIST_ONLY=1"
    set "PAUSE_AT_END=0"
)
if /I "%~1"=="--smoke" (
    set "OUT=%ROOT%games\tree_fb_match_smoke"
    set "EPISODE_TURNS=2"
    set "TIME_OVERRIDE=--wtime 0.5 --btime 0.5"
    set "PAUSE_AT_END=0"
)

if not exist "%SSBOT%" (
    echo Missing %SSBOT%
    echo Run build_msvc.bat first ^(Ctrl+Shift+B^).
    goto :failed
)
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 goto :failed
if "%LIST_ONLY%"=="0" (
    rem One copy of the bot for the whole match: a later build does not mix versions.
    if not exist "%OUT%\ssbot.exe" copy /Y "%SSBOT%" "%OUT%\ssbot.exe" >nul
    if not exist "%OUT%\ssbot.exe" goto :failed
    set "SSBOT=%OUT%\ssbot.exe"
    "!SSBOT!" bots > "%OUT%\bots.txt" 2>&1
    findstr /B /C:"%NEW% " "%OUT%\bots.txt" >nul
    if errorlevel 1 (
        echo %NEW% is not in this ssbot.exe: rebuild with build_msvc.bat,
        echo then delete %OUT%\ssbot.exe and start again.
        goto :failed
    )
)

echo Output: %OUT%
echo %NEW% against %OLD%, 120 s per team half-turn,
echo two episodes of %EPISODE_TURNS% team half-turns from the kickoff, colours swapped.
echo Finished records can be opened in soccer.exe while this window keeps running.

call :game W
if errorlevel 1 goto :failed
if /I "%~1"=="--smoke" (
    echo Smoke test complete.
    exit /b 0
)
call :game B
if errorlevel 1 goto :failed

if "%LIST_ONLY%"=="1" exit /b 0
set "FILES="
for %%f in ("%OUT%\*.ssn") do set FILES=!FILES! "%%f"
if defined FILES "%SSBOT%" stats !FILES! > "%OUT%\stats.txt" 2>&1
echo.
echo Match complete. Summary: %OUT%\stats.txt
type "%OUT%\stats.txt"
if "%PAUSE_AT_END%"=="1" pause
exit /b 0

rem ---- %1 colour of the new bot
:game
set "ID=tree%~1_vs_fb_kick"
if "%LIST_ONLY%"=="1" (
    echo %ID%
    exit /b 0
)
if /I "%~1"=="W" (
    set "WHITE=%NEW%"
    set "BLACK=%OLD%"
) else (
    set "WHITE=%OLD%"
    set "BLACK=%NEW%"
)
if exist "%OUT%\%ID%.ssn" if exist "%OUT%\%ID%.csv" (
    "%SSBOT%" replay "%OUT%\%ID%.ssn" >nul 2>&1
    if not errorlevel 1 (
        echo SKIP %ID%
        exit /b 0
    )
)
echo.
echo START %ID%: White !WHITE!, Black !BLACK!  ^(%date% %time%^)
"%SSBOT%" selfplay --white !WHITE! --black !BLACK! --episode-turns %EPISODE_TURNS% --seed 7 --threads 1 --out "%OUT%\%ID%.ssn" --trace "%OUT%\%ID%.csv" --silent %TIME_OVERRIDE%
if errorlevel 1 exit /b 1
"%SSBOT%" replay "%OUT%\%ID%.ssn"
if errorlevel 1 exit /b 1
echo DONE %ID%
exit /b 0

:failed
echo.
echo The run stopped after an error. Finished records remain in %OUT%.
echo Run this file again to resume.
if "%PAUSE_AT_END%"=="1" pause
exit /b 1
