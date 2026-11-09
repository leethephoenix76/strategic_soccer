#include "bot/goal_episode.h"

namespace soccer {
namespace bot {

namespace {

bool g_episodeShotOnly = false;

bool Terminal(GoalEpisodeStatus status) {
    return status == GoalEpisodeStatus::OurGoal || status == GoalEpisodeStatus::TheirGoal ||
           status == GoalEpisodeStatus::NoGoal;
}

bool HasEvent(const EventLog& events, EventType type) {
    for (const Event& event : events)
        if (event.type == type) return true;
    return false;
}

}  // namespace

void SetEpisodeShotOnly(bool on) { g_episodeShotOnly = on; }

GoalEpisode AdvanceGoalEpisode(GoalEpisode episode, const GameState& before,
                               const GameState& after, const EventLog& events) {
    if (Terminal(episode.status)) return episode;

    for (const Event& event : events)
        if (event.type == EventType::Goal) {
            episode.status = event.team == episode.team ? GoalEpisodeStatus::OurGoal
                                                        : GoalEpisodeStatus::TheirGoal;
            return episode;
        }

    const bool shotOnly = g_episodeShotOnly;
    if (shotOnly && HasEvent(events, EventType::ShotTaken)) {
        episode.status = GoalEpisodeStatus::NoGoal;
        return episode;
    }
    if (after.phase == Phase::Finished || HasEvent(events, EventType::HalfTime)) {
        episode.status = GoalEpisodeStatus::NoGoal;
        return episode;
    }

    if (HasEvent(events, EventType::BallOut)) {
        // An out ends either attack, including when the same team gets the
        // restart. Only an opponent restart starts the one counterattack that
        // belongs to our original attack's value.
        if (!shotOnly && episode.status == GoalEpisodeStatus::OurAttack && after.teamToMove != episode.team)
            episode.status = GoalEpisodeStatus::TheirCounterattack;
        else
            episode.status = GoalEpisodeStatus::NoGoal;
        return episode;
    }

    const int attackingTeam = episode.status == GoalEpisodeStatus::OurAttack
                                  ? episode.team : Opponent(episode.team);
    const bool opponentHolds = !after.ball.IsFree() && TeamOf(after.ball.holder) != attackingTeam;
    const bool opponentRestarts = before.phase == Phase::Play && after.phase == Phase::Lineup &&
                                  after.teamToMove != attackingTeam;
    // The episode's goal probability (shot-only): any restart ends it - its
    // line-up is not the episode (08.11.2026: the exact count spent nearly all
    // its expansions on line-up orders of own restarts).
    const bool restarts = before.phase == Phase::Play && after.phase == Phase::Lineup;
    if (shotOnly && (opponentHolds || restarts || HasEvent(events, EventType::BallOut))) {
        episode.status = GoalEpisodeStatus::NoGoal;
        return episode;
    }
    if (opponentHolds || opponentRestarts)
        episode.status = episode.status == GoalEpisodeStatus::OurAttack
                             ? GoalEpisodeStatus::TheirCounterattack : GoalEpisodeStatus::NoGoal;
    return episode;
}

GoalValueBounds InitialGoalBounds(GoalEpisodeStatus status) {
    switch (status) {
        case GoalEpisodeStatus::OurAttack: return {-1, 1};
        case GoalEpisodeStatus::TheirCounterattack: return {-1, 0};
        case GoalEpisodeStatus::OurGoal: return {1, 1};
        case GoalEpisodeStatus::TheirGoal: return {-1, -1};
        case GoalEpisodeStatus::NoGoal: return {0, 0};
    }
    return {-1, 1};
}

}  // namespace bot
}  // namespace soccer
