// Episodes of the search (docs/bot_design.md): which players take part in the
// play of the next few half-turns, so the search moves only them.
#pragma once

#include "engine/actions.h"
#include "engine/state.h"

#include <string>
#include <vector>

namespace soccer {
namespace bot {

// ---- The attack episode by the race to the goal (7.67) ----------------------
// The attacking team's carrier (the holder, or the runner who is a whole round
// first at a free ball) heads for the goal. Times are plies from now, when a
// player can stand on a cell by reach with inertia (reach.h: a lower bound, the
// others ignored). An attacker's way: the cells not behind him, between the
// goal posts' columns widened to his column, and one more column each side. The
// attack's progress by ply t: the nearest row to the goal that an attacker of
// the episode can stand on, on his way, by then. The episode ends when the
// carrier can stand on the best shooting cell of his way (the highest open shot
// probability); comparisons stop there (a defender one ply later still counts).
// - A partner takes part if he keeps pace: he can stand on a cell of the
//   carrier's way level with or ahead of the carrier's progress when he gets
//   there. Others are behind the play: a pass back ends the episode.
// - A defender takes part if he gets goal-side in time: he can stand on a cell
//   of an attacker's way at least as near the goal as the attack's progress one
//   ply earlier (a tie goes to the defence: a ball pushed forward waits there
//   during the defenders' turn). Others can not catch up: the carrier pushes
//   the ball ahead, a free ball can not be tackled, and at the same speed a
//   chaser stays behind.
// A contested free ball has no such episode (everybody takes part).
enum class AttackEpisodeKind { None, OneVsZero, TwoVsOne, General };

struct AttackEpisode {
    AttackEpisodeKind kind = AttackEpisodeKind::None;
    int team = -1;                 // the attacking team, -1: none
    int carrier = kNoPlayer;
    int carrierPly = 0;            // the ply at whose end the carrier has the ball (0: he has it now)
    int endPly = 0;                // the carrier can stand on the best shooting cell of his way by then
    unsigned attackers = 0;        // bit k: attacker k takes part (the carrier included)
    unsigned defenders = 0;        // bit k: defender k takes part
    unsigned outside = 0;          // bit k: player k is outside the episode
    std::string reason[kNumPlayers];
};

// `ballRace` (bot_design.md 4.4, 02.11.2026): a free ball's carrier is the
// player the ball-race solver (ball_race.h) proves takes it first, from that
// moment and cell; a race the solver does not decide has no episode. Without
// it the coarse race above (a whole round first by reach) is used.
AttackEpisode AnalyzeAttackEpisode(const GameState& s, bool explain = false, bool ballRace = false);

const char* AttackEpisodeName(AttackEpisodeKind kind);

// The moves of the team to move's outside players who have not moved yet, in
// order, by a fixed policy (no search): a defender runs back towards the cells
// between the ball and his goal, an attacker runs after the play to within a
// pass of the carrier, not behind him; each takes the destination from which
// he needs the fewest turns (ties: nearer the attacked goal - his own for a
// defender - then nearer the goal's column / the carrier's column, then
// staying). Legal in sequence from `s`.
std::vector<Action> OutsiderMoves(const GameState& s, const AttackEpisode& e);

}  // namespace bot
}  // namespace soccer
