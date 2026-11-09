@echo off
rem Suite v4 (tests\bot_suite_cases_v4.json, all 10 positions): the current
rem default goal-sel-run-geo-cover-timed against -rb- whose beam also ranks
rem the decisions that free the ball by the race (goal-sel-run-geo-cover-timed-rbr;
rem docs\session_log.md, 01.11.2026). One full team turn per position and budget,
rem one thread, seed 7. About 1 hour 40 minutes with the default budgets.
rem
rem   run_rbr_suite.bat               budgets 120 and 180 s
rem   run_rbr_suite.bat 90,180        other budgets (comma-separated, no spaces)
rem
rem Results: build\rbr_suite\summary.csv (moves, time, depth, expansions; a repeated
rem run writes build\rbr_suite_2, _3, ...),
rem the games (.ssn) and traces (.csv) next to it.
setlocal
cd /d "%~dp0"
if not exist build\ssbot.exe (
    echo [ERROR] build\ssbot.exe not found. Build first: Ctrl+Shift+B or build_msvc.bat
    exit /b 1
)
set "BUDGETS=%~1"
if "%BUDGETS%"=="" set "BUDGETS=120,180"
set "BASE=goal-sel-run-geo-cover-timed-b8-t120"
set "CAND=goal-sel-run-geo-cover-timed-rbr-b8-t120"
set "OUT=build\rbr_suite"
rem A finished run is kept: a new run goes to rbr_suite_2, _3, ...
set /a N=1
:next
if exist "%OUT%" (
    set /a N+=1
    call set "OUT=build\rbr_suite_%%N%%"
    goto next
)

echo Suite v4: %BASE% against %CAND%, budgets %BUDGETS% s
echo Results: %OUT%\summary.csv
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%~dp0tools\run_bot_suite.ps1' -All -Baseline '%BASE%' -Candidate '%CAND%' -Budgets %BUDGETS% -OutputDirectory '%OUT%'"
if errorlevel 1 (
    echo [ERROR] the suite stopped with an error, see the messages above
    exit /b 1
)
echo.
echo Done. Results: %OUT%\summary.csv
endlocal
