// The value of an attack episode by a minimax over its participants only
// (bot_design.md, "Таблица эпизода", 02.11.2026): the attack's best
// achievable goal probability against the best defence, the other players
// standing where they are (they still block shots and touch passes). The
// attack's turn: its participants move (the holder dribbles), then the holder
// shoots, passes into a partner's feet or keeps the ball; the defence's turn:
// its participants move and tackle the holder. A shot is worth its goal
// probability (what follows it is ignored), a lost or free ball and the end
// of the horizon are worth 0. Computed lazily with a transposition table.
#pragma once

#include "engine/actions.h"
#include "engine/state.h"

namespace soccer {
namespace bot {

struct EpisodeValueOptions {
    int turns = 6;              // team turns from the state (its rest of the turn counts as one)
    long nodeLimit = 20000000;  // movement states enumerated; the search stops (incomplete) beyond it
    double floor = 0;           // only a value above it is wanted: a result <= floor means "not above"
    // Opponents outside the episode with a moving front (ball_plan.h, 02.11.2026;
    // 0: none). The state must have the attack to move. They stand where they
    // are, but the ball may never be where one of them can be by then (reach
    // with inertia, others ignored): a pass whose bounding box he can stand in
    // is worth 0, the holder ending the attack's turn where he can stand next
    // to him in his reply is worth 0, and a shot whose blocking zone he can get
    // into counts him on his worst cell there.
    unsigned frontOpponents = 0;
    // Passes ahead (02.11.2026): the holder may also pass into an empty cell that
    // a partner of the episode can reach within two of his turns; the free ball
    // then rolls by the rules (its steps touched by anybody near), and the
    // episode goes on while it is free or held by the attack: a participant
    // stepping onto it picks it up. Off: a free ball ends the episode at 0.
    bool leadPasses = false;
    // With passes ahead: a touch of the free ball rarer than this (one deflection
    // cell) is not followed, worth 0 (a lower bound).
    double rareOutcome = 0.02;
};

struct EpisodeValueResult {
    double value = 0;           // the attack's goal probability
    long nodes = 0;
    long movementStates = 0;
    long leadTried = 0, leadCandidates = 0, freeNodes = 0, leadRaceLost = 0;   // passes ahead: tried, considered; nodes with a free ball
    bool complete = true;
    // The best decision of the attack's turn at the root (the state must have the
    // attack to move): the state after its participants' movements and the ball
    // action there (Shot, Pass or EndTurn: keep the ball); rootValue its value.
    bool rootFound = false;
    GameState rootState;
    Action rootAction = Action::EndTurn();
    double rootValue = 0;
};

// `attackers` / `defenders`: bit masks of the participants; the ball holder
// must be one of the attackers. The state is at the start of a team turn or
// inside it (the team to move continues its turn).
EpisodeValueResult EpisodeValue(const GameState& s, unsigned attackers, unsigned defenders,
                                const EpisodeValueOptions& options = EpisodeValueOptions());

}  // namespace bot
}  // namespace soccer
