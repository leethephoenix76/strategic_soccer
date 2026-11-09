// Rigorous short-horizon probability bounds for plan-family search.
#pragma once

#include "bot/goal_episode.h"

namespace soccer {
namespace bot {

// Upper bound on the probability that `team` scores during its current turn.
// With the team's holder, ignore blockers and obstacles to the holder's move.
// With a free or opponent-held ball, return the conservative bound 1: a pickup
// or tackle may permit a shot later in the same turn. Returns zero when this
// team cannot play the ball during the current turn.
double ImmediateShotUpper(const GameState& s, int team);

// Valid bounds on the full attack/counterattack value from this position.
// Near the end of a half, only an immediate shot by the moving team can score.
GoalValueBounds GoalStateBounds(const GameState& s, GoalEpisode episode);

}  // namespace bot
}  // namespace soccer
