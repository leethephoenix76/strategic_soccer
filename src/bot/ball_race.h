// Who reaches a free ball first (docs/bot_design.md, "Гонка за свободным мячом").
//
// The question is answered for the branch in which the ball is not touched on
// its way (interceptions and deflections are random outcomes of the ball's
// steps; the search averages over them). In that branch the race is a game of
// movement only, and the solver gives for every player two bounds on the
// moment he takes the ball:
//
//   lower - by inertia alone, every other player ignored. Exact for a lone
//           player; nobody can take the ball earlier than his lower bound.
//   upper - guaranteed: a route on which no move can be blocked, whatever the
//           opponents do (each opponent may stand on any cell he could reach
//           by then) and with his team-mates standing where they are.
//
// If a team's best guaranteed moment comes no later than the other team's best
// lower bound, the ball is certainly that team's (Taken). If nobody can reach
// the ball before its path ends out of play or at a keeper, the restart is
// certain (Restart). Otherwise the race is Contested and the solver names the
// players who may still take the ball first: the search has to play it out.
//
// Time is measured in slots: slot 0 is the rest of the current turn of the
// team to move, slot 1 the next turn of the other team, and so on. Event
// moments are half-slots: a move in slot i happens at 2i, the End Turn after
// slot i (and the ball's step at it) at 2i + 1.
#pragma once

#include "engine/state.h"

#include <string>
#include <vector>

namespace soccer {
namespace bot {

constexpr int kRaceNever = 1 << 20;

enum class BallRaceKind {
    NotFree,     // the ball is held or the state is not in play
    Taken,       // a team certainly gets the ball first
    Restart,     // nobody can reach the ball before it leaves play or reaches a keeper
    Contested,   // the bounds do not decide: the search has to play the race
};

// Where the ball can be taken: a cell of its path and the moments it is there.
struct BallWindow {
    Cell cell;
    int arrival = -1;        // moment of the step that brings the ball here (2i+1); -1: it is here now
    int lastMoveSlot = -1;   // last slot whose moves still find it here; -1: only a player standing
                             // here at the arrival takes it (the step leaves play or meets a keeper)
    bool openEnd = false;    // the ball rests here for good (lastMoveSlot is unbounded)
    bool certainStand = true;// false: standing here takes the ball only if other goal mouth cells
                             // crossed by the step are occupied too (not used for guarantees)
};

struct BallRacePlayer {
    int lower = kRaceNever;  // earliest moment he could take the ball, ignoring everybody else
    int upper = kRaceNever;  // moment he takes it on a route nobody can block
    Cell lowerCell, upperCell;
    bool upperComputed = false;
};

struct BallRaceResult {
    BallRaceKind kind = BallRaceKind::NotFree;
    int team = -1;            // Taken: the team that gets the ball; Restart: the team that restarts
    int player = kNoPlayer;   // Taken: the player with the earliest guaranteed take
    Cell cell;                // Taken: where he takes it
    int moment = -1;          // Taken: when (the guarantee); Restart: the moment of the out or keeper
    bool keeper = false;      // Restart: the keeper takes the ball (otherwise the ball is out)
    unsigned contestants = 0; // Contested: bit k - player k may still be the first to take the ball

    std::vector<BallWindow> windows;
    int endMoment = kRaceNever;  // the path ends (out or keeper) at this step unless somebody takes the ball
    bool endKeeper = false;
    int endTeam = -1;            // the team that restarts
    bool endCertain = false;     // the end does not depend on players standing in the goal mouth
    BallRacePlayer players[kNumPlayers];

    std::string Describe(const GameState& s) const;
};

struct BallRaceOptions {
    int horizonTurns = 16;      // own turns a guaranteed route may take
    bool allUppers = false;     // compute every player's guaranteed bound (diagnostics); otherwise
                                // only as many as the decision needs
};

BallRaceResult SolveBallRace(const GameState& s, const BallRaceOptions& options = BallRaceOptions());
// The windows of SolveBallRace alone: the untouched path of a free ball (07.11.2026).
std::vector<BallWindow> BallPathWindows(const GameState& s);

// The state after a Taken race: the winner holds the ball on his cell. Exact
// when the take happens in the current turn or at its End Turn (the winner's
// single move and the ball's step are played by the engine). For a later take
// the other players' cells are unknown: the winner is put on his cell with the
// ball and the other team is to move, as it will move before he gets there;
// everybody else keeps cell and inertia.
GameState BallRaceTakenState(const GameState& s, const BallRaceResult& r);

// A lower bound on the goal probability of the ball holder's shot, valid
// whatever the opponents do (bot_design.md, "Выход 1 в 0"): the holder runs on
// a route nobody can block (as the guaranteed take above), never ends a turn
// where an opponent could be next to him for a tackle, and shoots from a cell
// of the route; every player that could stand in the shot's blocking zone by
// then counts with his worst cell, team-mates stay where they are. 0 if there
// is no such route or shot.
// `floor`: only a bound above it is wanted (0 is returned otherwise, cheaply).
// A state built ahead of time (BallRaceTakenState for a later take) is
// described by `ahead`: every opponent has `opponentHeadStart` turns more than
// the state shows, and with `holderMoved` the holder has already used his move
// in his first turn (he took the ball with it): then he may only shoot from
// his cell or stand.
struct BreakawayAhead {
    int opponentHeadStart = 0;
    bool holderMoved = false;
    unsigned exact = 0;   // bit k: opponent k stands where the state shows (no head start)
    // The state the head start is counted from: the inertia of the opponents
    // with a head start is theirs there (their cells are the same).
    const GameState* origin = nullptr;
};
double BreakawayShotLowerBound(const GameState& s, int horizonTurns = 12, double floor = 0,
                               BreakawayAhead ahead = BreakawayAhead());
// How BallRaceTakenState is ahead of time for a Taken race.
BreakawayAhead BallRaceTakenAhead(const GameState& s, const BallRaceResult& r);

// A lower bound on the goal probability of a 2 v 1 (bot_design.md, "2 в 1"):
// the holder, his partner and the one defender who can join; everybody else
// counts as in the bound above (his worst reachable cell). When the defence
// is to move, the attackers choose after seeing where the defender went: the
// minimum over his cells of the best of the holder's breakaway bound (his
// shot now included) and a pass into the partner's feet (its worst-case
// chance of passing untouched times the partner's breakaway bound). A cell
// from which the defender can tackle the holder counts 0 (a lost ball adds
// nothing to the goal probability). When the attackers are to move: the
// holder's bound or a pass now. 0 if nothing is proven.
double TwoVsOneLowerBound(const GameState& s, int partner, int defender, int horizonTurns = 12,
                          double floor = 0);

// Moment as text: "s3" (a move in slot 3) or "s3+" (the End Turn after slot 3).
std::string RaceMomentName(int moment);

// Exposed for tests. Earliest number of own turns for a player standing at
// `from` whose previous turn's displacement had signs `sign` to stand on
// `target`, ignoring other players (0 if he is there).
int RaceTurns(Cell from, Cell sign, Cell target);
// Slot of a player's first remaining turn: 0 if his team is to move and he has
// not moved yet, 2 if it has moved, 1 for the other team.
int RaceFirstSlot(const GameState& s, int k);

}  // namespace bot
}  // namespace soccer
