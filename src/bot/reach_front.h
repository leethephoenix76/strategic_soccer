// Alternating reach waves with inertia and contact between opposing areas.
#pragma once

#include "engine/actions.h"
#include "engine/state.h"
#include "bot/ball_race.h"

#include <array>
#include <cstdint>
#include <vector>

namespace soccer::bot {

constexpr int kReachFrontCells = kFieldWidth * (kLastRow - kFirstRow + 1);

constexpr int ReachFrontIndex(Cell cell) {
    return (cell.y - kFirstRow) * kFieldWidth + cell.x;
}

struct ReachFrontFrame {
    // One bit per player. Teammates may overlap. Opponents cannot enter a
    // previously claimed cell or move through it.
    std::array<uint8_t, kReachFrontCells> players{};
};

enum class ReachFrontStop { Full, Stable, Horizon, InvalidStart };

struct ReachFront {
    std::vector<ReachFrontFrame> frames;  // frame 0 is the supplied position
    std::array<int, kReachFrontCells> firstArrival;  // half-turn, or -1
    ReachFrontStop stop = ReachFrontStop::InvalidStart;
};

// A geometric model from a play position. Each player's wave retains all
// possible (cell, signs of velocity) states, with cumulative cell claims.
// Inside a turn the players of the team to move who have moved stand during
// half-turn 1 (07.11.2026). Attacks and ball outcomes are outside this model.
// `ignore`: players left out of the map (they neither claim nor block), e.g.
// the ball carrier when the defence's access to his route is measured.
ReachFront ComputeReachFront(const GameState& s, int maxHalfTurns = kHalfTurnsPerMatch, unsigned ignore = 0);

// First team whose cumulative wave can meet an untouched pass at a ball-race
// window. Returns -1 if the map shows no meeting. This is a possibility, not
// a guaranteed reception; the windows must come from the BallThrough state.
int ReachPassFirstTeam(const GameState& s, const ReachFront& front,
                       const std::vector<BallWindow>& windows);

// Edges shared by cells of opposite teams, given as pairs of cell indices.
// Pass pruning (07.11.2026): the first team whose wave meets the untouched
// ball of `pass` from `s` (a pass to an empty cell; -1 otherwise or no
// meeting). The map must be computed from `s` itself.
int ReachPassTaker(const GameState& s, const ReachFront& front, const Action& pass);

// Potential 1 v 0 (07.11.2026): the cells the ball holder's own wave reaches
// before any opponent's - a route nobody can stand on first - and the best
// open shot among them. A detector, not a value: tackles and blocked shots
// are outside the map; BreakawayShotLowerBound checks a detected breakaway.
struct ReachBreakaway {
    double shot = 0;      // best OpenShotProbability over the holder's cells
    int halfTurn = -1;    // the frame where the holder first reaches a cell with that shot
    Cell cell{};
};
ReachBreakaway ReachHolderBreakaway(const GameState& s, const ReachFront& front);

std::vector<std::array<int, 2>> ReachFrontBorders(const ReachFrontFrame& frame);

}  // namespace soccer::bot
