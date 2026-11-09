// Border of a run: one runner against one defender (06.11.2026, fixed 08.11.2026).
//
// THIS PROCEDURE IS FIXED BY THE USER'S DECISION (08.11.2026). Do not change its
// rules; TestRunBorder47b in tests/ball_race_tests.cpp pins the g8_47b result
// (B2 g7-g5-g3-g1 against W4 f6-f4-f2: the border runs between files f and g).
//
// Rules (docs/session_log.md, 06.11.2026 "47b: граница рывка для всех игроков"):
//  - only the runner and the defender move; the other six stand and block
//    (engine geometry: MoveDestinations, inertia and MovePathBlocked);
//  - the teams alternate starting with the side to move in the position;
//  - the runner may change file but must keep the full advance towards the
//    goal line his inertia allows (the best advance among the cells of his
//    3x3 square inside the field, blocking ignored);
//  - the defender chooses among all his legal destinations;
//  - a branch ends when the runner stands on the last row before the goal
//    line or at the first forced loss of tempo (no legal cell with the full
//    advance); the runner keeps the cell he stands on;
//  - minimax: first the row reached (nearer the goal line is better for the
//    runner), with equal rows the file nearer the central file f.
// Unlike the reach map (reach_front.h) this is an exact two-player game, so it
// has no interleaving of the two waves on one file.
#pragma once

#include "engine/state.h"

#include <vector>

namespace soccer {
namespace bot {

struct RunBorderResult {
    bool valid = false;
    int rowsLeft = 0;      // rows between the runner's final cell and the goal line (0 = last row)
    int fileOffset = 0;    // |x - centre file| of the final cell
    // Principal variation: alternating destinations, starting with the side to move.
    std::vector<int> movers;     // player of each step
    std::vector<Cell> cells;     // destination of each step
    long nodes = 0;
};

RunBorderResult SolveRunBorder(const GameState& s, int runner, int defender);

}  // namespace bot
}  // namespace soccer
