// 64-bit hash of a game state for the transposition table of the search.
#pragma once

#include "engine/state.h"

#include <cstdint>

namespace soccer {
namespace bot {

// Hash of everything that influences the future of the game: positions and
// inertia of the players, their per-turn flags, the ball, the turn, time and
// score. The random generator, turnId and Take Back bookkeeping are ignored.
uint64_t PositionHash(const GameState& s);

// Exact equality for deterministic search continuations. The RNG is omitted
// because explicit outcomes are enumerated rather than sampled.
bool SameSearchState(const GameState& a, const GameState& b);

}  // namespace bot
}  // namespace soccer
