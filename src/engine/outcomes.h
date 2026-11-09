// Explicit random outcomes of actions. Every random event of the rules (a pass
// or a ball step with possible interceptions, a tackle, a poke, a shot) is a
// choice among a finite list of outcomes with known probabilities. A search
// can enumerate them (Outcomes) and apply a chosen one (ApplyOutcome);
// Apply simply samples one of them with the generator of the state.
#pragma once

#include "types.h"

namespace soccer {

enum class OutcomeKind {
    Certain,      // the action has no randomness
    BallThrough,  // pass / ball step: nobody touched the ball
    Intercepted,  // pass / ball step: `player` caught the ball
    Deflected,    // pass / ball step: `player` deflected the ball into `cell`
    Won,          // tackle / poke: the attacker got the ball
    KnockedAway,  // tackle / poke: the ball flew into `cell`
    Kept,         // tackle / poke: nothing changed
    Goal,         // shot: goal
    Caught,       // shot: the keeper caught the ball
    Parried,      // shot: the keeper parried the ball into `cell`
};

struct Outcome {
    OutcomeKind kind = OutcomeKind::Certain;
    int player = kNoPlayer;   // Intercepted, Deflected
    Cell cell;                // Deflected, KnockedAway, Parried
    double probability = 1.0;

    static Outcome Certain() { return Outcome{}; }
};

// Same outcome (the probability is not compared).
inline bool SameOutcome(const Outcome& a, const Outcome& b) {
    return a.kind == b.kind && a.player == b.player && a.cell == b.cell;
}

}  // namespace soccer
