#pragma once

#include "engine/state.h"

namespace soccer {
namespace bot {

// Proves that the opponent can collect a free ball before its next flight step,
// regardless of the current team's remaining movements.
bool ProvenOpponentPickupBeforeBallStep(const GameState& afterPass);

// Proves that an opponent can move to the next ball landing cell and collect
// it at EndTurn, regardless of our remaining moves. The caller must have a
// certain BallThrough pass outcome.
bool ProvenOpponentPickupAtNextBallStep(const GameState& afterPass);

// With the opponent choosing only EndTurn, proves that no own player can reach
// or touch the free ball before it goes out for the opponent or reaches a
// stationary opponent. The caller must have a certain BallThrough pass outcome.
bool ProvenNoOwnRecoveryOnPassiveReply(const GameState& afterPass);

}  // namespace bot
}  // namespace soccer
