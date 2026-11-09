@echo off
setlocal
rem Human (White) against goal-sel-run-geo-cover-timed-b8-t120 (Black), 120 s per team half-turn:
rem Geometric coverage static, turn order and timed run blocking (docs/bot_design.md 4.3).
rem The game is written after every action to build\live_timed_game.ssn.
set "ROOT=%~dp0"
if not exist "%ROOT%build\soccer.exe" (
    echo Missing soccer.exe. Run build_msvc.bat first.
    pause
    exit /b 1
)
start "" /D "%ROOT%" "%ROOT%build\soccer.exe" --black goal-sel-run-geo-cover-timed-b8-t120 --live-record build/live_timed_game.ssn
