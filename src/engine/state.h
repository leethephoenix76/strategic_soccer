// The complete state of a match. It is a plain value type: no pointers, no
// global variables, cheap to copy. Everything the rules need is in here.
#pragma once

#include "rng.h"
#include "types.h"

#include <cstdint>

namespace soccer {

struct PlayerState {
    Cell pos;             // current position (changes when the player moves in this turn)
    Cell turnStart;       // position at the start of the current turn of his team
    Cell prevTurnStart;   // position at the start of the previous turn of his team
    bool moved = false;   // has moved in this turn
    bool attacked = false;// has attacked in this turn
    Cell attackDir;       // direction of that attack (towards the ball)

    // Velocity carried over from the previous turn (inertia).
    Cell Velocity() const { return {turnStart.x - prevTurnStart.x, turnStart.y - prevTurnStart.y}; }
    // Displacement made in the current turn.
    Cell Displacement() const { return {pos.x - turnStart.x, pos.y - turnStart.y}; }
};

struct BallState {
    Cell pos;
    Cell vel;                  // velocity of a free rolling ball, cells per step
    Cell dec;                  // deceleration per step: -sign(initial velocity)
    int holder = kNoPlayer;    // player who has the ball, or kNoPlayer if it is free
    int stepPhase = 0;         // a free ball makes a step at End Turn when this is 0; toggles every End Turn
    int lastTouchTeam = kWhite;// team that touched the ball last
    bool lastTouchWasPass = false; // ...and whether that touch was a deliberate pass
    int attackTeam = kWhite;   // episode owner; a deflection alone does not transfer control

    bool IsFree() const { return holder == kNoPlayer; }
};

enum class Phase {
    Lineup,     // the teams place their players, one team after another
    Play,       // normal play
    Finished,   // the match is over
};

struct GameState {
    PlayerState players[kNumPlayers];
    BallState ball;

    Phase phase = Phase::Lineup;
    int lineupStep = 0;        // 0 - the first team lines up, 1 - the second one
    int teamToMove = kWhite;
    int score[kNumTeams] = {0, 0};
    int halfTurns = 0;         // End Turns made in play; match time = halfTurns / 2 minutes
    int turnId = 0;            // increments whenever a new turn (or line-up) starts

    // Per-turn bookkeeping.
    bool ballPlayed = false;   // the team has passed or shot in this turn
    bool committed = false;    // a random event happened in this turn: it can not be taken back

    // Pending restart after the keeper has taken the ball (resolved in the same action).
    int keeperRestartTeam = -1;
    bool keeperBackPass = false;

    // Deadlock detection: bit i = the ball was sealed by the team i turns ago (3 last turns).
    int sealHistory[kNumTeams] = {0, 0};

    Rng rng;

    double MatchMinutes() const { return halfTurns * 0.5; }
};

// A new match: kick-off positions, White lines up first.
GameState NewMatch(uint64_t seed);

// Index of the player standing in the cell, or kNoPlayer.
int PlayerAt(const GameState& s, Cell c);

}  // namespace soccer
