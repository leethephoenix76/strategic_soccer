// Ball plans and their participants (docs/bot_design.md 7, "BallInfluenceCone",
// 02.11.2026): a measuring tool, not used by the search.
//
// In an attack node (the team to move holds the ball) the path of the ball in
// the next round is fixed by the team's choice - a plan: the cell the holder
// plays the ball from and what he does there (hold, shoot, pass into a
// partner's feet, pass into space) - up to the random touches on the way. The
// touches can come only from players near that path. A plan's participants are
// the players who can change what happens to the ball in the round (our
// half-turn and the opponent's reply; a ball passed into space until the race
// for it is decided), by the rules:
//   - the holder (and the receiver of a pass into the feet);
//   - a pass: players standing at the pass in the bounding box of its segment
//     (the engine's candidates for an interception or deflection); our own
//     players count if they stand there now (moving away first is a choice);
//   - a ball passed into space: players who may take it first or stand in the
//     bounding box of a step of the ball before the race is decided (ball race
//     windows, reach with inertia, other players ignored);
//   - the reply: opponents who can tackle the carrier at the end of our
//     half-turn - a cell of their one-move reach next to him with the attack
//     direction allowed (reach ignores other players, so this is a superset);
//   - a shot: players in the blocking zone of the shot.
// Everybody else is outside the plan: whatever he does, the ball's events are
// the same; he matters only for the position the round leaves.
// "Closure" lists outside players who may still interfere by blocking: our
// players whose one-move reach meets the blocking square (3x3 around the
// centre of movement) of a participating opponent, and opponents standing in
// such a square or next to a participant's centre.
#pragma once
#include "bot/goal_episode.h"
#include "engine/actions.h"
#include "engine/state.h"
#include <string>
#include <vector>

namespace soccer {
namespace bot {

enum class PlanKind { Hold, Shot, PassFeet, PassSpace };

struct BallPlan {
    PlanKind kind = PlanKind::Hold;
    Cell from;                  // where the holder plays the ball (his cell at the end for Hold)
    Cell target;                // a pass
    int receiver = kNoPlayer;   // PassFeet
};

struct PlanParticipants {
    BallPlan plan;
    unsigned own = 0;           // bit k: participant of the team to move (the holder included)
    unsigned opponent = 0;      // bit k: participant of the other team
    unsigned closure = 0;       // bit k: outside player who may interfere by blocking
    std::string why[kNumPlayers];
    // Movement states of our half-turn consistent with the plan (holder on
    // `from`, receiver on the target, nobody of ours on a space target):
    long ownFull = 0;           // all players of the team to move free
    long ownPlan = 0;           // the outside players stay
    // The opponent's reply after the plan (a representative state; Hold,
    // PassFeet, and PassSpace when the ball is still in play and theirs to move):
    long replyFull = -1;        // -1: no reply counted
    long replyPlan = -1;
};

std::string PlanName(const GameState& s, const BallPlan& p);

// Every plan of the team to move, which must hold the ball (empty otherwise).
// `replies`: also count the opponent's reply movement states (slower).
std::vector<PlanParticipants> AnalyzeBallPlans(const GameState& s, bool replies);

// The plan a sequence of actions of one half-turn plays (the holder's cell at
// the pass or shot, or at the end; the receiver standing on the target).
bool PlanOfActions(const GameState& s, const std::vector<Action>& actions, BallPlan* plan);


// ---- Ball chains over several rounds (02.11.2026) --------------------------
// A chain is what the team with the ball intends for its next `rounds`
// half-turns: in each round the carrier keeps the ball or passes to a partner
// (into his feet or ahead of him: the receiver is anywhere he can reach), and
// the chain may end with a shot. Where everybody can be is not known ahead,
// so every player is his reach region by then (inertia, other players
// ignored: a superset). An opponent takes part in a chain if in some round
// his region meets the bounding box of a pass (as he stands at the pass), the
// cells next to the carrier after our half-turn (his reply: a tackle) or the
// blocking zone of the shot. Our participants are the carriers. Participants
// only grow with the rounds: beyond some depth every chain has all opponents
// and planning the ball's path no longer separates anybody.
struct BallChain {
    std::string text;           // e.g. "W4 hold, >W1, W1 shot"
    unsigned own = 0, opponent = 0;
    int rounds = 0;
    bool shot = false;
    double shotUpper = 0;       // best open shot probability from the shooter's region (no blockers)
};
std::vector<BallChain> AnalyzeBallChains(const GameState& s, int rounds);

// ---- Ball routes with a moving front (02.11.2026) ---------------------------
// The chains above compare whole reach regions and so take in every defender.
// Here the ball follows a concrete route: the carrier dribbles cell by cell
// (inertia, other players ignored), passes to a partner standing on a cell
// (and with a velocity) he can have by then, and shoots from a cell. An
// opponent interferes only where and when the ball is: at the pass (he can
// stand in its bounding box by then), in his reply (he can stand next to the
// carrier) or at the shot (he can stand in the shot's blocking zone). A
// defender behind the play, who can not catch up with the ball's front, never
// interferes. For every set of opponents the best open shot (no blockers) of
// a route on which only these opponents can interfere is found by a dynamic
// programme over (carrier, cell, velocity, set). A shot counts every
// opponent of the set who can get into its blocking zone by then on his worst
// cell (the others can not be there). Our own players are
// optimistic (they never block each other).
struct BallRouteBest {
    double shot = 0;            // best shot probability (the set's opponents who can reach the shot's
                                // blocking zone each on his worst cell), 0: none
    Cell cell;                  // where from
    int carrier = kNoPlayer;
    int round = -1;             // in which of our half-turns (0: this one)
    unsigned own = 0;           // bit k: our players who carry the ball on the route
    std::string route;          // the route, e.g. "W4 g3-g5, >W1 g5>i9, W1 i9-h11, hold, ..., W1 shot f16"
};
// best[r][mask]: the best shot by the end of our half-turn r (0-based) with
// only the opponents of `mask` (bit i: the i-th opponent) able to interfere.
std::vector<std::vector<BallRouteBest>> BallRoutes(const GameState& s, int rounds);

}  // namespace bot
}  // namespace soccer
