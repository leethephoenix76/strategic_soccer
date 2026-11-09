// Reachability with inertia: in how many turns a player can get to each cell.
//
// The movement state of a player is (cell, velocity): his destinations are the
// 3x3 square around cell + sign(velocity) (docs/game_model.md, "Движение игроков").
// A breadth-first search over these states (209 cells x 25 velocities) gives the
// minimum number of turns to every cell. Other players are ignored: the tables
// describe what a player can do, not what the position allows.
#pragma once

#include "engine/state.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace soccer {
namespace bot {

// Reverses a list of cells enumerated in the order of x, then y, for Black: the
// order of the mirrored field, so that ties are broken alike for both teams.
inline void TeamOrder(std::vector<Cell>& cells, int team) {
    if (team == kBlack) std::reverse(cells.begin(), cells.end());
}

constexpr int kUnreachable = 255;

struct ReachMap {
    uint8_t turns[kFieldWidth][kGridHeight];   // minimum turns to the cell (0 = his cell), kUnreachable if never

    int Turns(Cell c) const { return turns[c.x][c.y]; }
};

// Reach of a player standing in `cell` with velocity `velocity` (from the previous turn).
ReachMap ComputeReach(Cell cell, Cell velocity);

// Reach of player k at the start of the current turn (his turnStart and Velocity()).
ReachMap PlayerReach(const GameState& s, int k);

}  // namespace bot
}  // namespace soccer
