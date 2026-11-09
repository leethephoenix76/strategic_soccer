// Public interface of the game rules. Everything a user interface or a bot needs:
// validating and applying actions, enumerating legal actions and querying the
// position (destinations, attack options, ball path, shot probability...).
#pragma once

#include "actions.h"
#include "events.h"
#include "outcomes.h"
#include "state.h"

#include <vector>

namespace soccer {

// ---------------------------------------------------------------- actions

// Checks an action without changing the state.
ActionError Validate(const GameState& s, const Action& a);

// Validates and applies an action. A random outcome is sampled from Outcomes(s, a)
// with one draw of s.rng. Events describing
// what happened are appended to `events` (may be null). Returns the validation
// error; the state is unchanged if it is not ActionError::None.
// If `outcome` is not null, the applied outcome is stored there.
ActionError Apply(GameState& s, const Action& a, EventLog* events = nullptr, Outcome* outcome = nullptr);

// Possible random outcomes of an action with their probabilities (summing to 1),
// in a fixed order. An action without randomness has one Certain outcome; End
// Turn is random when the free ball makes a step. Empty if the action is not valid.
std::vector<Outcome> Outcomes(const GameState& s, const Action& a);

// Applies an action with the given outcome (one of Outcomes(s, a), compared by
// SameOutcome) instead of a random one; s.rng is not used. Returns BadOutcome
// (state unchanged) if it is not a possible outcome.
ActionError ApplyOutcome(GameState& s, const Action& a, const Outcome& o, EventLog* events = nullptr);

// Fast versions for search: no validation. The action must be legal (from
// LegalActions of this state) and the outcome one of its outcomes.
std::vector<Outcome> OutcomesUnchecked(const GameState& s, const Action& a);
void ApplyUnchecked(GameState& s, const Action& a, const Outcome& o, EventLog* events = nullptr);

// All legal actions of the team to move (EndTurn is always included while the
// match is not finished). Order: placements/moves, attacks, passes, shot, end turn.
std::vector<Action> LegalActions(const GameState& s);

// ---------------------------------------------------------------- queries

// Cells the player can move to in this turn (play phase), including his own cell.
// Takes inertia, blocking by other players and the direction of an attack made
// in this turn into account.
std::vector<Cell> MoveDestinations(const GameState& s, int player);

// Cells where the player can be placed during the line-up.
bool CanPlace(const GameState& s, int player, Cell target, ActionError* why = nullptr);

enum class AttackKind {
    None,
    Tackle,   // the opponent ball holder is in an adjacent cell
    Poke,     // a free ball diagonally adjacent, the corridor closed by two opponents
};
AttackKind GetAttackKind(const GameState& s, int player);

// The own ball holder can pass to this cell.
bool CanPassTo(const GameState& s, Cell target);

// Probability that a legal pass reaches its target without any player touching
// its first segment. Returns zero for an illegal pass.
double PassThroughProbability(const GameState& s, Cell target);

// The pass to `target` would roll (ignoring interceptions) into the goal mouth of
// the passer's own goal - a back pass that gives the ball to the opponent.
bool PassReachesOwnKeeper(const GameState& s, Cell target);

// Cells the ball would visit (ignoring interceptions): of a free rolling ball,
// or of a pass to `target` by the current holder.
std::vector<Cell> PredictBallPath(const GameState& s);
std::vector<Cell> PredictPassPath(const GameState& s, Cell target);

// Probability that a shot by `shooter` (the holder) scores.
double ShotProbability(const GameState& s, int shooter);

// ---------------------------------------------------------------- formulas
// The formulas of the rules for given cells, for models that place players
// hypothetically (the bot's planner). The actions above use the same functions.

// Tests a movement segment against occupied cells in the mover's local 3x3
// destination square, including two blockers that close a diagonal.
bool MovePathBlocked(Cell start, Cell target, const Cell* blockers, int blockerCount);

// An attack from `attacker` on the ball in `ball` is not against the velocity
// from the previous turn nor against the displacement in this turn.
bool AttackDirectionAllowed(Cell attacker, Cell ball, Cell velocity, Cell displacement);
// Probability that a tackle from `attacker` wins the ball from the holder in `holder`.
double TackleChance(Cell holder, Cell attacker, int attackerTeam);
// Probability that a fight (tackle or poke) knocks the ball away.
constexpr double kKnockAwayChance = 0.2;
// p(d) of a player at distance d from the line of the ball: interception p^2,
// deflection p - p^2.
double InterceptionWeight(double distance);
// Probability of a goal from a shot from `from` by `team` without blockers, and
// the factor of each blocking player at distance d from the shot line.
double OpenShotProbability(Cell from, int team);
double BlockFactor(double distance);
// The factor one player standing on `other` puts on a shot of `team` from `shooter`
// (1 outside the blocking zone); ShotProbability is OpenShotProbability times these.
double ShotBlockFactor(Cell shooter, int team, Cell other);
// Cells where a parried shot of `shootingTeam` lands, each equally likely:
// x = 3..7 in the 3 rows in front of the goal, never into the goal mouth.
constexpr int kParryCells = 15;
void ParryCells(int shootingTeam, Cell out[kParryCells]);

// The ball is sealed by `team`: no opponent player can reach the ball cell or a
// cell next to it (deadlock).
bool BallSealedBy(const GameState& s, int team);

}  // namespace soccer
