@echo off
setlocal
rem Human (White) against goal-sel-tree-fb-b8-t120 (Black), 120 s per team half-turn:
rem a best-first tree, the ball team's candidates by the ball's category, the free ball
rem by the race from the state as it is (docs/bot_design.md 3.3, 4.2). The game is written after every action to build\live_tree_game.ssn.
set "ROOT=%~dp0"
if not exist "%ROOT%build\soccer.exe" (
    echo Missing soccer.exe. Run build_msvc.bat first.
    pause
    exit /b 1
)
start "" /D "%ROOT%" "%ROOT%build\soccer.exe" --black goal-sel-tree-fb-b8-t120 --live-record build/live_tree_game.ssn
