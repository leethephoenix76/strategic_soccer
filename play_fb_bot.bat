@echo off
setlocal
rem Human (White) against goal-sel-run-fb-b8-t120 (Black), 120 s per team half-turn:
rem goal-sel-run-ep-b8 with the free ball by the race from the state as it is (docs/bot_design.md 4.2).
rem The game is written after every action to build\live_fb_game.ssn.
set "ROOT=%~dp0"
if not exist "%ROOT%build\soccer.exe" (
    echo Missing soccer.exe. Run build_msvc.bat first.
    pause
    exit /b 1
)
start "" /D "%ROOT%" "%ROOT%build\soccer.exe" --black goal-sel-run-fb-b8-t120 --live-record build/live_fb_game.ssn
