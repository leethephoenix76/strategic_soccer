// All distinct states reachable by zero or more legal player moves this turn.
#pragma once

#include "bot/goal_episode.h"
#include "engine/actions.h"

#include <cstddef>
#include <functional>
#include <vector>

namespace soccer {
namespace bot {

struct TeamMovementState {
    GameState state;
    GoalEpisode episode;
    Action moves[kPlayersPerTeam]{};  // one legal sequence reaching this state
    int count = 0;
};

struct TeamMovementList {
    std::vector<TeamMovementState> states;  // includes the unchanged starting state
    std::size_t transitions = 0;
    std::size_t duplicates = 0;
    bool complete = true;
};

// Recomputes legal destinations after each move. Identical full search states
// with the same episode are merged; one sequence is enough to continue from
// such a state. maxStates == 0 means no state-count limit.
TeamMovementList EnumerateTeamMovements(
    const GameState& start, GoalEpisode episode, std::size_t maxStates = 0,
    const std::function<bool()>& beforeMove = {}, unsigned frozen = 0);   // frozen: bit k - player k stays

}  // namespace bot
}  // namespace soccer
