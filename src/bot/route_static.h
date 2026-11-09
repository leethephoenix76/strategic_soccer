// Route static on the reach map (07.11.2026, prototype for measurement).
//
// The value of a held ball: the best goal chance the attack can reach that
// the defence cannot stop, with each defender stopping one threat. On the
// reach map (reach_front.h) every player has an arrival frame for each cell.
// A route takes the ball by dribble of the holder, or by one pass (to the
// feet or ahead) and the receiver's dribble, to a shot. Every defender commits
// to the cell of his next move; from there (and not before the map lets him)
// he acts on a route if he can be in time on its pass line, on the cell where
// a pass waits for its receiver, or in the shot's blocking zone. The value is
// the minimum over the defence's commitments of the best route. A defender who
// can reach no route (cut off) changes nothing; `open` is the best route with
// every defender at his best for it separately.
//
// The shot: open shot probability times the blocking of the defenders who can
// be in its zone in time; a pass: the interception odds of the rules for a
// defender who can be by its line; a defence moving first can tackle the
// holder where he stands. Measured 07.11.2026: docs/route_static_20261107.md.
#pragma once

#include "engine/state.h"

namespace soccer::bot {

struct RouteStatic {
    bool valid = false;
    int team = -1;            // the team with the ball
    double value = 0;         // min over the defence's assignments of the best unstopped route
    double open = 0;          // the best route no defender can reach (a 1 v 0)
    double best = 0;          // the best route if nobody defended
    int shooter = -1;         // the shooter of the threat that decides `value`
    Cell cell{};              // its shooting cell
    int frame = -1;           // and the frame of the shot
    unsigned defenders = 0;   // defenders who can interfere with some route
    long routes = 0;
    bool freeBall = false;    // the ball was free: valued from its taker by the map
    bool restart = false;     // ... nobody meets it before an out or the keeper
    int taker = -1;           // the holder, or the player who takes the free ball
    Cell takeCell{};
    int takeFrame = -1;
};

// `loss`: a lost ball costs the other side's goal chance from where he wins it
// (the same measure: his best dribble to a shot); false: a loss is worth 0.
RouteStatic ComputeRouteStatic(const GameState& s, bool loss = true);

}  // namespace soccer::bot
