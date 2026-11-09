#pragma once

#include "bot/goal_episode.h"

namespace soccer {
namespace bot {

// A cheap, position-based estimate on the common goal-difference scale.
// The search still evaluates every stochastic outcome of each retained action.
// Possession value W for the estimates of this thread (07.11.2026; 0 = off):
// a held ball is worth +-W (2k - 1) on top of the chance (k: kept until the
// holder acts), a shot p (1 + W) - W, a free ball +-W for its first taker.
void SetGeometricPossessionValue(double w);

double GeometricStaticEstimate(const GameState& state, GoalEpisode episode,
                               bool legalTackleZones = false,
                               double threatenedRetention = 1.0,
                               bool forcedPickupPreview = false,
                               bool exclusiveAttackChannels = false,
                               bool turnOrder = false,
                               bool coverage = false,
                               bool realizedOnly = false,
                               bool timedRoute = false,
                               bool noShotFloor = false,
                               bool timedFreeBall = false,
                               bool terminalFreeBall = false);

}  // namespace bot
}  // namespace soccer
