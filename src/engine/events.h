// Events produced by the rules while applying an action. The engine never shows
// anything itself: the user interface (or a log, or a bot) interprets the events.
#pragma once

#include <vector>

namespace soccer {

enum class EventType {
    LineupStarted,      // team: lines up now
    TurnStarted,        // team: moves now
    BallIntercepted,    // player: caught a pass / rolling ball
    BallDeflected,      // player: deflected a pass / rolling ball
    BallOut,            // team: touched the ball last; the opponent restarts
    TackleWon,          // player: the attacker took the ball
    TackleKnockedAway,  // player: the attacker knocked the ball away
    TackleFailed,       // player: the holder kept the ball
    PokeWon,            // player: poked a sealed free ball out
    PokeKnockedAway,    // player: knocked a sealed free ball away
    PokeFailed,         // player: the ball stays where it is
    ShotTaken,          // player: shoots
    Goal,               // team: scored
    KeeperCaught,       // team: the keeper's team caught the shot
    KeeperParried,      // team: the keeper's team parried the shot, play goes on
    KeeperTookBall,     // team: the ball came into the goal mouth, the keeper's team restarts
    BackPassPenalty,    // team: passed to the own keeper, the opponent restarts
    SealWarning,        // team: is sealing the ball
    SealPenalty,        // team: sealed the ball too long, the opponent restarts
    HalfTime,
    MatchOver,
};

struct Event {
    EventType type;
    int team = -1;
    int player = -1;
};

using EventLog = std::vector<Event>;

}  // namespace soccer
