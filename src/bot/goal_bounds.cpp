#include "bot/goal_bounds.h"

#include "engine/rules.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace soccer {
namespace bot {

double ImmediateShotUpper(const GameState& s, int team) {
    if (s.phase != Phase::Play || s.teamToMove != team || s.ballPlayed) return 0;
    if (s.ball.IsFree() || TeamOf(s.ball.holder) != team) return 1;

    const PlayerState& holder = s.players[s.ball.holder];
    double upper = OpenShotProbability(holder.pos, team);  // shooting without moving
    if (holder.moved) return upper;

    // Movement without blockers is a superset of every legal destination, even
    // when team-mates move first and open a corridor for the holder.
    const Cell velocity = holder.Velocity();
    const Cell centre{holder.turnStart.x + Sign(velocity.x), holder.turnStart.y + Sign(velocity.y)};
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const Cell cell{centre.x + dx, centre.y + dy};
            if (InField(cell)) upper = std::max(upper, OpenShotProbability(cell, team));
        }
    return upper;
}

GoalValueBounds GoalStateBounds(const GameState& s, GoalEpisode episode) {
    const GoalValueBounds general = InitialGoalBounds(episode.status);
    if (episode.status == GoalEpisodeStatus::OurGoal ||
        episode.status == GoalEpisodeStatus::TheirGoal ||
        episode.status == GoalEpisodeStatus::NoGoal ||
        s.phase != Phase::Play ||
        (s.halfTurns != kHalfTurnsPerHalf - 1 && s.halfTurns != kHalfTurnsPerMatch - 1))
        return general;
    const double p = ImmediateShotUpper(s, s.teamToMove);
    const double shotUpper = p == 0 ? 0 : std::min(1.0, std::nextafter(p, std::numeric_limits<double>::infinity()));
    if (s.teamToMove == episode.team)
        return episode.status == GoalEpisodeStatus::OurAttack ? GoalValueBounds{0, shotUpper}
                                                            : GoalValueBounds{0, 0};
    return {-shotUpper, 0};
}

}  // namespace bot
}  // namespace soccer
