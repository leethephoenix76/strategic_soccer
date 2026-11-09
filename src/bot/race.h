// The race for a free ball (docs/bot_design.md, 7.15): who of the two players
// that matter gets the ball first.
//
// The ball's path without touches is known (it rolls by the rules). For every
// player the reach tables give the earliest half-turn at which he could stand on
// the ball's cell when it is there - a lower bound, the others ignored. The
// runner of each team is its player with the earliest such bound; the other
// players stand where they are (they block cells and may still touch the ball by
// the rules). Between the two runners the race is a game solved exactly with the
// engine's rules (movement with inertia and blocking, the order of turns, the
// ball's steps and their random interceptions): the team to move tries to get
// the ball, the other team too, each player moving one cell square per turn.
//
// Solved by alpha-beta (Star1 at the ball's steps, a table of bounds, moves to
// the ball first; a runner who can not get near the ball only stands) - the same
// value as the plain minimax (RaceOptions::reference), ~1 ms at 8 half-turns.
//
// The value is the probability that a team gets the ball (or keeps it from the
// other: a ball nobody reaches within the horizon counts 1/2 each). The line of
// best play gives the first move of each runner and where the ball is taken.
#pragma once

#include "engine/state.h"

#include <string>
#include <vector>

namespace soccer {
namespace bot {

struct RaceOptions {
    int horizon = 12;   // half-turns
    bool reference = false;   // the plain minimax (for tests); otherwise alpha-beta, the same value
    double epsilon = 0;       // alpha-beta only: branches less likely than this are guessed (error at most this per branch)
    long maxStates = 0;       // alpha-beta only: beyond this many states the rest is guessed; 0: no limit
    bool line = true;         // false: only the value and the bounds, no line of best play (much cheaper)
    int clearMargin = 0;      // > 0: when one runner's bound is this many half-turns earlier (or the other
                              // has none), that team gets the ball without solving (no line either)
};

struct RaceResult {
    bool valid = false;           // a free ball in play
    int runner[kNumTeams] = {kNoPlayer, kNoPlayer};
    int bound[kNumTeams] = {-1, -1};   // earliest half-turn (lower bound) of the runner, -1: never
    double pWhite = 0.5;          // probability that White gets the ball
    int winner = -1;              // on the line of best play: the team that takes the ball, -1: nobody
    Cell pickup;                  // where
    int pickupHalfTurn = -1;      // after how many half-turns
    Cell firstMove[kNumTeams];    // the runners' moves on the line (their own cell: stay)
    GameState taken;              // winner >= 0: the state on the line right after the ball is taken
    std::vector<Cell> path;       // the ball's cells at the start of each half-turn, untouched
    long states = 0;
    std::string Describe() const;
};

// The state must be in play with a free ball.
RaceResult Race(const GameState& s, const RaceOptions& options = RaceOptions());

}  // namespace bot
}  // namespace soccer
