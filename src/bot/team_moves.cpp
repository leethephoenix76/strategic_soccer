#include "bot/team_moves.h"

#include "bot/hash.h"
#include "engine/rules.h"

#include <cstdint>
#include <unordered_map>

namespace soccer {
namespace bot {

namespace {

uint64_t MovementKey(const GameState& state, GoalEpisode episode) {
    return PositionHash(state) ^ (static_cast<uint64_t>(episode.team + 1) << 61) ^
           (static_cast<uint64_t>(episode.status) << 56);
}

}  // namespace

TeamMovementList EnumerateTeamMovements(
    const GameState& start, GoalEpisode episode, std::size_t maxStates,
    const std::function<bool()>& beforeMove, unsigned frozenMask) {
    TeamMovementList result;
    TeamMovementState rootNode;
    rootNode.state = start;
    rootNode.episode = episode;
    result.states.push_back(rootNode);
    if (start.phase != Phase::Play) return result;

    std::unordered_map<uint64_t, std::vector<std::size_t>> seen;
    seen[MovementKey(start, episode)].push_back(0);
    for (std::size_t i = 0; i < result.states.size(); ++i) {
        // Adding children can reallocate the vector, so keep a stable parent.
        const TeamMovementState parent = result.states[i];
        if (parent.count >= kPlayersPerTeam) continue;
        const int first = FirstPlayerOf(parent.state.teamToMove);
        for (int player = first; player < first + kPlayersPerTeam; ++player) {
            if (parent.state.players[player].moved) continue;
            if (frozenMask >> player & 1u) continue;
            for (Cell cell : MoveDestinations(parent.state, player)) {
                if (cell == parent.state.players[player].pos) continue;
                if (beforeMove && !beforeMove()) { result.complete = false; return result; }
                const Action action = Action::Move(player, cell);
                GameState child = parent.state;
                EventLog events;
                ApplyUnchecked(child, action, Outcome::Certain(), &events);
                const GoalEpisode next = AdvanceGoalEpisode(parent.episode, parent.state, child, events);
                ++result.transitions;
                const uint64_t key = MovementKey(child, next);
                auto& indices = seen[key];
                bool duplicate = false;
                for (std::size_t index : indices) {
                    const TeamMovementState& prior = result.states[index];
                    if (prior.episode.team == next.team && prior.episode.status == next.status &&
                        SameSearchState(prior.state, child)) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) { ++result.duplicates; continue; }
                if (maxStates && result.states.size() >= maxStates) {
                    result.complete = false;
                    return result;
                }
                TeamMovementState entry;
                entry.state = child;
                entry.episode = next;
                for (int step = 0; step < parent.count; ++step) entry.moves[step] = parent.moves[step];
                entry.moves[parent.count] = action;
                entry.count = parent.count + 1;
                indices.push_back(result.states.size());
                result.states.push_back(entry);
            }
        }
    }
    return result;
}

}  // namespace bot
}  // namespace soccer
