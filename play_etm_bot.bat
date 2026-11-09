@echo off
setlocal
rem Human (White) against the experimental bot goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-etm-b8-t120 (Black),
rem 120 s per team half-turn (docs/bot_design.md 5). The game is written after every action
rem to games\selective_league\game_etm_live.ssn.
set "ROOT=%~dp0"
if not exist "%ROOT%build\soccer.exe" (
    echo Missing soccer.exe. Run build_msvc.bat first.
    pause
    exit /b 1
)
start "" /D "%ROOT%" "%ROOT%build\soccer.exe" --black goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-etm-b8-t120 --live-record games/selective_league/game_etm_live.ssn
