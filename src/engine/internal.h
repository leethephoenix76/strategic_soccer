// Helpers shared by the rule implementation files. Not part of the public API.
#pragma once

#include "rules.h"

#include <vector>

namespace soccer {
namespace detail {

// ---- state.cpp
// Kick-off positions, ball in the centre, line-up phase; `firstTeam` lines up first.
void ResetField(GameState& s, int firstTeam);
// Restart from the centre: reset the field and announce the line-up.
void Restart(GameState& s, int firstTeam, EventLog* events);
// Restart that ends a turn in the middle (goal, caught shot, the keeper taking the
// ball after a pass or an attack): the turn counts as a played half-turn, so the
// clock runs on; at 45 minutes it is the half-time, at 90 the end of the match.
void RestartInterrupted(GameState& s, int firstTeam, EventLog* events);
void Emit(EventLog* events, EventType type, int team = -1, int player = -1);

// ---- ball.cpp
void Touch(GameState& s, int team, bool pass);
// Gives the ball to the player standing in the ball cell (and stops it), if any.
void UpdatePossession(GameState& s);
// Outcomes of a ball segment start -> start + vel: interceptions and deflections
// by players on the way (except `excludePlayer`, the passer), or the ball goes through.
void SegmentOutcomes(const GameState& s, Cell start, Cell vel, int excludePlayer, std::vector<Outcome>& out);
// Runs one segment. Returns true if the untouched ball goes out of play; a free
// goal mouth on the path is taken by the keeper instead.
bool ApplySegment(GameState& s, const Outcome& o, EventLog* events);
// Decelerates a free ball after a segment.
void Decelerate(BallState& b);
// A free ball lying in goal mouth cell c: the keeper takes it.
void KeeperCheck(GameState& s, Cell c);
// Performs the pending keeper restart, if any. Returns true if it happened.
// `midTurn`: the restart interrupts the turn (RestartInterrupted).
bool ResolveKeeperRestart(GameState& s, EventLog* events, bool midTurn);
// Cells of the 5x5 square around `center` inside the field, excluding `ex1`, `ex2`.
// Returns their number (at least 1).
int CellsAround(Cell center, Cell ex1, Cell ex2, Cell cells[25]);

// ---- duel.cpp
void TackleOutcomes(const GameState& s, int attacker, std::vector<Outcome>& out);
void PokeOutcomes(const GameState& s, int attacker, std::vector<Outcome>& out);
void ResolveTackle(GameState& s, int attacker, const Outcome& o, EventLog* events);
void ResolvePoke(GameState& s, int attacker, const Outcome& o, EventLog* events);

// ---- shot.cpp
void ShotOutcomes(const GameState& s, int shooter, std::vector<Outcome>& out);
void ResolveShot(GameState& s, int shooter, const Outcome& o, EventLog* events);

}  // namespace detail
}  // namespace soccer
