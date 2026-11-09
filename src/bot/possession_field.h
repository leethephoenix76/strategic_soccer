// Possession field on the reach map (07.11.2026, prototype for measurement).
//
// One reach map (reach_front.h) of a position, then a backward dynamic
// programme over frames: B(k, c, t) is the value for k's team of player k
// holding the ball on cell c at the start of his own frame t, in the
// possession scale (a goal +1, the other side's goal -1, a shot that misses or
// the end of the horizon with the ball: -W / +W). The holder can
//   - dribble to a cell of his map area (from c: standing start, never before
//     the map) and act there; ending a turn with the ball next to a defender
//     risks a tackle (TackleChance + knock-away, defenders as the map lets them);
//   - shoot: open shot times the blocking of defenders the map puts in the zone;
//   - pass (Chebyshev <= kMaxPassDistance) to a partner on the cell (to the
//     feet, or ahead: the ball waits there for him and a defender who can be
//     there first takes it), each defender by the line at his best;
//   - end the turn holding the ball.
// A lost ball is worth minus the other side's field where it wins it, so both
// teams' fields are computed together. Each defender interferes where the map
// says he can, each at his best separately (no joint assignment: the route
// static showed it adds nothing). Not in the search; measured by
// `ssbot possession-field`.
#pragma once

#include "engine/state.h"

#include <string>
#include <vector>

namespace soccer::bot {

struct PossessionField {
    bool valid = false;
    int team = -1;          // the team with the ball (or the free ball's taker's)
    double value = 0;       // for `team`, in the possession scale
    double white = 0;       // the same for White
    int holder = -1;        // the holder or the taker of a free ball
    bool freeBall = false;
    bool restart = false;
    char first = '-';       // the holder's best first action: s(hot), p(ass), d(ribble), h(old)
    int frames = 0;         // map frames
    long transitions = 0;
};

// `w`: the value of the possession at the horizon (+W for the holder) and of
// a missed shot (-W); 0 gives the plain "goal of this episode" scale.
// `passes`: passes allowed in one possession (a receiver's place comes from
// his map area, not from where he was: more passes would teleport partners).
// `territory`: the weight of the team's territory added to W at the horizon
// (its players nearer the goal), 0 - none.
PossessionField ComputePossessionField(const GameState& s, double w, int passes = 2, double territory = 0);

// Overload on the map (07.11.2026): each attacker's best shooting cell he
// reaches first with a shot worth more than keeping the ball (>= 2W/(1+W)),
// the defenders who can be on it or in its blocking zone before him, and the
// largest assignment of one defender per attacker. Attackers left over are
// free: a 1 v 0 if nobody can cover them, an overload of their coverers else.
struct OverloadAttacker { int player = -1; int cell = -1; int frame = 1 << 20; double shot = 0; unsigned cover = 0; };
struct ReachOverload {
    std::vector<OverloadAttacker> attackers;
    int matched = 0, deficit = 0;
    unsigned free = 0;         // attackers left without a defender
    unsigned overloaded = 0;   // the defenders those attackers depend on
};
struct ReachFront;
// `late`, `delay`: a free ball's taker (placed on the ball's cell) has it only
// `delay` frames later.
ReachOverload ComputeReachOverload(const GameState& s, const ReachFront& front, int team, double w,
                                   int late = -1, int delay = 0);

// Analysis of a held ball (JSON): the value, the map's arrival frames, the
// holder's categories of first ball action with their best lines and who acts
// on them, and the value and categories with each other player taken off.
std::string AnalyzePossession(const GameState& s, double w, int passes = 2);

// Delivery (08.11.2026, user): the danger the map finds - a free attacker
// (1 v 0) or attackers left to an overloaded defender (2 v 1) - is the
// target; a route brings the ball to one of them by the holder's dribble and
// at most `maxPasses` passes (to the feet or ahead, the ball waiting on its
// cell for the receiver), each pass through the defenders the map puts by
// its line in time. A route counts if at the delivery the danger is still
// there: from where and when he has the ball the target reaches a shooting
// cell (open shot >= 2W/(1+W)) before every defender but the overloaded ones
// can stand on it or in its blocking zone. Dribbling ends a turn next to a
// defender the map lets there: his tackle may win the ball. The value of a
// route is the chance to keep the ball to the delivery times the shot there
// (for a 2 v 1 the episode itself is left to the exact count). On the map of
// the position as a prototype for measurement (`ssbot delivery`).
struct DeliveryLeg {
    char kind = 'd';     // d(ribble one own turn), p(ass), r(eceive), h(old: the delivery)
    int player = -1;
    int from = -1, to = -1;   // cells
    int frame = 0;
    double keep = 1;     // the chance this leg keeps the ball
};
struct DeliveryRoute {
    int target = -1;          // the attacker the ball is delivered to
    int kind = 0;             // 1: 1 v 0, 2: against the overloaded defender(s)
    int passes = 0;
    double keep = 0;          // the chance the ball gets there
    double shot = 0;          // the open shot of the target's cell
    double value = 0;         // keep * shot
    int shotCell = -1, shotFrame = -1;
    std::vector<DeliveryLeg> legs;
    // The episode's participants for its exact count: the attackers who carry
    // or take the ball on the route, the target, and the defenders who can be
    // next to a cell of the route (or in the shot's blocking zone) by then;
    // the overloaded defenders always.
    unsigned participants = 0;
};
struct Delivery {
    bool valid = false;
    int team = -1, holder = -1, startCell = -1, startFrame = -1;
    bool freeBall = false;
    ReachOverload overload;
    unsigned targets1v0 = 0, targets2v1 = 0;
    std::vector<DeliveryRoute> routes;   // the best route per target and number of passes
    long states = 0;
};
Delivery ComputeDelivery(const GameState& s, double w = 0.2, int maxPasses = 2);
std::string FormatDelivery(const GameState& s, const Delivery& d);

}  // namespace soccer::bot
