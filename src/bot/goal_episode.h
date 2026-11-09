// The current attack and the immediate counterattack after a loss of possession.
#pragma once

#include "engine/events.h"
#include "engine/state.h"

namespace soccer {
namespace bot {

enum class GoalEpisodeStatus {
    OurAttack,
    TheirCounterattack,
    OurGoal,
    TheirGoal,
    NoGoal,
};

struct GoalEpisode {
    int team = kWhite;
    GoalEpisodeStatus status = GoalEpisodeStatus::OurAttack;
};

struct GoalValueBounds {
    double lower = -1;
    double upper = 1;
};

// Updates the episode after one exact engine transition. Events distinguish a
// keeper's restart from an ordinary free ball at the centre of the field.
// Diagnostics for an exact reference of an episode (ssbot --shot-only,
// 01.11.2026): the value becomes the episode's best goal probability - a shot
// without a goal, a lost ball and an out end it at 0 (no counterattack, no
// rebound). Process-wide; off by default; set before any search.
void SetEpisodeShotOnly(bool on);

GoalEpisode AdvanceGoalEpisode(GoalEpisode episode, const GameState& before,
                               const GameState& after, const EventLog& events);

// Honest initial bounds for P(our goal) - P(their goal in the counterattack).
GoalValueBounds InitialGoalBounds(GoalEpisodeStatus status);

}  // namespace bot
}  // namespace soccer
