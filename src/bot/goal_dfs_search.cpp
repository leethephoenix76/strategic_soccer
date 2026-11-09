#include "bot/goal_dfs_search.h"
#include "bot/ball_race.h"
#include "bot/geometric_static.h"
#include "bot/episode.h"
#include "bot/episode_value.h"
#include "engine/notation.h"

#include "bot/bot.h"

#include "bot/goal_bounds.h"
#include "bot/hash.h"
#include "bot/team_moves.h"
#include "bot/pass_pruning.h"
#include "bot/race.h"
#include "bot/reach.h"
#include "bot/reach_front.h"
#include "bot/value.h"

#include <algorithm>
#include <type_traits>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <map>
#include <vector>

namespace soccer {
namespace bot {

namespace {

using Clock = std::chrono::steady_clock;

struct ScopedTreeTimer {
    uint64_t* total;
    uint64_t* second;
    Clock::time_point begin;
    explicit ScopedTreeTimer(uint64_t* value, uint64_t* other = nullptr) : total(value), second(other) {
        if (total) begin = Clock::now();
    }
    ~ScopedTreeTimer() {
        if (total) {
            const auto elapsed = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
            *total += elapsed;
            if (second) *second += elapsed;
        }
    }
};

// A player's destinations in his team's frame (TeamOrder): the static's greedy
// choices keep the first of equal cells, so both teams break ties alike.
std::vector<Cell> TeamDestinations(const GameState& s, int player) {
    std::vector<Cell> cells = MoveDestinations(s, player);
    TeamOrder(cells, TeamOf(player));
    return cells;
}

bool Terminal(GoalEpisodeStatus s) {
    return s == GoalEpisodeStatus::OurGoal || s == GoalEpisodeStatus::TheirGoal ||
           s == GoalEpisodeStatus::NoGoal;
}

double Down(double x) { return std::nextafter(x, -std::numeric_limits<double>::infinity()); }
double Up(double x) { return std::nextafter(x, std::numeric_limits<double>::infinity()); }

GoalValueBounds Intersect(GoalValueBounds a, GoalValueBounds b) {
    GoalValueBounds both{std::max(a.lower, b.lower), std::min(a.upper, b.upper)};
    // Two valid bounds should always intersect. If arithmetic from a platform
    // violates that expectation, keep the enclosing interval, not a false proof.
    return both.lower <= both.upper ? both
                                    : GoalValueBounds{std::min(a.lower, b.lower), std::max(a.upper, b.upper)};
}

GoalValueBounds Scale(double pLower, double pUpper, GoalValueBounds b) {
    if (pUpper <= 0 || (b.lower == 0 && b.upper == 0)) return {0, 0};
    const double a = pLower * b.lower, c = pLower * b.upper;
    const double d = pUpper * b.lower, e = pUpper * b.upper;
    return {Down(std::min({a, c, d, e})), Up(std::max({a, c, d, e}))};
}

GoalValueBounds Add(GoalValueBounds a, GoalValueBounds b) {
    return {Down(a.lower + b.lower), Up(a.upper + b.upper)};
}

int ActionOrder(const GameState& s, const Action& a) {
    if (a.type == ActionType::Shot || a.type == ActionType::Attack) return 0;
    if (a.type == ActionType::Pass) return 1;
    if (a.type == ActionType::Move && a.player == s.ball.holder) return 2;
    if (a.type == ActionType::Move) return 3;
    if (a.type == ActionType::EndTurn) return 4;
    return 5;
}

// Approximate the next own turn after an uninterrupted direct pass. The
// opponent's intervening turn is left to the search; positions are fixed here.
double PartnerShotChanceNextTurn(const GameState& s, int receiver) {
    GameState next = s;
    PlayerState& p = next.players[receiver];
    p.prevTurnStart = p.turnStart;
    p.turnStart = p.pos;
    p.moved = false;
    p.attacked = false;
    next.ball.holder = receiver;
    next.ball.pos = p.pos;
    next.ballPlayed = false;
    next.teamToMove = TeamOf(receiver);
    return HolderShotChanceThisTurn(next);
}

double PartnerThreatNextTurn(const GameState& s) {
    if (s.phase != Phase::Play || s.ball.IsFree() || s.ballPlayed ||
        TeamOf(s.ball.holder) != s.teamToMove ||
        (s.halfTurns < kHalfTurnsPerHalf && s.halfTurns + 2 >= kHalfTurnsPerHalf) ||
        s.halfTurns + 2 >= kHalfTurnsPerMatch) return 0;

    const int holder = s.ball.holder;
    const int first = FirstPlayerOf(s.teamToMove);
    std::array<double, kPlayersPerTeam> bestByReceiver{};
    const auto evaluate = [&](const GameState& from) {
        for (int receiver = first; receiver < first + kPlayersPerTeam; ++receiver) {
            if (receiver == holder) continue;
            const Cell target = from.players[receiver].pos;
            if (!CanPassTo(from, target)) continue;
            const double through = PassThroughProbability(from, target);
            double& best = bestByReceiver[receiver - first];
            best = std::max(best, through * PartnerShotChanceNextTurn(from, receiver));
        }
    };
    evaluate(s);
    if (!s.players[holder].moved) {
        GameState after = s;
        after.players[holder].moved = true;
        for (Cell target : TeamDestinations(s, holder)) {
            if (target == s.players[holder].pos) continue;
            after.players[holder].pos = target;
            after.ball.pos = target;
            evaluate(after);
        }
    }

    // More dangerous receiving options increase the position's heuristic
    // threat. This union-like score is not a literal probability of scoring.
    double noThreat = 1;
    for (double chance : bestByReceiver) noThreat *= 1 - chance;
    return 1 - noThreat;
}

// A single full-length forward lead pass can keep rolling well past its target.
// Empty-field inertia reach is optimistic; the deeper search resolves the
// opponent's movement, later interceptions and the race for a free ball.
double LeadPassThreat(const GameState& s) {
    if (s.phase != Phase::Play || s.ball.IsFree() || s.ballPlayed ||
        TeamOf(s.ball.holder) != s.teamToMove) return 0;

    const int team = s.teamToMove;
    const Cell from = s.ball.pos;
    const int direction = Sign(AttackedGoalRow(team) - from.y);
    const Cell target{from.x, from.y + direction * kMaxPassDistance};
    if (!CanPassTo(s, target) || PlayerAt(s, target) != kNoPlayer) return 0;
    const double through = PassThroughProbability(s, target);
    if (through <= 0) return 0;
    const std::vector<Cell> path = PredictPassPath(s, target);
    const bool stopsInField = path.size() == kMaxPassDistance &&
                              path.back().x > 0 && path.back().x < kFieldWidth - 1 &&
                              path.back().y > kFirstRow && path.back().y < kLastRow;
    double noThreat = 1;
    for (int receiver = FirstPlayerOf(team);
         receiver < FirstPlayerOf(team) + kPlayersPerTeam; ++receiver) {
        if (receiver == s.ball.holder) continue;
        const PlayerState& p = s.players[receiver];
        const ReachMap reach = p.moved ? ComputeReach(p.pos, p.Displacement())
                                       : PlayerReach(s, receiver);
        double bestShot = 0;
        GameState received = s;
        for (std::size_t step = 0; step < path.size(); ++step) {
            const Cell cell = path[step];
            if (InGoalMouth(cell.x, cell.y)) break;
            const int occupant = PlayerAt(s, cell);
            if (occupant != kNoPlayer && occupant != receiver) break;
            // At the last landing, a stopped ball remains available for one
            // more own turn (as in the long pass in game2).
            const int movesAvailable = static_cast<int>(step) + (p.moved ? 0 : 1) +
                                       (stopsInField && step + 1 == path.size() ? 1 : 0);
            if (reach.Turns(cell) > movesAvailable) continue;
            received.players[receiver].pos = cell;
            bestShot = std::max(bestShot, ShotProbability(received, receiver));
        }
        noThreat *= 1 - through * bestShot;
    }
    return 1 - noThreat;
}

double PartnerOptionsThreat(const GameState& s) {
    const double direct = PartnerThreatNextTurn(s);
    return 1 - (1 - direct) * (1 - LeadPassThreat(s));
}

// The same attack model is used for either team after a controlled turnover.
// It deliberately contains no defensive reply, so the one-reply risk estimate
// below does not recurse into another forecast of the opponent's response.
double ActiveAttackThreat(const GameState& s) {
    const double shot = HolderShotChanceThisTurn(s);
    return 1 - (1 - shot) * (1 - PartnerOptionsThreat(s));
}

double AttackThreatWhenNextToMove(const GameState& s, bool includePassOptions) {
    if (s.ball.IsFree()) return 0;
    const int team = TeamOf(s.ball.holder);
    if (s.teamToMove == team && !s.ballPlayed)
        return includePassOptions ? ActiveAttackThreat(s) : HolderShotChanceThisTurn(s);

    const int nextHalfTurn = s.halfTurns + (s.teamToMove == team ? 2 : 1);
    if ((s.halfTurns < kHalfTurnsPerHalf && nextHalfTurn >= kHalfTurnsPerHalf) ||
        nextHalfTurn >= kHalfTurnsPerMatch) return 0;

    GameState next = s;
    if (s.teamToMove == team) {
        for (int player = FirstPlayerOf(team);
             player < FirstPlayerOf(team) + kPlayersPerTeam; ++player) {
            PlayerState& p = next.players[player];
            p.prevTurnStart = p.turnStart;
            p.turnStart = p.pos;
            p.moved = false;
            p.attacked = false;
        }
    }
    next.teamToMove = team;
    next.ballPlayed = false;
    next.halfTurns = nextHalfTurn;
    return includePassOptions ? ActiveAttackThreat(next) : HolderShotChanceThisTurn(next);
}

struct ReplyRisk {
    double keep = 1;
    double counter = 0;
};

// For each defender, test a legal tackle from the current cell or after that
// defender's move. Combining their best attempts is a cheap approximation of
// one opposing team turn; movement conflicts and knock-away recovery are left
// to the deeper minimax.
ReplyRisk OpponentTackleRisk(const GameState& s, int defending, bool withCounter = true) {
    GameState reply = s;
    reply.teamToMove = defending;
    reply.ballPlayed = false;
    double noCounter = 1;
    ReplyRisk risk;
    for (int defender = FirstPlayerOf(defending);
         defender < FirstPlayerOf(defending) + kPlayersPerTeam; ++defender) {
        const PlayerState& player = reply.players[defender];
        // One move ends at most two cells from turnStart; a tackle needs one
        // more cell. This rejects distant defenders before MoveDestinations.
        if (ChebyshevDistance(player.pos, reply.ball.pos) > 1 &&
            (player.moved || ChebyshevDistance(player.turnStart, reply.ball.pos) > 3))
            continue;
        double bestWonChance = -1;
        double bestCounterThreat = 0;
        const auto consider = [&](const GameState& attempt) {
            if (GetAttackKind(attempt, defender) != AttackKind::Tackle) return;
            const double wonChance = TackleChance(attempt.ball.pos,
                                                   attempt.players[defender].pos, defending);
            bestWonChance = std::max(bestWonChance, wonChance);
            if (!withCounter) return;
            GameState won = attempt;
            PlayerState& attacker = won.players[defender];
            attacker.attacked = true;
            attacker.attackDir = {won.ball.pos.x - attacker.pos.x,
                                  won.ball.pos.y - attacker.pos.y};
            won.ball.holder = defender;
            won.ball.pos = attacker.pos;
            won.ball.attackTeam = defending;
            bestCounterThreat = std::max(bestCounterThreat,
                                         wonChance * ActiveAttackThreat(won));
        };
        consider(reply);
        if (!reply.players[defender].moved) {
            GameState moved = reply;
            moved.players[defender].moved = true;
            for (Cell target : TeamDestinations(reply, defender)) {
                if (target == reply.players[defender].pos) continue;
                moved.players[defender].pos = target;
                consider(moved);
            }
        }
        if (bestWonChance < 0) continue;
        risk.keep *= 1 - bestWonChance - kKnockAwayChance;
        noCounter *= 1 - bestCounterThreat;
    }
    risk.counter = 1 - noCounter;
    return risk;
}

// Used only while building the movement beam. Full turnover scoring is delayed
// until a complete movement state or action is ranked.
double QuickKeepChance(const GameState& s, int defending) {
    double keep = 1;
    for (int player = FirstPlayerOf(defending);
         player < FirstPlayerOf(defending) + kPlayersPerTeam; ++player) {
        const Cell defender = s.players[player].pos;
        const int distance = ChebyshevDistance(defender, s.ball.pos);
        if (distance > 2) continue;
        double threat = 0;
        if (distance == 1 && AttackDirectionAllowed(defender, s.ball.pos,
                s.players[player].Velocity(), s.players[player].Displacement()))
            threat = TackleChance(s.ball.pos, defender, defending) + kKnockAwayChance;
        else if (distance == 2) {
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    Cell cell{s.ball.pos.x + dx, s.ball.pos.y + dy};
                    if (!InField(cell) || (dx == 0 && dy == 0) || PlayerAt(s, cell) != kNoPlayer)
                        continue;
                    if (ChebyshevDistance(defender, cell) <= 1)
                        threat = std::max(threat, TackleChance(s.ball.pos, cell, defending) +
                                                    kKnockAwayChance);
                }
        }
        keep *= 1 - std::min(1.0, std::max(0.0, threat));
    }
    return keep;
}

// The restart after an out, a keeper's catch or a back pass to one's own keeper:
// both teams take the standard formation (the selective bots always play it),
// the restarting team then has the ball at the centre. Unchanged if the line-up
// does not end in play.
GameState StandardRestart(const GameState& s) {
    GameState t = s;
    for (int guard = 0; guard < 4 * kNumPlayers && t.phase == Phase::Lineup; ++guard) {
        const Action a = LineupAction(t);
        if (Validate(t, a) != ActionError::None) break;
        ApplyUnchecked(t, a, Outcome::Certain());
    }
    return t;
}

// A common, deliberately cheap value scale for both sides of the minimax.
// It estimates the current attack (or the one allowed counterattack), rather
// than rewarding possession for its own sake. This is not a probability bound.
// `restartAsPosition`: a restart is the position it leads to, so losing the
// ball out of play costs the opponent's attack from the centre (otherwise 0).
// `ballStep`: a rolling free ball is valued where its next forced step takes
// it (an out, a touch by a standing player, the next cell), with every player
// standing still; the replies themselves are left to the search.
double PositionEstimate(const GameState& s, GoalEpisode episode, bool detailedRisk = true,
                        bool restartAsPosition = false, bool ballStep = false,
                        bool raceFreeBall = false);

// ---- The attack valued against the opponent's reply (7.61) -------------------
// The attacker's plans are compared by their value against the move the
// defending team still has before the shot: a shot now, or the ball kept or
// passed this turn (a partner may run first and take it in his feet) and a shot
// on the next own turn. Between them the defenders reply: each in turn takes
// the cell (with inertia and blocking, MoveDestinations) that most lowers the
// shooter's best shot, and they may tackle the new holder (OpponentTackleRisk).
// A plan is one line of play: the best plan counts, not the union of options.
thread_local bool tReplyAware = false;
NodeDiagTarget gNodeDiag;

// ---- Possession beyond the episode (7.62) ------------------------------------
// With the continuation on, an episode that ends without a goal is worth the
// possession that follows (ValueTable::PossessionDefault, bot/value.h:
// the expected goal difference for the side with the ball, discounted 0.95
// per round), and a plan of the attack may keep the ball instead of shooting.
// Near the end of a half there is no next possession: within two half-turns of
// it, and on the first half-turn of the second half (a half-time kickoff is
// not told apart from an out there), the continuation is 0, so the honest
// bounds of the last half-turn (goal_bounds.cpp) stay valid.
thread_local bool tContinuation = false;
constexpr double kContinuationDiscount = 0.95;   // per round, as in the possession table

bool ContinuationAllowed(const GameState& s) {
    const int h = s.halfTurns;
    return h < kHalfTurnsPerHalf - 2 || (h > kHalfTurnsPerHalf && h < kHalfTurnsPerMatch - 2);
}

// The value for `team` of the possession that starts in `s` (a turn start, a
// restart or a state within a turn).
double ContinuationValue(const GameState& s, int team) {
    if (!ContinuationAllowed(s)) return 0;
    return ValueTable::PossessionDefault().OfState(s, team);
}

// The possession value (07.11.2026): +-W for `team` by who has the ball in
// `s` - the holder, the restarting team after the standard line-up, the
// winner of a free ball's race (0 if contested); 0 near the end of a half.
double PossessionValue(const GameState& s, int team, double w) {
    if (w <= 0 || !ContinuationAllowed(s)) return 0;
    GameState t = s;
    if (t.phase == Phase::Lineup) t = StandardRestart(t);
    if (t.phase != Phase::Play) return 0;
    int owner = -1;
    if (!t.ball.IsFree()) owner = TeamOf(t.ball.holder);
    else {
        const BallRaceResult r = SolveBallRace(t);
        if (r.kind == BallRaceKind::Taken || r.kind == BallRaceKind::Restart) owner = r.team;
    }
    if (owner < 0) return 0;
    return owner == team ? w : -w;
}

// The value for `team` of holding the ball in `cell` at its next turn start.
double HoldValue(const GameState& s, Cell cell, int team) {
    if (!ContinuationAllowed(s)) return 0;
    return kContinuationDiscount * ValueTable::PossessionDefault().Of(BallOwner::Mover, cell, team);
}

// The start of `team`'s next turn: its per-turn flags as the engine resets them.
GameState TurnStartOf(GameState t, int team) {
    for (int k = FirstPlayerOf(team); k < FirstPlayerOf(team) + kPlayersPerTeam; ++k) {
        PlayerState& p = t.players[k];
        p.prevTurnStart = p.turnStart;
        p.turnStart = p.pos;
        p.moved = false;
        p.attacked = false;
    }
    t.teamToMove = team;
    t.ballPlayed = false;
    return t;
}

// `t`: the defenders to move, the shooter holds the ball. The defenders' greedy
// reply against his best shot on his next turn; returns that shot, `t` replied.
double ShotAfterReply(GameState& t, int shooter, int defending) {
    const int attacking = Opponent(defending);
    std::array<int, kPlayersPerTeam> order{};
    for (int i = 0; i < kPlayersPerTeam; ++i) order[i] = FirstPlayerOf(defending) + i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return ChebyshevDistance(t.players[a].pos, t.players[shooter].pos) <
               ChebyshevDistance(t.players[b].pos, t.players[shooter].pos);
    });
    // The shooter's cells on his next turn (his move with inertia, the
    // defenders as they stand now); the reply is scored against all of them.
    // A shot is OpenShotProbability times one factor per player (ShotBlockFactor),
    // so moving one defender changes one factor.
    const GameState start = TurnStartOf(t, attacking);
    std::vector<Cell> cells{t.players[shooter].pos};
    for (Cell c : TeamDestinations(start, shooter))
        if (!(c == t.players[shooter].pos)) cells.push_back(c);
    std::vector<double> base(cells.size());
    for (int d : order) {
        if (t.players[d].moved) continue;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            double p = OpenShotProbability(cells[i], attacking);
            for (int k = 0; k < kNumPlayers; ++k)
                if (k != shooter && k != d) p *= ShotBlockFactor(cells[i], attacking, t.players[k].pos);
            base[i] = p;
        }
        const auto shotWith = [&](Cell x) {
            double b = 0;
            for (std::size_t i = 0; i < cells.size(); ++i) {
                if (cells[i] == x) continue;
                const int occupant = PlayerAt(t, cells[i]);
                if (occupant != kNoPlayer && occupant != shooter && occupant != d) continue;
                b = std::max(b, base[i] * ShotBlockFactor(cells[i], attacking, x));
            }
            return b;
        };
        const Cell from = t.players[d].pos;
        Cell bestCell = from;
        double best = shotWith(from);
        for (Cell c : TeamDestinations(t, d)) {
            if (c == from) continue;
            const double v = shotWith(c);
            if (v < best) {
                best = v;
                bestCell = c;
            }
        }
        t.players[d].pos = bestCell;
        t.players[d].moved = true;
    }
    return HolderShotChanceThisTurn(TurnStartOf(t, attacking));
}

// The value of one plan: `t` is the position after the attacker's turn with
// `shooter` holding the ball, the defenders to move.
// `counter`: charge the counterattack after a won tackle (the keep plans of
// our own attack; a pass's receiver is charged only the lost chance to keep).
// With the continuation (7.62) the attacker on his next turn shoots or keeps
// the ball (the better of the two), and a lost ball not charged as a
// counterattack is the opponent's possession in that cell.
double PlanAfterReply(GameState t, int shooter, int defending, bool counter) {
    const ReplyRisk risk = OpponentTackleRisk(t, defending, counter);
    const Cell cell = t.players[shooter].pos;
    double next = ShotAfterReply(t, shooter, defending);
    if (!tContinuation) return risk.keep * next - (counter ? risk.counter : 0);
    next = std::max(next, HoldValue(t, cell, Opponent(defending)));
    const double loss = counter ? risk.counter : (1 - risk.keep) * HoldValue(t, cell, defending);
    return risk.keep * next - loss;
}

// ---- Support over several turns (7.63, 7.64) --------------------------------
// The explicit plans look one own turn ahead. Beyond it, the attack's options
// are judged by reach with inertia (reach.h, other players ignored): a shot
// from cell x on own turn k + 1 (2 <= k <= the horizon; k = 1 is the explicit
// plans) by
// - the holder, who dribbles to x by turn k, or
// - a partner r who stands on x by turn k and gets the ball there by one real
//   pass of the holder on turn k from a cell the holder reaches by then (the
//   cell nearest to x): a pass to the feet (at most kMaxPassDistance per axis),
//   or, when x is farther, a pass into space that rolls on and reaches x at the
//   opponent's End Turn (offset 2n - 1 per axis for a pass of n, n <= 5; the
//   shortest such pass). Longer chains are the search's: its first plies make
//   the first passes.
// Each defender looks after one attacker; the defence chooses the assignment
// that minimises the best option. A defender looking after the shooter
// - takes x away if he can stand on it first (on a pass into space: on x or on
//   the cell where the ball waits during his turn, by his turn k);
// - intercepts the pass: InterceptionWeight of his best cell, among those he
//   can reach before the ball passes (k - 1 turns for the pass, k for the
//   rolling step), inside the segment's bounding box (the engine's rule);
// - tackles a ball received at the feet from a cell next to x he reaches within
//   k turns (a ball into space arrives after his turn: no tackle);
// - blocks the shot (the best ShotBlockFactor over the cells of the shot line
//   he reaches within k turns).
// Option: 0.95^(k - 1) * OpenShotProbability(x) * the product over the
// shooter's defenders of (1 - interception) (1 - tackle) * block. An attacker
// who has moved this turn, and every defender, counts his turns from his
// team's next turn; the attackers move first in a round.
thread_local int tSupportHorizon = 0;
// The free ball by the race from the state as it is, the defenders' turns
// before the pickup (7.71).
thread_local bool tRaceDefence = false;

// Cells of the shot line from c to the centre of the attacked goal, c excluded.
int ShotLine(Cell c, int team, std::array<Cell, kGridHeight>* line) {
    const int goalRow = AttackedGoalRow(team);
    const int dy = goalRow - c.y, dx = kGoalCenterX - c.x;
    const int steps = std::max(Abs(dx), Abs(dy));
    int n = 0;
    for (int i = 1; i < steps; ++i) {
        const Cell b{c.x + static_cast<int>(std::lround(double(dx) * i / steps)),
                     c.y + static_cast<int>(std::lround(double(dy) * i / steps))};
        if (InField(b)) (*line)[n++] = b;
    }
    return n;
}

double SupportValue(const GameState& s, int attacking, double floor) {
    const int defending = Opponent(attacking);
    const int holder = s.ball.holder;
    const int a0 = FirstPlayerOf(attacking), d0 = FirstPlayerOf(defending);
    std::array<ReachMap, kNumPlayers> reach;
    std::array<int, kNumPlayers> offset{};
    for (int k = 0; k < kNumPlayers; ++k) {
        const PlayerState& p = s.players[k];
        const bool fromNext = TeamOf(k) == defending || p.moved;
        reach[k] = ComputeReach(p.pos, fromNext ? p.Displacement() : p.Velocity());
        offset[k] = TeamOf(k) == attacking && p.moved ? 1 : 0;
    }
    const auto turnsOf = [&](int k, Cell c) {
        const int t = reach[k].Turns(c);
        if (t == kUnreachable) return kUnreachable;
        return t == 0 ? 0 : t + offset[k];
    };
    // Ties below (the nearest holder's cell, the shortest pass) are broken in
    // the attacking team's frame, alike for both teams (TeamOrder).
    const int frame = attacking == kWhite ? 1 : -1;
    // The holder's cells by turn k (he may still pass on turn k).
    std::vector<Cell> holderCells[kGridHeight];
    for (int x = 0; x < kFieldWidth; ++x)
        for (int y = kFirstRow; y <= kLastRow; ++y) {
            const int t = turnsOf(holder, {x, y});
            for (int k = std::max(t, 1); k <= tSupportHorizon && k < kGridHeight; ++k)
                holderCells[k].push_back({x, y});
        }
    for (std::vector<Cell>& cells : holderCells) TeamOrder(cells, attacking);
    // The best interception of a segment by defender d among cells he reaches within `turns`.
    const auto intercept = [&](int d, Cell from, Cell to, int turns) {
        const int dx = to.x - from.x, dy = to.y - from.y;
        const double len = std::sqrt(double(dx * dx + dy * dy));
        if (len == 0) return 0.0;
        double best = 0;
        for (int x = std::min(from.x, to.x); x <= std::max(from.x, to.x); ++x)
            for (int y = std::min(from.y, to.y); y <= std::max(from.y, to.y); ++y) {
                const Cell c{x, y};
                if (c == from || c == to || !InField(c) || turnsOf(d, c) > turns) continue;
                const double dist = std::fabs(dx * double(y - from.y) - dy * double(x - from.x)) / len;
                best = std::max(best, InterceptionWeight(dist));
            }
        return best;
    };
    // threat[r][S]: r's best option when the defenders of the set S look after him.
    std::array<std::array<double, 16>, kPlayersPerTeam> threat{};
    std::array<Cell, kGridHeight> line;
    for (int x = 0; x < kFieldWidth; ++x)
        for (int y = kFirstRow; y <= kLastRow; ++y) {
            const Cell c{x, y};
            const double open = OpenShotProbability(c, attacking);
            if (open <= floor) continue;
            int n = -1;
            for (int k = 2; k <= tSupportHorizon; ++k) {
                const double top = std::pow(kContinuationDiscount, k - 1) * open;
                if (top <= floor) break;
                // The holder's cell for a pass on turn k: the one nearest to x.
                Cell from{-1, -1};
                int nearest = kUnreachable;
                for (Cell a : holderCells[k]) {
                    const int dist = ChebyshevDistance(a, c);
                    if (dist < nearest) { nearest = dist; from = a; }
                }
                for (int i = 0; i < kPlayersPerTeam; ++i) {
                    const int r = a0 + i;
                    const int t = turnsOf(r, c);
                    if (t == kUnreachable || t > k) continue;
                    // How the ball gets to x: 0 dribble, 1 pass to the feet, 2 pass into space.
                    int kind = r == holder ? 0 : -1;
                    Cell wait{};   // where a ball into space waits during the defenders' turn
                    Cell passer = from;
                    if (r != holder) {
                        if (nearest >= 1 && nearest <= kMaxPassDistance) kind = 1;
                        else {
                            // The shortest pass into space from a holder's cell that rolls to x.
                            int bestLength = kUnreachable;
                            for (int fx = -kMaxPassDistance; fx <= kMaxPassDistance; ++fx)
                                for (int fy = -kMaxPassDistance; fy <= kMaxPassDistance; ++fy) {
                                    const int nx = frame * fx, ny = frame * fy;
                                    const int length = std::max(Abs(nx), Abs(ny));
                                    if (length == 0 || length >= bestLength) continue;
                                    const Cell a{c.x - (2 * nx - Sign(nx)), c.y - (2 * ny - Sign(ny))};
                                    const Cell d{a.x + nx, a.y + ny};
                                    if (!InField(a) || !InField(d) || turnsOf(holder, a) > k) continue;
                                    bestLength = length;
                                    passer = a;
                                    wait = d;
                                }
                            if (bestLength != kUnreachable) kind = 2;
                        }
                        if (kind < 0) continue;
                    }
                    if (n < 0) n = ShotLine(c, attacking, &line);
                    std::array<double, kPlayersPerTeam> factor;
                    for (int j = 0; j < kPlayersPerTeam; ++j) {
                        const int d = d0 + j;
                        double f = 1;
                        if (kind == 2 ? turnsOf(d, c) <= k || turnsOf(d, wait) <= k : turnsOf(d, c) < t) {
                            factor[j] = 0;
                            continue;
                        }
                        if (kind == 1) f *= 1 - intercept(d, passer, c, k - 1);
                        if (kind == 2) f *= (1 - intercept(d, passer, wait, k - 1)) * (1 - intercept(d, wait, c, k));
                        if (kind != 2) {
                            double tackle = 0;
                            for (int ex = -1; ex <= 1; ++ex)
                                for (int ey = -1; ey <= 1; ++ey) {
                                    const Cell e{x + ex, y + ey};
                                    if ((ex == 0 && ey == 0) || !InField(e) || turnsOf(d, e) > k) continue;
                                    tackle = std::max(tackle, TackleChance(c, e, defending));
                                }
                            f *= 1 - tackle;
                        }
                        double block = 1;
                        for (int b = 0; b < n; ++b)
                            if (turnsOf(d, line[b]) <= k) block = std::min(block, ShotBlockFactor(c, attacking, line[b]));
                        factor[j] = f * block;
                    }
                    for (int set = 0; set < 16; ++set) {
                        double v = top;
                        for (int j = 0; j < kPlayersPerTeam; ++j)
                            if (set >> j & 1) v *= factor[j];
                        threat[i][set] = std::max(threat[i][set], v);
                    }
                }
            }
        }
    // The defence's best assignment: each defender to one attacker.
    double best = 1;
    for (int code = 0; code < 256; ++code) {
        std::array<int, kPlayersPerTeam> set{};
        for (int j = 0; j < kPlayersPerTeam; ++j) set[code >> (2 * j) & 3] |= 1 << j;
        double worst = 0;
        for (int i = 0; i < kPlayersPerTeam; ++i) worst = std::max(worst, threat[i][set[i]]);
        best = std::min(best, worst);
    }
    return std::max(best, floor);
}

// The attacker to move, the ball not played yet.
double AttackerToMoveValue(const GameState& s, int attacking, bool ourAttack) {
    const int defending = Opponent(attacking);
    const int holder = s.ball.holder;
    double best = HolderShotChanceThisTurn(s);
    const auto afterTurn = [&](GameState t, int shooter, Cell cell) {
        t.ball.holder = shooter;
        t.ball.pos = cell;
        t.ballPlayed = true;
        return TurnStartOf(t, defending);
    };
    // Keep: the holder's two best cells by the next-turn shot without a reply
    // (with the continuation, 7.62: or by the possession held there).
    {
        std::vector<std::pair<double, Cell>> cells;
        std::vector<Cell> dest{s.players[holder].pos};
        if (!s.players[holder].moved)
            for (Cell c : TeamDestinations(s, holder)) if (!(c == s.players[holder].pos)) dest.push_back(c);
        for (Cell c : dest) {
            GameState t = s;
            t.players[holder].pos = c;
            t.players[holder].moved = true;
            t.ball.pos = c;
            double proxy = ShotProbability(t, holder);
            if (tContinuation) proxy = std::max(proxy, HoldValue(t, c, attacking));
            cells.push_back({proxy, c});
        }
        std::stable_sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (std::size_t i = 0; i < cells.size() && i < 2; ++i) {
            GameState t = s;
            t.players[holder].pos = cells[i].second;
            t.players[holder].moved = true;
            best = std::max(best, PlanAfterReply(afterTurn(t, holder, cells[i].second), holder,
                                                 defending, ourAttack));
        }
    }
    // Passes: to each partner where he stands or after his move this turn.
    for (int r = FirstPlayerOf(attacking); r < FirstPlayerOf(attacking) + kPlayersPerTeam; ++r) {
        if (r == holder) continue;
        std::vector<Cell> dest{s.players[r].pos};
        if (!s.players[r].moved)
            for (Cell c : TeamDestinations(s, r)) if (!(c == s.players[r].pos)) dest.push_back(c);
        double bestProxy = -1, bestDeliver = 0;
        GameState bestState;
        Cell bestCell{};
        for (Cell c : dest) {
            GameState t = s;
            t.players[r].pos = c;
            if (!(c == s.players[r].pos)) t.players[r].moved = true;
            if (!CanPassTo(t, c)) continue;
            const double deliver = PassThroughProbability(t, c);
            if (deliver <= 0) continue;
            GameState received = t;
            received.ball.holder = r;
            received.ball.pos = c;
            double next = ShotProbability(received, r);
            if (tContinuation) next = std::max(next, HoldValue(received, c, attacking));
            const double proxy = deliver * next;
            if (proxy > bestProxy) {
                bestProxy = proxy;
                bestDeliver = deliver;
                bestState = t;
                bestCell = c;
            }
        }
        if (bestProxy <= 0) continue;
        best = std::max(best, bestDeliver * PlanAfterReply(afterTurn(bestState, r, bestCell), r,
                                                            defending, false));
    }
    // Support (7.63): the best option over 2..N own turns, charged the risk of
    // the holder keeping the ball where he stands now.
    if (tSupportHorizon >= 2) {
        GameState t = s;
        t.players[holder].moved = true;
        const GameState after = afterTurn(t, holder, s.players[holder].pos);
        const ReplyRisk risk = OpponentTackleRisk(after, defending, ourAttack);
        const double loss = ourAttack ? risk.counter
                            : tContinuation ? (1 - risk.keep) * HoldValue(after, s.players[holder].pos, defending)
                                            : 0;
        if (risk.keep > 0) {
            const double floor = (best + loss) / risk.keep;
            const double support = SupportValue(s, attacking, floor);
            if (support > floor) best = std::max(best, risk.keep * support - loss);
        }
    }
    return best;
}

// The defenders move before the attacker's next turn: their greedy reply to
// the holder's shot (and the tackle on him), then the attacker's turn.
double ReplyAwareAttack(const GameState& s, int attacking, bool ourAttack) {
    const int defending = Opponent(attacking);
    if (s.teamToMove == attacking && !s.ballPlayed) return AttackerToMoveValue(s, attacking, ourAttack);
    GameState t = s.teamToMove == attacking ? TurnStartOf(s, defending) : s;
    const ReplyRisk risk = OpponentTackleRisk(t, defending);
    ShotAfterReply(t, t.ball.holder, defending);
    const double next = AttackerToMoveValue(TurnStartOf(t, attacking), attacking, ourAttack);
    if (tContinuation && !ourAttack)
        return risk.keep * next - (1 - risk.keep) * HoldValue(t, t.players[t.ball.holder].pos, defending);
    return risk.keep * next - (ourAttack ? risk.counter : 0);
}

// A free ball by the race (race.h): the two runners that matter play the race
// for it exactly by the rules (the value only, no line of best play). Each team
// is then valued holding the ball where its runner can first stand on the
// ball's untouched path (the race's lower bound), the others where they are:
// value = p(White) V(White's runner holds) + (1 - p(White)) V(Black's runner holds).
// Budget of the race (7.60): 6 half-turns, branches below 1% and states beyond
// 60 guessed (the race's own cheap guess); a runner a whole round earlier wins
// without solving. At the kickoff the root values agree to 1e-5 with 8 / 600.
// Returns false when no runner can reach the ball.
bool RaceFreeBallEstimate(const GameState& s, GoalEpisode episode, bool restartAsPosition,
                          double* value) {
    static thread_local std::unordered_map<uint64_t, double> cache;
    const uint64_t key = PositionHash(s) ^ (static_cast<uint64_t>(episode.team + 1) << 61) ^
                         (static_cast<uint64_t>(episode.status) << 56) ^ (restartAsPosition ? 1ull << 55 : 0) ^
                         (tReplyAware ? 1ull << 54 : 0) ^ (tContinuation ? 1ull << 53 : 0) ^
                         (static_cast<uint64_t>(tSupportHorizon & 15) << 49) ^ (tRaceDefence ? 1ull << 47 : 0);
    const auto hit = cache.find(key);
    if (hit != cache.end()) {
        *value = hit->second;
        return true;
    }
    RaceOptions ro;
    ro.horizon = 6;
    ro.epsilon = 0.01;
    ro.maxStates = 60;
    ro.line = false;
    // A runner a whole round (two half-turns) earlier gets the ball: no solve.
    ro.clearMargin = 2;
    const RaceResult r = Race(s, ro);
    if (!r.valid || (r.runner[kWhite] == kNoPlayer && r.runner[kBlack] == kNoPlayer)) return false;
    const auto holds = [&](int team) {
        const int k = r.runner[team];
        if (k == kNoPlayer || r.bound[team] < 0 ||
            r.bound[team] >= static_cast<int>(r.path.size())) return 0.0;
        GameState h = s;
        const Cell cell = r.path[r.bound[team]];
        const int occupant = PlayerAt(h, cell);
        if (occupant != kNoPlayer && occupant != k) return 0.0;
        PlayerState& p = h.players[k];
        p.pos = p.turnStart = p.prevTurnStart = cell;
        h.ball.holder = k;
        h.ball.pos = cell;
        h.ball.vel = {0, 0};
        h.ball.lastTouchTeam = team;
        const GoalEpisode next = AdvanceGoalEpisode(episode, s, h, EventLog());
        if (tRaceDefence) {
            // The runner gets the ball at half-turn bound; the other team has its
            // turns before that: each one its greedy reply against his shot
            // (ShotAfterReply, as in the static's attack plans). Then the side
            // whose half-turn it is moves; the other players stand.
            const int defending = Opponent(team);
            const auto mover = [&](int i) { return i % 2 == 0 ? s.teamToMove : Opponent(s.teamToMove); };
            for (int i = 0; i < r.bound[team]; ++i) {
                if (mover(i) != defending) continue;
                GameState g = TurnStartOf(h, defending);
                ShotAfterReply(g, k, defending);
                h = g;
            }
            h = TurnStartOf(h, mover(r.bound[team]));
            h.players[k].prevTurnStart = h.players[k].turnStart = h.players[k].pos;
            h.players[k].moved = false;
        }
        return PositionEstimate(h, next, true, restartAsPosition, false, false);
    };
    const double v = r.pWhite * holds(kWhite) + (1 - r.pWhite) * holds(kBlack);
    if (cache.size() > (1u << 18)) cache.clear();
    cache.emplace(key, v);
    *value = v;
    return true;
}

double BallStepEstimate(const GameState& s, GoalEpisode episode, bool detailedRisk,
                        bool restartAsPosition, int endTurns, bool raceFreeBall) {
    double value = 0;
    for (const Outcome& outcome : OutcomesUnchecked(s, Action::EndTurn())) {
        GameState child = s;
        EventLog events;
        ApplyUnchecked(child, Action::EndTurn(), outcome, &events);
        const GoalEpisode next = AdvanceGoalEpisode(episode, s, child, events);
        // The ball waits one End Turn after a pass: then its step comes at the next one.
        const bool waited = child.phase == Phase::Play && child.ball.IsFree() &&
                            child.ball.pos == s.ball.pos && child.ball.vel == s.ball.vel;
        value += outcome.probability *
                 (waited && endTurns > 1 && !Terminal(next.status)
                      ? BallStepEstimate(child, next, detailedRisk, restartAsPosition, endTurns - 1,
                                         raceFreeBall)
                      : PositionEstimate(child, next, detailedRisk, restartAsPosition, false,
                                         raceFreeBall));
    }
    return value;
}

double PositionEstimate(const GameState& s, GoalEpisode episode, bool detailedRisk,
                        bool restartAsPosition, bool ballStep, bool raceFreeBall) {
    if (Terminal(episode.status)) {
        if (tContinuation && episode.status == GoalEpisodeStatus::NoGoal)
            return ContinuationValue(s, episode.team);
        return InitialGoalBounds(episode.status).lower;
    }
    if (restartAsPosition && s.phase == Phase::Lineup) {
        const GameState restart = StandardRestart(s);
        if (restart.phase == Phase::Play)
            return PositionEstimate(restart, episode, detailedRisk, false, false, raceFreeBall);
    }
    if (s.phase != Phase::Play) return 0;
    // With the race (7.71) a free ball is the race's from the state as it is:
    // the race plays the ball's steps and both runners' turns by the rules.
    // The step preview below plays the step at this team's End Turn without
    // its move, which gives a rolling ball to the other side.
    if (tRaceDefence && raceFreeBall && detailedRisk && s.ball.IsFree()) {
        double raced = 0;
        if (RaceFreeBallEstimate(s, episode, restartAsPosition, &raced)) return raced;
    }
    if (ballStep && s.ball.IsFree() && !(s.ball.vel == Cell{0, 0}))
        return BallStepEstimate(s, episode, detailedRisk, restartAsPosition, 2, raceFreeBall);
    const int attacking = episode.status == GoalEpisodeStatus::OurAttack
                              ? episode.team : Opponent(episode.team);
    const int defending = Opponent(attacking);
    const double sign = attacking == episode.team ? 1.0 : -1.0;
    if (s.halfTurns == kHalfTurnsPerHalf - 1 || s.halfTurns == kHalfTurnsPerMatch - 1) {
        if (s.ballPlayed) return 0;
        if (!s.ball.IsFree() && TeamOf(s.ball.holder) == attacking && s.teamToMove == attacking)
            return sign * HolderShotChanceThisTurn(s);
    }
    if (raceFreeBall && detailedRisk && s.ball.IsFree()) {
        double raced = 0;
        if (RaceFreeBallEstimate(s, episode, restartAsPosition, &raced)) return raced;
    }
    if (s.ball.IsFree() || TeamOf(s.ball.holder) != attacking) {
        int ownDistance = kGridHeight, otherDistance = kGridHeight;
        for (int player = 0; player < kNumPlayers; ++player) {
            const int distance = ChebyshevDistance(s.players[player].pos, s.ball.pos);
            if (TeamOf(player) == attacking) ownDistance = std::min(ownDistance, distance);
            else otherDistance = std::min(otherDistance, distance);
        }
        const double reach = double(otherDistance) / (ownDistance + otherDistance + 1);
        return sign * reach * OpenShotProbability(s.ball.pos, attacking);
    }

    if (tReplyAware && detailedRisk)
        return std::max(-1.0, std::min(1.0, sign * ReplyAwareAttack(
            s, attacking, episode.status == GoalEpisodeStatus::OurAttack)));
    const bool canShootNow = s.teamToMove == attacking && !s.ballPlayed;
    const double shotNow = canShootNow ? HolderShotChanceThisTurn(s) : 0;
    const double continuation = canShootNow ? (detailedRisk ? PartnerOptionsThreat(s)
                                                           : PartnerThreatNextTurn(s))
                                            : detailedRisk ? AttackThreatWhenNextToMove(s, true)
                                                           : ShotProbability(s, s.ball.holder);
    const ReplyRisk risk = detailedRisk
                               ? OpponentTackleRisk(s, defending)
                               : ReplyRisk{QuickKeepChance(s, defending), 0};
    double value = shotNow + (1 - shotNow) * risk.keep * continuation;
    if (episode.status == GoalEpisodeStatus::OurAttack)
        value -= (1 - shotNow) * risk.counter;
    return std::max(-1.0, std::min(1.0, sign * value));
}

// Pass pruning by the reach map (07.11.2026, the user's rule: a pass the
// opponent can take is almost never worth considering - moving the passer
// and the receiver gives a safe delivery). One map of the state serves all
// its passes; a pass to an empty cell whose untouched ball the map meets with
// the opponent's area first is removed. The map is a possibility, not a
// guarantee: unproven. On the games (ssbot reach-prune-check) no pass that the
// ball race guarantees to the passing team was removed.
// The best open shot of the field (the cells in front of the goal centre),
// from the shot rule itself.
const double kBestOpenShot = [] {
    double best = 0;
    for (int x = 0; x < kFieldWidth; ++x)
        for (int y = kFirstRow; y <= kLastRow; ++y) best = std::max(best, OpenShotProbability({x, y}, kWhite));
    return best;
}();

long PruneReachPasses(const GameState& s, std::vector<Action>& actions, long* maps) {
    if (s.phase != Phase::Play || s.ball.IsFree() || TeamOf(s.ball.holder) != s.teamToMove ||
        s.ballPlayed || s.keeperRestartTeam >= 0)
        return 0;
    const bool anyPass = std::any_of(actions.begin(), actions.end(), [&](const Action& a) {
        return a.type == ActionType::Pass && PlayerAt(s, a.target) == kNoPlayer;
    });
    if (!anyPass) return 0;
    const ReachFront front = ComputeReachFront(s);
    if (maps) ++*maps;
    if (front.stop == ReachFrontStop::InvalidStart) return 0;
    const int opponent = Opponent(s.teamToMove);
    const auto before = actions.size();
    actions.erase(std::remove_if(actions.begin(), actions.end(), [&](const Action& a) {
        return a.type == ActionType::Pass && ReachPassTaker(s, front, a) == opponent;
    }), actions.end());
    return static_cast<long>(before - actions.size());
}

// Defence forced decisions (09.11.2026, the user's rule): a tackle or an
// interception the defending side can make this turn always enters its
// search, whatever the static says of it. The defending side is the team to
// move when the episode's attack is the other team's.
bool DefendingTurn(const GameState& s) {
    return s.phase == Phase::Play && s.teamToMove != s.ball.attackTeam;
}

// Cells where a player of the team to move takes or meets the free ball this
// turn: the ball's cell now and the cells its step at this turn's end crosses.
std::vector<Cell> InterceptCells(const GameState& s) {
    std::vector<Cell> cells;
    if (s.phase != Phase::Play || !s.ball.IsFree()) return cells;
    for (const BallWindow& w : BallPathWindows(s))
        if (w.arrival <= 1) cells.push_back(w.cell);
    return cells;
}

// Player `p` can tackle (or poke) now, or stands on an interception cell.
bool ForcedPlayer(const GameState& s, int p, const std::vector<Cell>& cells) {
    if (!s.players[p].attacked && GetAttackKind(s, p) != AttackKind::None) return true;
    return std::find(cells.begin(), cells.end(), s.players[p].pos) != cells.end();
}

bool ForcedState(const GameState& s, const std::vector<Cell>& cells) {
    const int first = FirstPlayerOf(s.teamToMove);
    for (int p = first; p < first + kPlayersPerTeam; ++p)
        if (ForcedPlayer(s, p, cells)) return true;
    return false;
}

// Kinds of a team's decision for the diversity of the beam: the holder's run,
// another player's move, a pass to a partner's feet, a pass into space, a shot,
// an attack and the end of the turn (keeping the ball where it is).
int DecisionKind(const GameState& s, const Action& a, bool pickupKind = false) {
    switch (a.type) {
        case ActionType::Move:
            // A move onto the free ball (taking it now) is its own kind.
            if (pickupKind && s.phase == Phase::Play && s.ball.IsFree() && a.target == s.ball.pos) return 8;
            return a.player == s.ball.holder ? 0 : 1;
        case ActionType::Pass: {
            const int receiver = PlayerAt(s, a.target);
            return receiver != kNoPlayer && TeamOf(receiver) == s.teamToMove ? 2 : 3;
        }
        case ActionType::Shot: return 4;
        case ActionType::Attack: return 5;
        case ActionType::EndTurn: return 6;
        default: return 7;
    }
}

// Categories of a decision of the team with the ball, matching the channels
// of the geometric estimate (bot_design.md 4.3, 29.10.2026): keep the ball
// with the holder, shoot, or pass to one partner - to his feet or to a cell he
// can still reach this turn. A pass nobody can reach is its own category.
int PartnerCategory(const GameState& s, const Action& a) {
    switch (a.type) {
        case ActionType::EndTurn: return 0;
        case ActionType::Shot: return 1;
        case ActionType::Pass: {
            const int team = s.teamToMove;
            const int first = FirstPlayerOf(team);
            int best = kNoPlayer, bestDistance = 1 << 20;
            for (int k = first; k < first + kPlayersPerTeam; ++k) {
                if (k == s.ball.holder) continue;
                const Cell p = s.players[k].pos;
                if (p == a.target) return 2 + (k - first);
                if (s.players[k].moved) continue;
                for (Cell c : MoveDestinations(s, k))
                    if (c == a.target) {
                        const int distance = ChebyshevDistance(p, a.target);
                        if (distance < bestDistance) { bestDistance = distance; best = k; }
                        break;
                    }
            }
            return best == kNoPlayer ? 6 : 2 + (best - first);
        }
        default: return 7;
    }
}

// Beam with every kind represented: the best of each kind by the cheap
// estimate joins the usual shortlist of the 2B best, all of them get the
// detailed estimate, then the best of each kind is kept and the remaining
// places go to the best overall. Returns kept positions, best first, and their
// detailed values. When the best of every kind is among the B best anyway, the
// beam is the plain one.
template <class Detailed>
std::vector<std::size_t> DiverseBeam(const std::vector<double>& cheap, const std::vector<int>& kind,
                                     Detailed detailed, bool maximize, std::size_t width,
                                     std::vector<double>* kept,
                                     const std::vector<std::size_t>* group = nullptr,
                                     const std::function<void(const std::vector<std::size_t>&)>* prefetch = nullptr) {
    const std::size_t n = cheap.size();
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return maximize ? cheap[a] > cheap[b] : cheap[a] < cheap[b];
    });
    std::vector<std::size_t> shortlist(order.begin(), order.begin() + std::min(n, 2 * width));
    std::vector<int> seen;
    for (std::size_t i : shortlist) seen.push_back(kind[i]);
    for (std::size_t i : order)
        if (std::find(seen.begin(), seen.end(), kind[i]) == seen.end()) {
            seen.push_back(kind[i]);
            shortlist.push_back(i);
        }
    // Each group (the movement state a decision starts from) also brings its
    // best decision, so one state's many passes do not fill the whole beam.
    if (group) {
        std::vector<std::size_t> groups;
        for (std::size_t i : shortlist) groups.push_back((*group)[i]);
        for (std::size_t i : order)
            if (std::find(groups.begin(), groups.end(), (*group)[i]) == groups.end()) {
                groups.push_back((*group)[i]);
                if (std::find(shortlist.begin(), shortlist.end(), i) == shortlist.end()) shortlist.push_back(i);
            }
    }
    std::vector<double> value(n, 0);
    if (prefetch) (*prefetch)(shortlist);   // the detailed values of the shortlist, computed together
    for (std::size_t i : shortlist) value[i] = detailed(i);
    std::stable_sort(shortlist.begin(), shortlist.end(), [&](std::size_t a, std::size_t b) {
        return maximize ? value[a] > value[b] : value[a] < value[b];
    });
    std::vector<std::size_t> chosen;
    std::vector<int> represented;
    for (std::size_t i : shortlist) {
        if (chosen.size() == width) break;
        if (std::find(represented.begin(), represented.end(), kind[i]) != represented.end()) continue;
        represented.push_back(kind[i]);
        chosen.push_back(i);
    }
    if (group) {
        std::vector<std::size_t> covered;
        for (std::size_t i : chosen) covered.push_back((*group)[i]);
        for (std::size_t i : shortlist) {
            if (chosen.size() == width) break;
            if (std::find(covered.begin(), covered.end(), (*group)[i]) != covered.end()) continue;
            covered.push_back((*group)[i]);
            chosen.push_back(i);
        }
    }
    for (std::size_t i : shortlist) {
        if (chosen.size() == width) break;
        if (std::find(chosen.begin(), chosen.end(), i) == chosen.end()) chosen.push_back(i);
    }
    std::stable_sort(chosen.begin(), chosen.end(), [&](std::size_t a, std::size_t b) {
        if (value[a] != value[b]) return maximize ? value[a] > value[b] : value[a] < value[b];
        return std::find(shortlist.begin(), shortlist.end(), a) <
               std::find(shortlist.begin(), shortlist.end(), b);
    });
    kept->clear();
    for (std::size_t i : chosen) kept->push_back(value[i]);
    return chosen;
}

}  // namespace

GoalStaticSummary EvaluateGoalStatic(const GameState& s) {
    GoalStaticSummary summary;
    if (s.phase != Phase::Play || s.ball.IsFree()) return summary;
    const int team = TeamOf(s.ball.holder);
    summary.attackThreat = AttackThreatWhenNextToMove(s, true);
    if (s.teamToMove == team && !s.ballPlayed) {
        summary.directPassThreat = PartnerThreatNextTurn(s);
        summary.leadPassThreat = LeadPassThreat(s);
    }
    const ReplyRisk risk = OpponentTackleRisk(s, Opponent(team));
    summary.keepChance = risk.keep;
    summary.counterThreat = risk.counter;
    return summary;
}

double HolderShotChanceThisTurn(const GameState& s) {
    if (s.phase != Phase::Play || s.ball.IsFree() || s.ballPlayed ||
        TeamOf(s.ball.holder) != s.teamToMove) return 0;

    const int holder = s.ball.holder;
    double best = ShotProbability(s, holder);
    if (s.players[holder].moved) return best;

    // MoveDestinations applies the actual inertia and blocking rules. For each
    // legal destination, only the holder's position changes the shot formula.
    GameState after = s;
    after.players[holder].moved = true;
    for (Cell target : TeamDestinations(s, holder)) {
        if (target == s.players[holder].pos) continue;
        after.players[holder].pos = target;
        after.ball.pos = target;
        best = std::max(best, ShotProbability(after, holder));
    }
    return best;
}

struct GoalDfsSearch::Impl {
    struct Entry {
        GameState state;
        uint64_t key = 0;
        GoalEpisode episode;
        GoalValueBounds bounds;
        bool used = false;
    };

    // Transposition entry of the selective tree: the value of a node is a
    // function of (state, episode, remaining turn depth) only. Values are
    // reused at the same depth, so the result equals the search without it.
    struct SelectiveEntry {
        GameState state;
        uint64_t key = 0;
        GoalEpisode episode;
        GoalValueBounds bounds;
        int depth = -1;
        bool used = false;
    };

    explicit Impl(const GoalDfsOptions& o) : options(o) {}

    struct MemoryScope {
        Impl& owner;
        std::size_t bytes;
        bool admitted;
        MemoryScope(Impl& o, std::size_t n) : owner(o), bytes(n), admitted(o.Reserve(n)) {}
        ~MemoryScope() { if (admitted) owner.liveBytes -= bytes; }
    };

    GoalDfsOptions options;
    GoalDfsStats stats;
    struct TreeProfile {
        uint64_t nodeNs = 0, movementNs = 0, categoryNs = 0;
        uint64_t edgeNs = 0, elementaryNs = 0, nodeForNs = 0, estimateNs = 0;
        uint64_t estimateCalls = 0, nodeCalls = 0, edgeCalls = 0;
        uint64_t estimateStageNs[8]{};
        uint64_t estimateStageCalls[8]{};
        uint64_t actionScoreNs[6]{};
        uint64_t actionScoreCalls[6]{};
        uint64_t actionScoreOutcomes[6]{};
    };
    // Cache of geometric static values (selectiveStaticCacheBits). The key is
    // every field of the state the estimate can read, the episode and the
    // realized-only flag, compared in full: a hit returns exactly the value the
    // estimate would compute. Direct-mapped, a new value replaces the old one.
    struct StaticKey {
        uint64_t words[11];
        bool operator==(const StaticKey& o) const {
            for (int i = 0; i < 11; ++i)
                if (words[i] != o.words[i]) return false;
            return true;
        }
    };
    struct StaticCacheEntry {
        StaticKey key;
        double value = 0;
        bool used = false;
    };
    mutable std::vector<StaticCacheEntry> staticCache;
    // The static cache shared by the threads of a parallel search (a seqlock per
    // entry over atomic words: a torn read is a miss, a busy entry is not written).
    struct SharedStaticEntry {
        std::atomic<uint32_t> seq{0};   // 0: empty, odd: being written, even: valid
        std::atomic<uint64_t> words[11];
        std::atomic<uint64_t> value{0};
    };
    struct SharedStaticCache {
        std::unique_ptr<SharedStaticEntry[]> entries;
        std::size_t mask = 0;
        explicit SharedStaticCache(int bits) : entries(new SharedStaticEntry[std::size_t(1) << bits]),
                                               mask((std::size_t(1) << bits) - 1) {
            for (std::size_t i = 0; i <= mask; ++i)
                for (auto& w : entries[i].words) w.store(0, std::memory_order_relaxed);
        }
        bool Find(const StaticKey& key, std::size_t hash, double* value) const {
            const SharedStaticEntry& e = entries[hash & mask];
            const uint32_t s1 = e.seq.load(std::memory_order_acquire);
            if (s1 == 0 || (s1 & 1u)) return false;
            for (int i = 0; i < 11; ++i)
                if (e.words[i].load(std::memory_order_relaxed) != key.words[i]) return false;
            const uint64_t bits = e.value.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (e.seq.load(std::memory_order_relaxed) != s1) return false;
            std::memcpy(value, &bits, sizeof bits);
            return true;
        }
        void Store(const StaticKey& key, std::size_t hash, double value) {
            SharedStaticEntry& e = entries[hash & mask];
            uint32_t s0 = e.seq.load(std::memory_order_relaxed);
            if ((s0 & 1u) || !e.seq.compare_exchange_strong(s0, s0 + 1, std::memory_order_acquire)) return;
            for (int i = 0; i < 11; ++i) e.words[i].store(key.words[i], std::memory_order_relaxed);
            uint64_t bits;
            std::memcpy(&bits, &value, sizeof bits);
            e.value.store(bits, std::memory_order_relaxed);
            e.seq.store(s0 + 2, std::memory_order_release);
        }
    };
    std::shared_ptr<SharedStaticCache> sharedStatic;   // set while a parallel pool runs
    std::shared_ptr<SharedStaticCache> sharedRace;     // the race cache, shared the same way (NaN: undecided)
    std::shared_ptr<SharedStaticCache> sharedLeaf;     // whole leaf values (ballRace) while a pool runs
    struct RaceCacheEntry {
        StaticKey key;
        double value = 0;
        bool decided = false;   // RaceEstimate's answer; false: left to the static
        bool used = false;
    };
    mutable std::vector<RaceCacheEntry> raceCache;   // the race of free-ball estimates, as the static cache
    mutable long raceCacheHits = 0, raceCacheCalls = 0;
    mutable long staticCalls = 0, staticHits = 0;

    static StaticKey MakeStaticKey(const GameState& s, GoalEpisode e, bool realized) {
        uint8_t b[88] = {};
        int n = 0;
        const auto cell = [&](Cell c) {
            b[n++] = static_cast<uint8_t>(c.x + 64);
            b[n++] = static_cast<uint8_t>(c.y + 64);
        };
        for (const PlayerState& p : s.players) {
            cell(p.pos);
            cell(p.turnStart);
            cell(p.prevTurnStart);
            cell(p.attackDir);
            b[n++] = static_cast<uint8_t>(p.moved | p.attacked << 1);
        }
        const BallState& ball = s.ball;
        cell(ball.pos);
        cell(ball.vel);   // ball.dec is read only along a non-zero velocity, where it is -sign(vel)
        b[n++] = static_cast<uint8_t>(ball.holder + 1);
        b[n++] = static_cast<uint8_t>(ball.stepPhase | ball.lastTouchTeam << 1 | ball.lastTouchWasPass << 2 |
                                      ball.attackTeam << 3);
        b[n++] = static_cast<uint8_t>(int(s.phase) | s.lineupStep << 2 | s.teamToMove << 3 | s.ballPlayed << 4);
        b[n++] = static_cast<uint8_t>(s.halfTurns & 0xff);
        b[n++] = static_cast<uint8_t>(s.halfTurns >> 8);
        b[n++] = static_cast<uint8_t>(s.score[0]);
        b[n++] = static_cast<uint8_t>(s.score[1]);
        b[n++] = static_cast<uint8_t>(s.sealHistory[0] | s.sealHistory[1] << 4);
        b[n++] = static_cast<uint8_t>((s.keeperRestartTeam + 1) | s.keeperBackPass << 2);
        b[n++] = static_cast<uint8_t>(e.team | int(e.status) << 1 | realized << 4);
        StaticKey key;
        std::memcpy(key.words, b, sizeof key.words);
        return key;
    }
    static uint64_t StaticKeyHash(const StaticKey& k) {
        uint64_t h = 0x9e3779b97f4a7c15ULL;
        for (uint64_t w : k.words) {
            h ^= w + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            h *= 0xbf58476d1ce4e5b9ULL;
            h ^= h >> 31;
        }
        return h;
    }

    // Leaves with a free ball by the race (selectiveBallRace): taken, restart,
    // contested (valued by the static), contested and searched further.
    mutable long raceLeaves[5] = {0, 0, 0, 0, 0};  // taken, restart, contested, extended, late takes
    int raceExtensions = 0;   // extensions on the current line
    int episodeExtensions = 0;  // episode extensions on the current line (selectiveEpisodeExtension)
    long episodeExtensionCount = 0;
    mutable long breakawayLeaves = 0, breakawayRaised = 0;  // leaves given the bound; those raised by it
    mutable BreakawayAhead breakawayAhead;   // the state being estimated is built ahead of time
    mutable long twoVsOneLeaves = 0, twoVsOneRaised = 0;
    // ---- Parallel estimates (02.11.2026) -------------------------------------
    // The candidate ranking of a node (most of the beam's time) is a batch of
    // independent estimates. With options.threads > 1 a batch is shared by a
    // pool: the main search and threads - 1 helpers, each with its own caches.
    // A job goes to the slot given by its state's hash, so the same state is
    // always estimated by the same cache: values (and so the search) do not
    // depend on the number of threads.
    struct EstimateJob {
        const GameState* s = nullptr;
        GoalEpisode episode{};
        Action action{};
        bool isAction = false;
        bool detailedRisk = false, race = false, partialTurn = false, ballRace = false;
        double value = 0;
    };

    double RunJob(const EstimateJob& j) {
        return j.isAction ? ActionEstimate(*j.s, j.episode, j.action, j.detailedRisk, j.race, j.ballRace)
                          : Estimate(*j.s, j.episode, j.detailedRisk, j.race, j.partialTurn, j.ballRace);
    }

    static EstimateJob StateJob(const GameState& s, GoalEpisode e, bool detailedRisk = true, bool race = false,
                                bool partialTurn = false, bool ballRace = false) {
        EstimateJob j;
        j.s = &s;
        j.episode = e;
        j.detailedRisk = detailedRisk;
        j.race = race;
        j.partialTurn = partialTurn;
        j.ballRace = ballRace;
        return j;
    }
    static EstimateJob ActionJob(const GameState& s, GoalEpisode e, const Action& a, bool detailedRisk = false,
                                 bool race = false, bool ballRace = false) {
        EstimateJob j;
        j.s = &s;
        j.episode = e;
        j.action = a;
        j.isAction = true;
        j.detailedRisk = detailedRisk;
        j.race = race;
        j.ballRace = ballRace;
        return j;
    }

    struct EstimatePool {
        int size = 1;
        std::vector<std::unique_ptr<Impl>> helpers;
        std::vector<std::thread> threads;
        std::atomic<uint64_t> generation{0};
        std::atomic<int> pending{0};
        std::atomic<bool> quit{false};
        std::vector<EstimateJob>* jobs = nullptr;
        std::vector<std::vector<std::size_t>> buckets;
        long batches = 0, parallelJobs = 0;

        EstimatePool(const GoalDfsOptions& o, int n) : size(n), buckets(n) {
            for (int i = 1; i < n; ++i) helpers.emplace_back(new Impl(o));
            for (int i = 1; i < n; ++i) threads.emplace_back([this, i] { Work(i); });
        }
        ~EstimatePool() {
            quit.store(true);
            for (std::thread& t : threads) t.join();
        }
        std::atomic<std::size_t> nextJob{0};
        void Drain(Impl& h) {
            const std::size_t n = jobs->size();
            for (;;) {
                const std::size_t i = nextJob.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) return;
                (*jobs)[i].value = h.RunJob((*jobs)[i]);
            }
        }
        void Work(int slot) {
            uint64_t seen = 0;
            Impl& h = *helpers[slot - 1];
            while (!quit.load(std::memory_order_relaxed)) {
                const uint64_t g = generation.load(std::memory_order_acquire);
                if (g == seen) {
                    std::this_thread::yield();
                    continue;
                }
                seen = g;
                Drain(h);
                pending.fetch_sub(1, std::memory_order_release);
            }
        }
    };
    std::unique_ptr<EstimatePool> pool;
    static constexpr std::size_t kMinParallelBatch = 8;

    static uint64_t JobHash(const EstimateJob& j) {
        uint64_t h = PositionHash(*j.s) ^ (static_cast<uint64_t>(j.episode.team + 1) << 3) ^
                     (static_cast<uint64_t>(j.episode.status) << 7);
        if (j.isAction)
            h ^= (static_cast<uint64_t>(j.action.type) + 1) * 0x9E3779B97F4A7C15ull ^
                 (static_cast<uint64_t>(j.action.player + 2) << 17) ^
                 (static_cast<uint64_t>(j.action.target.x + 1) << 25) ^ (static_cast<uint64_t>(j.action.target.y + 1) << 33);
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 32;
        return h;
    }

    double batchSeconds = 0, smallBatchSeconds = 0;   // options.profileRoot: time in batches
    long batchJobs = 0, batchCount = 0;
    void RunJobs(std::vector<EstimateJob>& jobs) {
        const bool profile = options.profileRoot;
        const auto t0 = profile ? Clock::now() : Clock::time_point();
        RunJobsInner(jobs);
        if (profile) {
            const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
            if (jobs.size() >= kMinParallelBatch) {
                batchSeconds += sec;
                batchJobs += static_cast<long>(jobs.size());
                ++batchCount;
            } else {
                smallBatchSeconds += sec;
            }
        }
    }
    void RunJobsInner(std::vector<EstimateJob>& jobs) {
        if (!pool || jobs.size() < kMinParallelBatch) {
            for (EstimateJob& j : jobs) j.value = RunJob(j);
            return;
        }
        EstimatePool& p = *pool;
        p.jobs = &jobs;
        p.nextJob.store(0, std::memory_order_relaxed);
        p.pending.store(p.size - 1, std::memory_order_relaxed);
        p.generation.fetch_add(1, std::memory_order_release);
        p.Drain(*this);
        while (p.pending.load(std::memory_order_acquire) > 0) std::this_thread::yield();
        ++p.batches;
        p.parallelJobs += static_cast<long>(jobs.size());
    }

    // Root split of the selective beam (02.11.2026): helpers kept between
    // searches, so their tables and caches carry over like the main one's.
    std::vector<std::unique_ptr<Impl>> selectiveHelpers;
    std::string rootProfile;   // per-iteration root decision times (options.profileRoot)
    mutable long tableLeaves = 0, tableRaised = 0, tableIncomplete = 0;   // episode table leaves
    int refining = 0;   // inside the refined ranking's short searches: no refinement there (06.11)
    mutable std::unordered_map<uint64_t, double> tableCache;              // < 0: incomplete  // 2 v 1 leaves; those whose bound beat the 1 v 0 one
    // Transposition entries searched under an extension see a different
    // horizon: they are kept apart from the plain ones.
    // The beam width of the current node: narrower inside an episode extension.
    int BeamWidth() const {
        return episodeExtensions > 0 && options.selectiveExtensionWidth > 0
                   ? std::min(options.selectiveWidth, options.selectiveExtensionWidth)
                   : options.selectiveWidth;
    }
    // The movement beam per ply: wider than the decision beam when asked (06.11).
    int MovementWidth() const {
        return options.selectiveMovementWidth > 0 && episodeExtensions == 0
                   ? std::max(options.selectiveMovementWidth, BeamWidth()) : BeamWidth();
    }
    int TableDepth(int depth) const { return depth + 1000 * (episodeExtensions + raceExtensions); }
    mutable TreeProfile treeProfile;
    mutable int profileStage = 0;  // other, node, movement, category, edge, elementary, ranking, actions
    struct ProfileStageScope {
        Impl& owner;
        int previous;
        explicit ProfileStageScope(Impl& search, int stage) : owner(search), previous(search.profileStage) {
            if (owner.options.profileTree) owner.profileStage = stage;
        }
        ~ProfileStageScope() { if (owner.options.profileTree) owner.profileStage = previous; }
    };
    Clock::time_point start, deadline;
    double timeLimit = 0;
    int rootHalfTurns = 0;
    long positions = 0, expansions = 0;
    int maxDepth = 0;
    bool stopped = false, memoryLimited = false;
    std::size_t liveBytes = 0, peakBytes = 0;
    std::size_t memoryBudgetBytes = 0;
    std::vector<Entry> cache;
    std::vector<SelectiveEntry> selectiveCache;
    // The attack episode of the node being expanded (7.67): its outsiders do not
    // branch, and the ball reaching one of them ends the line (a leaf).
    const AttackEpisode* nodeEpisode = nullptr;
    unsigned exitMask = 0;
    std::size_t treeBytes = 0;          // the best-first tree (7.68), estimated
    std::atomic<long>* sharedExpansions = nullptr;
    std::atomic<bool>* sharedStop = nullptr;

    bool Reserve(std::size_t bytes) {
        if (options.maxMemoryBytes &&
            bytes > memoryBudgetBytes - std::min(liveBytes, memoryBudgetBytes)) {
            memoryLimited = true;
            stopped = true;
            if (sharedStop) sharedStop->store(true, std::memory_order_relaxed);
            return false;
        }
        liveBytes += bytes;
        peakBytes = std::max(peakBytes, liveBytes);
        return true;
    }

    bool Budget() {
        if (stopped || (sharedStop && sharedStop->load(std::memory_order_relaxed))) return false;
        if (!sharedExpansions && options.expansions > 0 && expansions >= options.expansions) stopped = true;
        if (timeLimit > 0 && Clock::now() >= deadline) stopped = true;
        if (stopped && sharedStop) sharedStop->store(true, std::memory_order_relaxed);
        return !stopped;
    }

    bool TakeExpansion() {
        if (!Budget()) return false;
        if (sharedExpansions && options.expansions > 0 &&
            sharedExpansions->fetch_add(1, std::memory_order_relaxed) >= options.expansions) {
            sharedExpansions->fetch_sub(1, std::memory_order_relaxed);
            stopped = true;
            sharedStop->store(true, std::memory_order_relaxed);
            return false;
        }
        ++expansions;
        return true;
    }

    uint64_t Key(const GameState& s, GoalEpisode e) const {
        return PositionHash(s) ^ (static_cast<uint64_t>(e.team + 1) << 61) ^
               (static_cast<uint64_t>(e.status) << 56);
    }

    Entry* Slot(uint64_t key) {
        if (cache.empty()) return nullptr;
        return &cache[key % cache.size()];
    }

    GoalValueBounds Cached(const GameState& s, GoalEpisode e, GoalValueBounds base) {
        const uint64_t key = Key(s, e);
        Entry* slot = Slot(key);
        if (slot && slot->used && slot->key == key && slot->episode.team == e.team &&
            slot->episode.status == e.status && SameSearchState(slot->state, s)) {
            ++stats.cacheHits;
            return Intersect(base, slot->bounds);
        }
        return base;
    }

    void Store(const GameState& s, GoalEpisode e, GoalValueBounds value) {
        const uint64_t key = Key(s, e);
        Entry* slot = Slot(key);
        if (!slot) return;
        if (slot->used && slot->key == key && slot->episode.team == e.team &&
            slot->episode.status == e.status && SameSearchState(slot->state, s)) {
            slot->bounds = Intersect(slot->bounds, value);
            return;
        }
        if (slot->used) ++stats.cacheReplacements;
        slot->state = s;
        slot->key = key;
        slot->episode = e;
        slot->bounds = value;
        slot->used = true;
    }

    SelectiveEntry* SelectiveProbe(const GameState& s, GoalEpisode e) {
        if (selectiveCache.empty()) return nullptr;
        const uint64_t key = Key(s, e);
        SelectiveEntry& slot = selectiveCache[key % selectiveCache.size()];
        if (slot.used && slot.key == key && slot.episode.team == e.team &&
            slot.episode.status == e.status && SameSearchState(slot.state, s)) {
            ++stats.cacheHits;
            return &slot;
        }
        return nullptr;
    }

    void SelectiveStore(const GameState& s, GoalEpisode e, int depth, GoalValueBounds bounds) {
        if (selectiveCache.empty()) return;
        const uint64_t key = Key(s, e);
        SelectiveEntry& slot = selectiveCache[key % selectiveCache.size()];
        const bool same = slot.used && slot.key == key && slot.episode.team == e.team &&
                          slot.episode.status == e.status && SameSearchState(slot.state, s);
        // Keep the deeper result of the same node; another node always replaces.
        if (same && slot.depth > depth) return;
        if (same && slot.depth == depth) {
            // Both are valid bounds of the same value.
            slot.bounds = Intersect(slot.bounds, bounds);
        } else {
            if (!same && slot.used) ++stats.cacheReplacements;
            slot.state = s;
            slot.key = key;
            slot.episode = e;
            slot.bounds = bounds;
            slot.depth = depth;
            slot.used = true;
        }
    }

    // GoalStateBounds with the continuation (7.62): an episode without a goal
    // is worth the possession that follows, so a counterattack may end above 0.
    GoalValueBounds Bounds(const GameState& s, GoalEpisode e) const {
        if (options.selectivePossessionValue > 0) {
            // The scale "who scores next": an episode without a goal is worth
            // the possession that follows; a counterattack may end above 0.
            if (e.status == GoalEpisodeStatus::NoGoal) {
                const double c = PossessionValue(s, e.team, options.selectivePossessionValue);
                return {c, c};
            }
            if (e.status == GoalEpisodeStatus::TheirCounterattack && ContinuationAllowed(s)) return {-1, 1};
            return GoalStateBounds(s, e);
        }
        if (!options.selectiveContinuation) return GoalStateBounds(s, e);
        if (e.status == GoalEpisodeStatus::NoGoal) {
            const double c = ContinuationValue(s, e.team);
            return {c, c};
        }
        if (e.status == GoalEpisodeStatus::TheirCounterattack && ContinuationAllowed(s)) return {-1, 1};
        return GoalStateBounds(s, e);
    }
    GoalValueBounds Envelope(const GameState& s, GoalEpisode e) const {
        if ((options.selectiveContinuation || options.selectivePossessionValue > 0) &&
            e.status == GoalEpisodeStatus::TheirCounterattack &&
            ContinuationAllowed(s))
            return {-1, 1};
        return InitialGoalBounds(e.status);
    }

    // `exact`: the expensive parts of the estimate — the race for a free ball
    // and the attack against the reply — only where few states are valued: the
    // leaves and the short lists, not every movement state.
    // `ballRace`: a leaf of the search (not a candidate being ranked) - with
    // selectiveBallRace a free ball there is valued by who reaches it first.
    // The leaf value (ballRace) cached whole while a parallel pool runs, so a
    // leaf estimated ahead by a helper is not estimated again (only states
    // valued as they stand: no ahead-of-time breakaway context).
    double Estimate(const GameState& s, GoalEpisode episode, bool detailedRisk = true,
                    bool race = false, bool partialTurn = false, bool ballRace = false) const {
        if (sharedLeaf && ballRace && race && !partialTurn && breakawayAhead.opponentHeadStart == 0 &&
            !breakawayAhead.holderMoved && breakawayAhead.exact == 0 && breakawayAhead.origin == nullptr) {
            const StaticKey key = MakeStaticKey(s, episode, detailedRisk);
            const std::size_t hash = StaticKeyHash(key);
            double cached;
            if (sharedLeaf->Find(key, hash, &cached)) return cached;
            const double value = EstimateRaw(s, episode, detailedRisk, race, partialTurn, ballRace);
            sharedLeaf->Store(key, hash, value);
            return value;
        }
        return EstimateRaw(s, episode, detailedRisk, race, partialTurn, ballRace);
    }

    double EstimateRaw(const GameState& s, GoalEpisode episode, bool detailedRisk,
                       bool race, bool partialTurn, bool ballRace) const {
        ScopedTreeTimer timer(options.profileTree ? &treeProfile.estimateNs : nullptr,
                              options.profileTree ? &treeProfile.estimateStageNs[profileStage] : nullptr);
        if (options.profileTree) {
            ++treeProfile.estimateCalls;
            ++treeProfile.estimateStageCalls[profileStage];
        }
        if (options.selectiveBallRace && ballRace && s.phase == Phase::Play && s.ball.IsFree() &&
            s.keeperRestartTeam < 0) {
            // Cached as the static: the same state, episode and detail give the same answer.
            const int bits = options.selectiveStaticCacheBits;
            if (bits <= 0 || bits > 26) {
                double value;
                if (RaceEstimate(s, episode, detailedRisk, &value)) return value;
            } else {
                if (sharedRace) {
                    const StaticKey key = MakeStaticKey(s, episode, detailedRisk);
                    const std::size_t hash = StaticKeyHash(key);
                    ++raceCacheCalls;
                    double cached;
                    if (sharedRace->Find(key, hash, &cached)) {
                        ++raceCacheHits;
                    } else {
                        double value = 0;
                        cached = RaceEstimate(s, episode, detailedRisk, &value)
                                     ? value : std::numeric_limits<double>::quiet_NaN();
                        sharedRace->Store(key, hash, cached);
                    }
                    if (!std::isnan(cached)) return cached;
                    goto raceDone;
                }
                if (raceCache.empty()) raceCache.resize(std::size_t(1) << std::max(1, bits - 3));
                const StaticKey key = MakeStaticKey(s, episode, detailedRisk);
                RaceCacheEntry& entry = raceCache[StaticKeyHash(key) & (raceCache.size() - 1)];
                ++raceCacheCalls;
                if (entry.used && entry.key == key) {
                    ++raceCacheHits;
                } else {
                    double value = 0;
                    entry.decided = RaceEstimate(s, episode, detailedRisk, &value);
                    entry.value = value;
                    entry.key = key;
                    entry.used = true;
                }
                if (entry.decided) return entry.value;
            }
        }
        raceDone:
        // A leaf in a 1 v 0 attack episode: the holder's proven shot (a lower
        // bound whatever the defence does) is a floor of the attack's value.
        if (options.selectiveBreakawayBound && ballRace && s.phase == Phase::Play && !s.ball.IsFree() &&
            !Terminal(episode.status)) {
            const double value = Estimate(s, episode, detailedRisk, race, partialTurn, false);
            const int attackNow = episode.status == GoalEpisodeStatus::OurAttack ? episode.team
                                                                                 : Opponent(episode.team);
            if (TeamOf(s.ball.holder) != attackNow) return value;
            const bool needEpisode = !options.selectiveBreakawayAnyEpisode || options.selectiveTwoVsOneBound ||
                                     options.selectiveEpisodeTable;
            const AttackEpisode attack = needEpisode ? AnalyzeAttackEpisode(s, false, options.selectiveEpisodeBallRace) : AttackEpisode();
            if (!options.selectiveBreakawayAnyEpisode && attack.kind != AttackEpisodeKind::OneVsZero) return value;
            ++breakawayLeaves;
            // Bounds below are goal probabilities; with the possession value a
            // proven shot b is worth b - (1 - b) W (the ball is given away).
            const double w = ContinuationAllowed(s) ? options.selectivePossessionValue : 0;
            const auto toValue = [w](double b) { return b * (1 + w) - w; };
            const double floor = w > 0 ? std::max(0.0, ((attackNow == episode.team ? value : -value) + w) / (1 + w))
                                       : (attackNow == episode.team ? value : -value);
            double bound = BreakawayShotLowerBound(s, 12, floor, breakawayAhead);
            // A 2 v 1 in a state the game reaches (not one built ahead of time).
            if (options.selectiveTwoVsOneBound && attack.kind == AttackEpisodeKind::TwoVsOne &&
                breakawayAhead.opponentHeadStart == 0 && !breakawayAhead.holderMoved) {
                int partner = kNoPlayer, defender = kNoPlayer;
                for (int k = 0; k < kNumPlayers; ++k) {
                    if ((attack.attackers >> k & 1u) && k != s.ball.holder) partner = k;
                    if (attack.defenders >> k & 1u) defender = k;
                }
                if (partner != kNoPlayer && defender != kNoPlayer) {
                    ++twoVsOneLeaves;
                    const double two = TwoVsOneLowerBound(s, partner, defender, 12, std::max(floor, bound));
                    if (two > bound) {
                        bound = two;
                        ++twoVsOneRaised;
                    }
                }
            }
            // The episode table: the minimax over the participants of a small episode.
            if (options.selectiveEpisodeTable && breakawayAhead.opponentHeadStart == 0 &&
                (attack.kind == AttackEpisodeKind::OneVsZero || attack.kind == AttackEpisodeKind::TwoVsOne)) {
                ++tableLeaves;
                // The floor is part of the key: a result at or below it only says "not above".
                const double tableFloor = std::max(floor, bound);
                uint64_t floorBits;
                std::memcpy(&floorBits, &tableFloor, sizeof floorBits);
                const uint64_t key = PositionHash(s) ^ (static_cast<uint64_t>(attack.attackers) << 40) ^
                                     (static_cast<uint64_t>(attack.defenders) << 50) ^
                                     (floorBits * 0x9E3779B97F4A7C15ull);
                auto it = tableCache.find(key);
                double table;
                if (it != tableCache.end()) {
                    table = it->second;
                } else {
                    EpisodeValueOptions eo;
                    eo.turns = options.selectiveEpisodeTableTurns;
                    eo.nodeLimit = options.selectiveEpisodeTableNodes;
                    eo.floor = tableFloor;
                    const EpisodeValueResult r = EpisodeValue(s, attack.attackers, attack.defenders, eo);
                    // Diagnostics: every computed table, for ssbot table-bench.
                    if (!options.tableDumpPath.empty()) {
                        if (FILE* f = std::fopen(options.tableDumpPath.c_str(), "a")) {
                            static_assert(std::is_trivially_copyable<GameState>::value, "raw dump");
                            std::string hex;
                            const unsigned char* raw = reinterpret_cast<const unsigned char*>(&s);
                            for (std::size_t i = 0; i < sizeof(GameState); ++i) {
                                static const char* digits = "0123456789abcdef";
                                hex += digits[raw[i] >> 4];
                                hex += digits[raw[i] & 15];
                            }
                            std::fprintf(f, "%s|%u|%u|%.9f|%.9f|%d|%ld\n", hex.c_str(), attack.attackers,
                                         attack.defenders, eo.floor, r.value, r.complete ? 1 : 0, r.movementStates);
                            std::fclose(f);
                        }
                    }
                    table = r.complete ? r.value : -1;
                    if (tableCache.size() > 2000000) tableCache.clear();
                    tableCache.emplace(key, table);
                }
                if (table < 0) ++tableIncomplete;
                else if (table > bound) {
                    bound = table;
                    if (attackNow == episode.team ? table > value : -table < value) ++tableRaised;
                }
            }
            if (bound <= 0) return value;
            if (w > 0) bound = toValue(bound);
            if (attackNow == episode.team ? bound > value : -bound < value) ++breakawayRaised;
            return attackNow == episode.team ? std::max(value, bound) : std::min(value, -bound);
        }
        if (options.selectiveGeometricStatic) {
            const bool realized = partialTurn && options.selectivePartialTurnRealized;
            SetGeometricPossessionValue(ContinuationAllowed(s) ? options.selectivePossessionValue : 0);
            const auto compute = [&]() {
                return GeometricStaticEstimate(s, episode, options.selectiveLegalTackleZones,
                                               options.selectiveThreatenedRetention,
                                               options.selectiveForcedPickupPreview,
                                               options.selectiveExclusiveAttackChannels,
                                               options.selectiveTurnOrderStatic,
                                               options.selectiveCoverageStatic, realized,
                                               options.selectiveTimedRoute,
                                               options.selectiveNoShotFloor,
                                               options.selectiveTimedFreeBall,
                                               options.selectiveTerminalFreeBall);
            };
            const int bits = options.selectiveStaticCacheBits;
            if (bits <= 0 || bits > 26) return compute();
            if (staticCache.empty()) staticCache.resize(std::size_t(1) << bits);
            const StaticKey key = MakeStaticKey(s, episode, realized);
            if (sharedStatic) {
                const std::size_t hash = StaticKeyHash(key);
                ++staticCalls;
                double cached;
                if (sharedStatic->Find(key, hash, &cached)) {
                    ++staticHits;
                    return cached;
                }
                const double value = compute();
                sharedStatic->Store(key, hash, value);
                return value;
            }
            StaticCacheEntry& entry = staticCache[StaticKeyHash(key) & (staticCache.size() - 1)];
            ++staticCalls;
            if (entry.used && entry.key == key) {
                ++staticHits;
                return entry.value;
            }
            const double value = compute();
            entry.key = key;
            entry.value = value;
            entry.used = true;
            return value;
        }
        tReplyAware = options.selectiveReplyAware && race;
        tContinuation = options.selectiveContinuation;
        tSupportHorizon = options.selectiveSupportHorizon;
        tRaceDefence = options.selectiveRaceDefence;
        return PositionEstimate(s, episode, detailedRisk, options.selectiveRestartPosition,
                                options.selectiveBallStepPreview,
                                race && options.selectiveRaceFreeBall);
    }

    // A free ball by who reaches it first (bot_design.md 4.4). A certain take
    // is the state after it, valued as that possession; a certain restart is
    // the ball rolling out (or to a keeper) with everybody standing, then the
    // restart position. A contested race returns false: the caller searches it
    // further or falls back on the static estimate.
    bool RaceEstimate(const GameState& s, GoalEpisode episode, bool detailedRisk, double* value) const {
        const BallRaceResult r = SolveBallRace(s);
        if (r.kind == BallRaceKind::Taken) {
            ++raceLeaves[0];
            const GameState t = BallRaceTakenState(s, r);
            const GoalEpisode next = AdvanceGoalEpisode(episode, s, t, EventLog());
            const BreakawayAhead ahead = BallRaceTakenAhead(s, r);
            if (ahead.opponentHeadStart > 0 && !Terminal(next.status)) {
                // The take comes after more than one turn of the other team: the
                // taken state, with the opponents where they stand now, is no
                // position the game reaches and its static is not trusted. The
                // leaf keeps the static of the free ball, raised by what is
                // proven for the winner: his breakaway bound whatever the
                // opponents did meanwhile.
                ++raceLeaves[4];
                const double base = Estimate(s, episode, detailedRisk, true, false, false);
                if (!options.selectiveBreakawayBound) { *value = base; return true; }
                const bool ours = r.team == episode.team;
                const double w = ContinuationAllowed(t) ? options.selectivePossessionValue : 0;
                const double own = ours ? base : -base;
                double bound = BreakawayShotLowerBound(t, 12, w > 0 ? std::max(0.0, (own + w) / (1 + w)) : own, ahead);
                if (w > 0 && bound > 0) bound = bound * (1 + w) - w;
                *value = ours ? std::max(base, bound) : std::min(base, -bound);
                return true;
            }
            breakawayAhead = ahead;
            *value = Terminal(next.status) ? Bounds(t, next).lower : Estimate(t, next, detailedRisk, true, false, true);
            breakawayAhead = BreakawayAhead();
            return true;
        }
        if (r.kind == BallRaceKind::Restart) {
            GameState t = s;
            GoalEpisode e = episode;
            for (int guard = 0; guard < 64 && t.phase == Phase::Play && t.ball.IsFree() && !Terminal(e.status);
                 ++guard) {
                const std::vector<Outcome> outcomes = OutcomesUnchecked(t, Action::EndTurn());
                Outcome through = outcomes.front();
                for (const Outcome& o : outcomes)
                    if (o.kind == OutcomeKind::BallThrough) through = o;
                const GameState before = t;
                EventLog events;
                ApplyUnchecked(t, Action::EndTurn(), through, &events);
                e = AdvanceGoalEpisode(e, before, t, events);
            }
            if (Terminal(e.status)) {
                ++raceLeaves[1];
                *value = Bounds(t, e).lower;
                return true;
            }
            if (t.phase == Phase::Lineup) {
                const GameState restart = options.selectiveRestartPosition ? StandardRestart(t) : t;
                ++raceLeaves[1];
                *value = restart.phase == Phase::Play ? Estimate(restart, e, detailedRisk, true) : 0.0;
                return true;
            }
            return false;   // not the expected end: leave it to the static
        }
        ++raceLeaves[2];
        return false;
    }

    double ActionEstimate(const GameState& s, GoalEpisode episode, const Action& action,
                          bool detailedRisk = false, bool race = false, bool ballRace = false) {
        const bool scored = options.profileTree && profileStage == 7;
        const int kind = static_cast<int>(action.type);
        ScopedTreeTimer timer(scored ? &treeProfile.actionScoreNs[kind] : nullptr);
        if (scored) ++treeProfile.actionScoreCalls[kind];
        double value = 0;
        const std::vector<Outcome> outcomes = OutcomesUnchecked(s, action);
        if (scored) treeProfile.actionScoreOutcomes[kind] += outcomes.size();
        for (const Outcome& outcome : outcomes) {
            GameState child = s;
            EventLog events;
            ApplyUnchecked(child, action, outcome, &events);
            // In the ranking only the likely outcomes are worth the race.
            const bool raced = ballRace && outcome.probability >= options.selectiveRareOutcomeProbability;
            value += outcome.probability * Estimate(
                child, AdvanceGoalEpisode(episode, s, child, events), detailedRisk, race, false, raced);
        }
        return value;
    }

    // Players that do not move in the selective tree: the outsiders of the
    // attack episode of the node (7.67).
    // Round depth (09.11.2026, the user's rule): the root's decision is taken
    // only from depths whose leaves come after the opponent's reply (even team
    // turns), depth 1 only as a fallback; deeper than 2 the depth grows by a
    // whole round.
    int NextRootDepth(int depth) const {
        return options.selectiveRoundDepth && depth >= 2 ? depth + 2 : depth + 1;
    }
    bool CommitRootDepth(int depth) const {
        return !options.selectiveRoundDepth || depth == 1 || depth % 2 == 0;
    }

    unsigned FrozenPlayers() const {
        return (options.selectiveAttackEpisode && nodeEpisode ? nodeEpisode->outside : 0u) | options.frozenPlayers;
    }

    TeamMovementList SelectiveMovements(const GameState& initial, GoalEpisode episode) {
        TeamMovementList result;
        const unsigned frozen = FrozenPlayers();
        TeamMovementState rootNode;
        rootNode.state = initial;
        rootNode.episode = episode;
        if (options.selectiveAttackEpisode && nodeEpisode) {
            // The outsiders of the team to move take their policy moves first.
            for (const Action& move : OutsiderMoves(initial, *nodeEpisode)) {
                if (Terminal(rootNode.episode.status)) break;
                EventLog events;
                const GameState before = rootNode.state;
                ApplyUnchecked(rootNode.state, move, Outcome::Certain(), &events);
                rootNode.episode = AdvanceGoalEpisode(rootNode.episode, before, rootNode.state, events);
                rootNode.moves[rootNode.count++] = move;
            }
        }
        result.states.push_back(rootNode);
        std::vector<std::size_t> frontier{0};
        const bool maximize = initial.teamToMove == episode.team;
        const bool forcedDefence = options.selectiveDefenceForced && DefendingTurn(initial);
        const std::vector<Cell> forcedCells = forcedDefence ? InterceptCells(initial) : std::vector<Cell>();
        for (int ply = 0; ply < kPlayersPerTeam && !frontier.empty(); ++ply) {
            struct Ranked { double value; TeamMovementState node; std::size_t order; };
            std::vector<Ranked> candidates;
            for (std::size_t index : frontier) {
                const TeamMovementState parent = result.states[index];
                if (Terminal(parent.episode.status)) continue;
                const int first = FirstPlayerOf(parent.state.teamToMove);
                for (int player = first; player < first + kPlayersPerTeam; ++player) {
                    if (parent.state.players[player].moved || (frozen >> player & 1u)) continue;
                    for (Cell cell : MoveDestinations(parent.state, player)) {
                        if (cell == parent.state.players[player].pos) continue;
                        if (!TakeExpansion()) { result.complete = false; return result; }
                        ++stats.moveExpansions;
                        const Action action = Action::Move(player, cell);
                        TeamMovementState child;
                        child.state = parent.state;
                        EventLog events;
                        ApplyUnchecked(child.state, action, Outcome::Certain(), &events);
                        child.episode = AdvanceGoalEpisode(parent.episode, parent.state,
                                                           child.state, events);
                        child.count = parent.count + 1;
                        for (int step = 0; step < parent.count; ++step)
                            child.moves[step] = parent.moves[step];
                        child.moves[parent.count] = action;
                        ++result.transitions;
                        candidates.push_back({0.0, child, candidates.size()});
                    }
                }
            }
            {
                std::vector<EstimateJob> jobs;
                jobs.reserve(candidates.size());
                for (const Ranked& c : candidates)
                    jobs.push_back(StateJob(c.node.state, c.node.episode, false, false, true,
                                            options.selectiveRaceMovements));
                RunJobs(jobs);
                for (std::size_t i = 0; i < candidates.size(); ++i) candidates[i].value = jobs[i].value;
            }
            std::sort(candidates.begin(), candidates.end(), [maximize](const Ranked& a, const Ranked& b) {
                if (a.value != b.value) return maximize ? a.value > b.value : a.value < b.value;
                return a.order < b.order;
            });
            if (gNodeDiag.active && refining == 0 && SameSearchState(initial, gNodeDiag.root) &&
                ply < static_cast<int>(gNodeDiag.prefixes.size())) {
                const std::size_t width = static_cast<std::size_t>(MovementWidth());
                for (std::size_t r = 0; r < candidates.size(); ++r)
                    if (SameSearchState(candidates[r].node.state, gNodeDiag.prefixes[ply])) {
                        const double cut = candidates[std::min(candidates.size(), width) - 1].value;
                        std::size_t equal = 0;
                        for (const Ranked& c : candidates) equal += c.value == cut;
                        std::fprintf(stderr, "diag: ply %d: target prefix rank %zu of %zu, value %.4f, cut %.4f (%zu candidates equal to the cut)\n",
                                     ply + 1, r + 1, candidates.size(), candidates[r].value, cut, equal);
                    }
                if (std::getenv("SS_DIAG_LIST")) {
                    std::map<double, int> histogram;
                    for (const Ranked& c : candidates) ++histogram[c.value];
                    std::fprintf(stderr, "diag: ply %d values:", ply + 1);
                    for (const auto& [v, n] : histogram) std::fprintf(stderr, " %.4f x%d", v, n);
                    std::fprintf(stderr, "\n");
                    for (std::size_t r = 0; r < std::min<std::size_t>(candidates.size(), 12); ++r) {
                        std::string line;
                        for (int m = 0; m < candidates[r].node.count; ++m)
                            line += FormatAction(candidates[r].node.state, candidates[r].node.moves[m]) + " ";
                        std::fprintf(stderr, "diag:   %2zu %.4f %s\n", r + 1, candidates[r].value, line.c_str());
                    }
                }
            }
            // Lineage diversity (06.11): the root searches each of its first
            // moves with its own movement beam; this beam keeps, before the best
            // overall, the best continuation of each first move (lineage), so
            // the kept partial states are not all variations of one idea.
            // Player diversity (06.11): the best move of each player first, so a
            // beam dominated by one player's cells (the racer for a free ball)
            // still carries the others' moves.
            if (options.selectivePlayerDiversity)
                std::stable_partition(candidates.begin(), candidates.end(), [&, seen = 0u](const Ranked& c) mutable {
                    const int player = c.node.moves[c.node.count - 1].player;
                    if (seen >> player & 1u) return false;
                    seen |= 1u << player;
                    return true;
                });
            if (options.selectiveLineageDiversity && ply > 0)
                std::stable_partition(candidates.begin(), candidates.end(), [&, seen = std::vector<Action>()](const Ranked& c) mutable {
                    const Action& lead = c.node.moves[0];
                    for (const Action& a : seen)
                        if (a.player == lead.player && a.target == lead.target) return false;
                    seen.push_back(lead);
                    return true;
                });
            frontier.clear();
            // Pickup diversity: the best move that takes the free ball keeps a
            // place in the ply's beam (the ranking may value the free ball as
            // already owned and the taking move below it).
            std::size_t pickup = candidates.size();
            if (options.selectivePickupDiversity >= 2)
                for (std::size_t i = 0; i < candidates.size(); ++i) {
                    const GameState& from = result.states[0].state;
                    const GameState& to = candidates[i].node.state;
                    if (from.ball.IsFree() && !to.ball.IsFree() && TeamOf(to.ball.holder) == initial.teamToMove) {
                        pickup = i;
                        break;
                    }
                }
            bool pickupKept = pickup == candidates.size();
            for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
                const Ranked& candidate = candidates[ci];
                if (!pickupKept && frontier.size() + 1 >= static_cast<std::size_t>(MovementWidth()) && ci < pickup) {
                    if (ci != pickup) continue;   // the last place waits for the pickup
                }
                if (ci == pickup) pickupKept = true;
                bool duplicate = false;
                for (const TeamMovementState& previous : result.states)
                    if (previous.episode.status == candidate.node.episode.status &&
                        SameSearchState(previous.state, candidate.node.state)) {
                        duplicate = true;
                        break;
                    }
                if (duplicate) { ++result.duplicates; continue; }
                if (frontier.size() >= static_cast<std::size_t>(MovementWidth())) break;
                frontier.push_back(result.states.size());
                result.states.push_back(candidate.node);
            }
            // Defence forced (09.11): the best move of each player that lets him
            // tackle or puts him on the ball's path joins the beam beyond its width.
            if (forcedDefence) {
                unsigned added = 0;
                for (const Ranked& candidate : candidates) {
                    const int player = candidate.node.moves[candidate.node.count - 1].player;
                    if (added >> player & 1u) continue;
                    if (!ForcedPlayer(candidate.node.state, player, forcedCells)) continue;
                    added |= 1u << player;
                    bool present = false;
                    for (const TeamMovementState& previous : result.states)
                        if (previous.episode.status == candidate.node.episode.status &&
                            SameSearchState(previous.state, candidate.node.state)) {
                            present = true;
                            break;
                        }
                    if (present) continue;
                    frontier.push_back(result.states.size());
                    result.states.push_back(candidate.node);
                    ++stats.defenceForcedKept;
                }
            }
            stats.selectiveMovementDrops += static_cast<long>(candidates.size() - frontier.size());
        }
        if (options.selectiveGreedyMovements && !initial.ball.IsFree() &&
            TeamOf(initial.ball.holder) == initial.teamToMove) {
            const int first = FirstPlayerOf(initial.teamToMove);
            const int goal = AttackedGoalRow(initial.teamToMove);
            std::function<void(const TeamMovementState&)> complete = [&](const TeamMovementState& node) {
                if (!result.complete || Terminal(node.episode.status)) return;
                if (node.count > 0) {
                    bool duplicate = false;
                    for (const TeamMovementState& prior : result.states)
                        if (prior.episode.status == node.episode.status &&
                            SameSearchState(prior.state, node.state)) {
                            duplicate = true;
                            break;
                        }
                    if (!duplicate) result.states.push_back(node);
                }
                for (int player = first; player < first + kPlayersPerTeam; ++player) {
                    if (node.state.players[player].moved || (frozen >> player & 1u)) continue;
                    const std::vector<Cell> destinations = MoveDestinations(node.state, player);
                    Cell best{};
                    bool haveBest = false;
                    for (Cell cell : destinations) {
                        if (cell == node.state.players[player].pos) continue;
                        if (!haveBest ||
                            Abs(cell.y - goal) < Abs(best.y - goal) ||
                            (Abs(cell.y - goal) == Abs(best.y - goal) &&
                             Abs(cell.x - kGoalCenterX) < Abs(best.x - kGoalCenterX))) {
                            best = cell;
                            haveBest = true;
                        }
                    }
                    for (Cell cell : destinations) {
                        if (cell == node.state.players[player].pos ||
                            (node.count > 0 && cell != best)) continue;
                        if (!TakeExpansion()) { result.complete = false; return; }
                        ++stats.moveExpansions;
                        TeamMovementState child = node;
                        const Action action = Action::Move(player, cell);
                        EventLog events;
                        ApplyUnchecked(child.state, action, Outcome::Certain(), &events);
                        child.episode = AdvanceGoalEpisode(node.episode, node.state,
                                                           child.state, events);
                        child.moves[child.count++] = action;
                        ++result.transitions;
                        complete(child);
                        if (!result.complete) return;
                    }
                }
            };
            complete(rootNode);
        }
        return result;
    }

    // The ball has come to a player outside the node's attack episode: the
    // episode is over, the static estimate values the rest (7.67).
    bool EpisodeExit(const GameState& child, GoalEpisode next) const {
        return options.selectiveEpisodeExits && exitMask && child.phase == Phase::Play &&
               !Terminal(next.status) && !child.ball.IsFree() && (exitMask >> child.ball.holder & 1u);
    }

    GoalValueBounds ActionBounds(const GameState& s, GoalEpisode episode, const Action& action,
                                 int depth, double alpha, double beta,
                                 std::vector<Action>* turnContinuation = nullptr) {
        const GoalValueBounds base = Bounds(s, episode);
        if (!TakeExpansion()) return base;
        switch (action.type) {
            case ActionType::Place: ++stats.placeExpansions; break;
            case ActionType::Move: ++stats.moveExpansions; break;
            case ActionType::Attack: ++stats.attackExpansions; break;
            case ActionType::Pass: ++stats.passExpansions; break;
            case ActionType::Shot: ++stats.shotExpansions; break;
            case ActionType::EndTurn:
                ++stats.endTurnExpansions;
                if (s.phase == Phase::Play && s.ball.IsFree() && s.ball.stepPhase == 0)
                    ++stats.ballStepExpansions;
                break;
        }
        auto certain = [&](const Outcome& outcome, std::size_t outcomeBytes) {
            MemoryScope frame(*this, outcomeBytes + sizeof(GameState) + 128);
            if (!frame.admitted || !Budget()) return base;
            ++stats.outcomesProcessed;
            GameState child = s;
            EventLog events;
            ApplyUnchecked(child, action, outcome, &events);
            const GoalEpisode next = AdvanceGoalEpisode(episode, s, child, events);
            maxDepth = std::max(maxDepth, child.halfTurns - rootHalfTurns);
            const int remaining = depth - (options.completeTurns && child.turnId != s.turnId ? 1 : 0);
            if (options.selectiveWidth == 0 && action.type == ActionType::Pass &&
                episode.status == GoalEpisodeStatus::OurAttack &&
                s.teamToMove == episode.team && next.status == GoalEpisodeStatus::OurAttack) {
                if (ProvenOpponentPickupBeforeBallStep(child)) {
                    ++stats.forcedPickupPrunes;
                    return Intersect(base, GoalValueBounds{-1, 0});
                }
                if (outcome.kind == OutcomeKind::BallThrough && outcome.probability == 1.0 &&
                    ProvenOpponentPickupAtNextBallStep(child)) {
                    ++stats.nextStepPickupPrunes;
                    return Intersect(base, GoalValueBounds{-1, 0});
                }
                if (outcome.kind == OutcomeKind::BallThrough && outcome.probability == 1.0 &&
                    ProvenNoOwnRecoveryOnPassiveReply(child)) {
                    ++stats.noRecoveryPrunes;
                    return Intersect(base, GoalValueBounds{-1, 0});
                }
            }
            if (EpisodeExit(child, next)) {
                const double estimate = Estimate(child, next, true, true, false, true);
                return GoalValueBounds{estimate, estimate};
            }
            std::vector<Action>* continuation = action.type == ActionType::Move &&
                child.turnId == s.turnId ? turnContinuation : nullptr;
            return Intersect(base, Solve(child, next, remaining, alpha, beta, continuation));
        };
        // These actions are certain in the engine. A free-ball step at EndTurn
        // can have interceptions and must still enumerate its outcomes.
        if (action.type == ActionType::Place || action.type == ActionType::Move ||
            (action.type == ActionType::EndTurn &&
             !(s.phase == Phase::Play && s.ball.IsFree() && s.ball.stepPhase == 0)))
            return certain(Outcome::Certain(), 0);
        std::vector<Outcome> outcomes = OutcomesUnchecked(s, action);
        if (outcomes.size() == 1 && outcomes[0].probability == 1.0)
            return certain(outcomes[0], outcomes.capacity() * sizeof(Outcome));
        std::stable_sort(outcomes.begin(), outcomes.end(), [](const Outcome& a, const Outcome& b) {
            return a.probability > b.probability;
        });
        const std::size_t bytes = outcomes.capacity() * sizeof(Outcome) +
                                  (outcomes.size() + 1) * sizeof(GoalValueBounds) + sizeof(GameState) + 128;
        MemoryScope frame(*this, bytes);
        if (!frame.admitted) return base;
        std::vector<GoalValueBounds> suffix(outcomes.size() + 1, {0, 0});
        for (std::size_t i = outcomes.size(); i-- > 0;) {
            suffix[i].lower = std::max(0.0, Down(suffix[i + 1].lower + outcomes[i].probability));
            suffix[i].upper = Up(suffix[i + 1].upper + outcomes[i].probability);
        }
        GoalValueBounds done{0, 0};
        // GoalStateBounds(s) bounds the expectation before the random action,
        // not each individual outcome. A rare goal branch can be worth 1 even
        // when the state's immediate-shot upper bound is much smaller.
        const GoalValueBounds outcomeEnvelope = Envelope(s, episode);
        auto partial = [&](std::size_t next) {
            return Intersect(base, Add(done, Scale(suffix[next].lower, suffix[next].upper,
                                                  outcomeEnvelope)));
        };
        std::size_t processed = 0;
        for (std::size_t i = 0; i < outcomes.size(); ++i) {
            if (!Budget()) break;
            ++stats.outcomesProcessed;
            GameState child = s;
            EventLog events;
            ApplyUnchecked(child, action, outcomes[i], &events);
            const GoalEpisode next = AdvanceGoalEpisode(episode, s, child, events);
            maxDepth = std::max(maxDepth, child.halfTurns - rootHalfTurns);
            const int remaining = depth - (options.completeTurns && child.turnId != s.turnId ? 1 : 0);
            GoalValueBounds value;
            if (options.selectiveWidth == 0 && action.type == ActionType::Pass &&
                episode.status == GoalEpisodeStatus::OurAttack &&
                s.teamToMove == episode.team && next.status == GoalEpisodeStatus::OurAttack &&
                ProvenOpponentPickupBeforeBallStep(child)) {
                ++stats.forcedPickupPrunes;
                value = {-1, 0};
            } else if (EpisodeExit(child, next)) {
                const double estimate = Estimate(child, next, true, true, false, true);
                value = {estimate, estimate};
            } else if (options.selectiveWidth > 0 && options.selectiveMainOutcome && !Terminal(next.status) &&
                       ((action.type == ActionType::Pass && outcomes[i].kind != OutcomeKind::BallThrough) ||
                        (action.type == ActionType::Attack && outcomes[i].kind == OutcomeKind::KnockedAway))) {
                // Main outcome only (07.11.2026, the user's rule): after a random
                // event the position is new, and only the price of failure matters -
                // a pass caught or deflected is a loss of the ball, valued by the
                // static after the end of the turn (the opponent to move: his
                // counter); a ball knocked away in a tackle likewise. Not searched.
                const double estimate = child.phase == Phase::Play && !child.ballPlayed && !child.committed
                                            ? Estimate(child, next, true, true, false, true)
                                            : ActionEstimate(child, next, Action::EndTurn(), true, true, true);
                value = {estimate, estimate};
                ++stats.mainOutcomeStatic;
                // The price of a lost ball checked by search (07.11.2026): where the
                // reach map gives the opponent who caught it, first of all, a cell
                // with the field's best open shot (in front of the goal) and the
                // static gives him less, the rest of this turn and his turn are searched (on the
                // games the static undervalues such a counter by ~0.16, one turn
                // of search corrects it; elsewhere by ~0.04).
                if (options.selectiveLossReachCheck && child.phase == Phase::Play && !child.ball.IsFree() &&
                    TeamOf(child.ball.holder) != s.teamToMove && Budget()) {
                    ++stats.reachMaps;
                    const ReachBreakaway b = ReachHolderBreakaway(child, ComputeReachFront(child));
                    const double holderValue = TeamOf(child.ball.holder) == episode.team ? estimate : -estimate;
                    if (b.shot >= kBestOpenShot && b.shot > holderValue) {
                        ++stats.lossChecks;
                        value = Solve(child, next, 2, -1, 1);
                    }
                }
            } else if (options.selectiveWidth > 0 && options.selectiveRareOutcomeProbability > 0 &&
                       outcomes[i].probability < options.selectiveRareOutcomeProbability &&
                       !Terminal(next.status)) {
                // A rare outcome (one of many deflection cells, say) keeps its
                // weight in the expectation but is valued statically, not searched.
                const double estimate = Estimate(child, next, true, true, false, true);
                value = {estimate, estimate};
            } else {
                value = Solve(child, next, remaining, -1, 1);
            }
            done = Add(done, Scale(outcomes[i].probability, outcomes[i].probability, value));
            processed = i + 1;
            const GoalValueBounds possible = partial(i + 1);
            if (possible.upper <= alpha || possible.lower >= beta) {
                ++stats.cutoffs;
                return possible;
            }
            if (stopped) return possible;
        }
        return partial(processed);
    }

    GoalValueBounds Solve(const GameState& s, GoalEpisode episode, int depth, double alpha, double beta,
                          std::vector<Action>* turnContinuation = nullptr) {
        GoalValueBounds base = Bounds(s, episode);
        if (Terminal(episode.status)) return base;
        // Placement is outside a play half-turn. Its combinatorial restart is
        // approximated at the selective horizon instead of silently searching
        // millions of line-up orders with no beam.
        if (options.selectiveWidth > 0 && s.phase == Phase::Lineup) {
            // The restart is an ordinary node: its forced line-up leads to a
            // known position, searched with the remaining depth.
            if (options.selectiveRestartPosition) {
                const GameState restart = StandardRestart(s);
                if (restart.phase == Phase::Play)
                    return Solve(restart, episode, depth, alpha, beta, nullptr);
            }
            const double estimate = Estimate(s, episode);
            return {estimate, estimate};
        }
        if (options.selectiveWidth > 0 && (depth <= 0 || !Budget())) {
            // A contested race for a free ball at the horizon is not valued
            // statically: one more team turn is searched (bot_design.md 4.4).
            // The decisive moment of the line just beyond the horizon: the
            // carrier of the attack episode can stand on his best shooting
            // cell within two rounds (plies 1..4) - the static sees one round,
            // the extension one more. One more round (two team turns, so the
            // leaves keep the same side to move) is searched.
            if (depth <= 0 && options.selectiveEpisodeExtension &&
                episodeExtensions < options.selectiveEpisodeExtensionLimit && s.phase == Phase::Play &&
                s.keeperRestartTeam < 0 && Budget()) {
                const AttackEpisode leafEpisode = AnalyzeAttackEpisode(s, false, options.selectiveEpisodeBallRace);
                bool inWindow = true;
                if (options.selectiveExtensionInWindow && leafEpisode.kind != AttackEpisodeKind::None &&
                    leafEpisode.endPly <= 4) {
                    // A leaf whose estimate already fails the window changes
                    // nothing above it: valued statically as before.
                    const double estimate = Estimate(s, episode, true, true, false, true);
                    inWindow = estimate > alpha && estimate < beta;
                    if (!inWindow) return {estimate, estimate};
                }
                bool detected = leafEpisode.kind != AttackEpisodeKind::None && leafEpisode.endPly <= 4;
                // The reach map as a second detector (07.11.2026): the holder's own
                // wave - cells he reaches before any opponent - holds, within the
                // same two rounds, an open shot above what the leaf gives his team.
                // A possibility only; the extension decides by search.
                if (!detected && options.selectiveReachExtension && !s.ball.IsFree()) {
                    const double estimate = Estimate(s, episode, true, true, false, true);
                    inWindow = !options.selectiveExtensionInWindow || (estimate > alpha && estimate < beta);
                    if (!inWindow) return {estimate, estimate};
                    ++stats.reachMaps;
                    const ReachFront front = ComputeReachFront(s, 4);
                    const ReachBreakaway b = ReachHolderBreakaway(s, front);
                    const double holderValue = TeamOf(s.ball.holder) == episode.team ? estimate : -estimate;
                    if (b.shot > holderValue) {
                        detected = true;
                        ++stats.reachExtensions;
                    }
                }
                if (inWindow && detected) {
                    ++episodeExtensionCount;
                    ++episodeExtensions;
                    const GoalValueBounds value = Solve(s, episode, 2, alpha, beta, turnContinuation);
                    --episodeExtensions;
                    return value;
                }
            }
            if (depth <= 0 && options.selectiveBallRace && raceExtensions < options.selectiveBallRaceExtensions &&
                s.phase == Phase::Play && s.ball.IsFree() && s.keeperRestartTeam < 0 && Budget() &&
                SolveBallRace(s).kind == BallRaceKind::Contested) {
                ++raceLeaves[3];
                ++raceExtensions;
                const GoalValueBounds value = Solve(s, episode, 1, alpha, beta, turnContinuation);
                --raceExtensions;
                return value;
            }
            const double estimate = Estimate(s, episode, true, true, false, true);
            return {estimate, estimate};
        }
        if (depth <= 0 || !Budget()) return base;
        ++positions;
        if (options.selectiveWidth == 0) base = Cached(s, episode, base);
        if (base.lower >= base.upper) return base;
        // The selective tree clips internal values to `base`, but not leaves, so
        // a window test against `base` is kept out of the selective search.
        if (options.selectiveWidth == 0 && (base.upper <= alpha || base.lower >= beta)) return base;
        const bool maximize = s.teamToMove == episode.team;
        GoalValueBounds best = maximize ? GoalValueBounds{-1, -1} : GoalValueBounds{1, 1};
        bool complete = true;
        auto consider = [&](GoalValueBounds value) {
            if (maximize) {
                best.lower = std::max(best.lower, value.lower);
                best.upper = std::max(best.upper, value.upper);
                alpha = std::max(alpha, best.lower);
                if (best.lower >= beta) { ++stats.cutoffs; return false; }
            } else {
                best.lower = std::min(best.lower, value.lower);
                best.upper = std::min(best.upper, value.upper);
                beta = std::min(beta, best.upper);
                if (best.upper <= alpha) { ++stats.cutoffs; return false; }
            }
            return true;
        };
        // After a cutoff the unsearched candidates are unknown: keep only the
        // side of the interval that the searched ones establish.
        const auto openCut = [&]() {
            if (maximize) best.upper = std::max(best.upper, base.upper);
            else best.lower = std::min(best.lower, base.lower);
        };
        const bool windowed = options.selectiveWidth > 0 && options.selectiveAlphaBeta;
        // The node that records the root's continuation is always searched.
        if (options.selectiveWidth > 0 && options.selectiveTransposition && !turnContinuation &&
            s.phase == Phase::Play) {
            if (const SelectiveEntry* entry = SelectiveProbe(s, episode)) {
                // Only the same remaining depth: a deeper value would change the result.
                if (entry->depth == TableDepth(depth)) {
                    const GoalValueBounds known = Intersect(base, entry->bounds);
                    // A point value (complete node) or a bound that decides the window.
                    if (known.upper - known.lower <= 1e-12 || known.upper <= alpha ||
                        known.lower >= beta) {
                        ++stats.selectiveTableCuts;
                        return known;
                    }
                }
            }
        }
        // The node's attack episode (7.67), restored for the caller on return.
        struct EpisodeScope {
            Impl& owner;
            const AttackEpisode* episode;
            unsigned mask;
            EpisodeScope(Impl& o) : owner(o), episode(o.nodeEpisode), mask(o.exitMask) {}
            ~EpisodeScope() { owner.nodeEpisode = episode; owner.exitMask = mask; }
        } episodeScope(*this);
        AttackEpisode attackEpisode;
        if (options.selectiveWidth > 0 && options.selectiveAttackEpisode && s.phase == Phase::Play) {
            attackEpisode = AnalyzeAttackEpisode(s, false, options.selectiveEpisodeBallRace);
            nodeEpisode = &attackEpisode;
            exitMask = attackEpisode.outside;
        }
        if (options.selectiveWidth > 0 && options.groupMovements &&
            options.completeTurns && s.phase == Phase::Play) {
            bool haveLine = false;
            double lineValue = maximize ? -1 : 1;
            const auto remember = [&](const TeamMovementState& node, const Action* action,
                                      GoalValueBounds value) {
                if (!turnContinuation) return;
                const double candidate = maximize ? value.lower : value.upper;
                if (haveLine && (maximize ? candidate <= lineValue : candidate >= lineValue)) return;
                haveLine = true;
                lineValue = candidate;
                turnContinuation->assign(node.moves, node.moves + node.count);
                if (action) turnContinuation->push_back(*action);
            };
            TeamMovementList movements = SelectiveMovements(s, episode);
            MemoryScope movementFrame(*this, movements.states.capacity() * sizeof(TeamMovementState));
            if (!movementFrame.admitted || !movements.complete) return base;
            const bool diag = gNodeDiag.active && refining == 0 &&
                              SameSearchState(s, gNodeDiag.root);
            long diagState = -1;
            if (diag) {
                const auto find = [&](const GameState& g) -> long {
                    for (std::size_t i = 0; i < movements.states.size(); ++i)
                        if (SameSearchState(movements.states[i].state, g)) return static_cast<long>(i);
                    return -1;
                };
                if (gNodeDiag.prefixes.empty()) diagState = 0;
                else {
                    for (std::size_t k = 0; k < gNodeDiag.prefixes.size(); ++k)
                        if (find(gNodeDiag.prefixes[k]) < 0) {
                            std::fprintf(stderr, "diag: movement beam lost the target after %zu of %zu moves (beam states %zu)\n",
                                         k, gNodeDiag.prefixes.size(), movements.states.size());
                            break;
                        }
                    diagState = find(gNodeDiag.prefixes.back());
                }
                if (diagState >= 0)
                    std::fprintf(stderr, "diag: movement state found (index %ld of %zu)\n", diagState, movements.states.size());
            }
            using RankedState = std::pair<double, std::size_t>;
            std::vector<RankedState> rankedStates;
            MemoryScope rankFrame(*this, movements.states.size() * sizeof(RankedState));
            if (!rankFrame.admitted) return base;
            rankedStates.reserve(movements.states.size());
            const int previewTop = options.selectiveEndTurnPreview ? options.selectiveEndTurnPreviewTop : 0;
            {
                if (!Budget()) return base;
                std::vector<EstimateJob> jobs;
                std::vector<int> endJob(movements.states.size(), -1);
                jobs.reserve(2 * movements.states.size());
                for (std::size_t i = 0; i < movements.states.size(); ++i) {
                    const TeamMovementState& node = movements.states[i];
                    jobs.push_back(StateJob(node.state, node.episode, true, false, true,
                                            options.selectiveRaceMovements));
                    if (options.selectiveEndTurnPreview && previewTop <= 0 && !Terminal(node.episode.status)) {
                        endJob[i] = static_cast<int>(jobs.size());
                        jobs.push_back(ActionJob(node.state, node.episode, Action::EndTurn(), true));
                    }
                }
                RunJobs(jobs);
                std::size_t at = 0;
                for (std::size_t i = 0; i < movements.states.size(); ++i) {
                    double estimate = jobs[at++].value;
                    if (endJob[i] >= 0) {
                        const double ended = jobs[at++].value;
                        estimate = maximize ? std::max(estimate, ended) : std::min(estimate, ended);
                    }
                    rankedStates.push_back({estimate, i});
                }
            }
            if (previewTop > 0) {
                // Two stages: every movement state by its own estimate, then the
                // EndTurn preview (a full estimate of the finished turn) only for
                // the best `previewTop` of them.
                std::vector<RankedState> byOwn = rankedStates;
                const std::size_t top = std::min(byOwn.size(), static_cast<std::size_t>(previewTop));
                std::partial_sort(byOwn.begin(), byOwn.begin() + top, byOwn.end(),
                                  [maximize](const RankedState& a, const RankedState& b) {
                                      if (a.first != b.first) return maximize ? a.first > b.first : a.first < b.first;
                                      return a.second < b.second;
                                  });
                for (std::size_t r = 0; r < top; ++r) {
                    if (!Budget()) return base;
                    const std::size_t i = byOwn[r].second;
                    const TeamMovementState& node = movements.states[i];
                    if (Terminal(node.episode.status)) continue;
                    const double ended = ActionEstimate(node.state, node.episode, Action::EndTurn(), true);
                    double& estimate = rankedStates[i].first;
                    estimate = maximize ? std::max(estimate, ended) : std::min(estimate, ended);
                }
            }
            auto better = [maximize](const auto& a, const auto& b) {
                if (a.first != b.first) return maximize ? a.first > b.first : a.first < b.first;
                return a.second < b.second;
            };
            // Refined ranking (06.11, experiment): the static gives many movement
            // states the same value; the best `selectiveRefineStates` of them are
            // valued again by a short search - the end of the turn and the
            // opponent's reply turn - and only they compete for the beam.
            if (options.selectiveRefineStates > 0 && refining == 0 && episodeExtensions == 0 &&
                rankedStates.size() > static_cast<std::size_t>(BeamWidth())) {
                std::vector<RankedState> order = rankedStates;
                std::stable_sort(order.begin(), order.end(), better);
                const std::size_t top = std::min(order.size(), static_cast<std::size_t>(options.selectiveRefineStates));
                std::vector<char> refined(rankedStates.size(), 0);
                ++refining;
                for (std::size_t r = 0; r < top && Budget(); ++r) {
                    const std::size_t i = order[r].second;
                    const TeamMovementState& node = movements.states[i];
                    if (Terminal(node.episode.status)) continue;
                    const double own = Estimate(node.state, node.episode, true, false, true, options.selectiveRaceMovements);
                    const GoalValueBounds reply = ActionBounds(node.state, node.episode, Action::EndTurn(), 2, -1, 1);
                    const double ended = maximize ? reply.lower : reply.upper;
                    for (RankedState& entry : rankedStates)
                        if (entry.second == i) {
                            entry.first = maximize ? std::max(own, ended) : std::min(own, ended);
                            refined[i] = 1;
                        }
                    ++stats.selectiveRefinedStates;
                }
                --refining;
                for (RankedState& entry : rankedStates)
                    if (!refined[entry.second]) entry.first = maximize ? -2 : 2;
            }
            if (diag && diagState >= 0) {
                std::vector<RankedState> order = rankedStates;
                std::stable_sort(order.begin(), order.end(), better);
                for (std::size_t r = 0; r < order.size(); ++r)
                    if (order[r].second == static_cast<std::size_t>(diagState))
                        std::fprintf(stderr, "diag: movement state rank %zu of %zu, estimate %.4f (best %.4f, 8th %.4f)\n", r + 1,
                                     order.size(), order[r].first, order[0].first,
                                     order[std::min<std::size_t>(order.size(), BeamWidth()) - 1].first);
            }
            const std::vector<RankedState> allRanked = rankedStates;
            const std::size_t stateCount = std::min(rankedStates.size(),
                                                    static_cast<std::size_t>(BeamWidth()));
            if (options.selectiveBallDiversity) {
                std::sort(rankedStates.begin(), rankedStates.end(), better);
                std::vector<RankedState> chosen;
                chosen.reserve(stateCount);
                // Lineage diversity (06.11): one state per first move first.
                if (options.selectiveLineageDiversity)
                    for (const RankedState& ranked : rankedStates) {
                        if (chosen.size() >= stateCount / 2) break;
                        const TeamMovementState& node = movements.states[ranked.second];
                        if (node.count == 0) continue;
                        const Action& lead = node.moves[0];
                        if (std::none_of(chosen.begin(), chosen.end(), [&](const RankedState& prior) {
                                const TeamMovementState& other = movements.states[prior.second];
                                return other.count > 0 && other.moves[0].player == lead.player &&
                                       other.moves[0].target == lead.target;
                            })) chosen.push_back(ranked);
                    }
                for (const RankedState& ranked : rankedStates) {
                    if (chosen.size() == stateCount) break;
                    if (std::any_of(chosen.begin(), chosen.end(), [&](const RankedState& prior) {
                            return prior.second == ranked.second;
                        })) continue;
                    const GameState& candidate = movements.states[ranked.second].state;
                    bool represented = false;
                    for (const RankedState& prior : chosen) {
                        const GameState& existing = movements.states[prior.second].state;
                        if (candidate.ball.holder == existing.ball.holder &&
                            candidate.ball.pos == existing.ball.pos) {
                            represented = true;
                            break;
                        }
                    }
                    if (!represented) chosen.push_back(ranked);
                    if (chosen.size() == stateCount) break;
                }
                for (const RankedState& ranked : rankedStates) {
                    if (chosen.size() == stateCount) break;
                    if (std::none_of(chosen.begin(), chosen.end(), [&](const RankedState& prior) {
                            return prior.second == ranked.second;
                        })) chosen.push_back(ranked);
                }
                rankedStates = std::move(chosen);
            } else {
                std::partial_sort(rankedStates.begin(), rankedStates.begin() + stateCount,
                                  rankedStates.end(), better);
                rankedStates.resize(stateCount);
            }
            if (stateCount && std::none_of(rankedStates.begin(), rankedStates.end(),
                                          [](const auto& entry) { return entry.second == 0; }))
                rankedStates.back() = {Estimate(s, episode), 0};
            // Defence forced (09.11): the movement states with a tackle or an
            // interception stay for the decision beam, beyond the width.
            const bool forcedDefence = options.selectiveDefenceForced && DefendingTurn(s);
            const std::vector<Cell> forcedCells = forcedDefence ? InterceptCells(s) : std::vector<Cell>();
            std::vector<char> forcedStateOf(movements.states.size(), 0);
            if (forcedDefence) {
                for (std::size_t i = 0; i < movements.states.size(); ++i)
                    forcedStateOf[i] = !Terminal(movements.states[i].episode.status) &&
                                       ForcedState(movements.states[i].state, forcedCells);
                std::vector<RankedState> order = allRanked;
                std::stable_sort(order.begin(), order.end(), better);
                for (const RankedState& ranked : order) {
                    if (!forcedStateOf[ranked.second]) continue;
                    if (std::any_of(rankedStates.begin(), rankedStates.end(),
                                    [&](const RankedState& kept) { return kept.second == ranked.second; })) continue;
                    rankedStates.push_back(ranked);
                }
            }
            if (diag && diagState >= 0)
                std::fprintf(stderr, "diag: movement state %s the %zu kept\n",
                             std::any_of(rankedStates.begin(), rankedStates.end(), [&](const auto& e) {
                                 return e.second == static_cast<std::size_t>(diagState); }) ? "among" : "NOT among",
                             rankedStates.size());
            stats.selectiveMovementDrops += static_cast<long>(movements.states.size() - stateCount);

            struct Candidate { double estimate; std::size_t stateIndex; Action action; std::size_t order; };
            std::vector<Candidate> candidates;
            for (const RankedState& ranked : rankedStates) {
                const TeamMovementState& node = movements.states[ranked.second];
                if (Terminal(node.episode.status)) {
                    const GoalValueBounds value = Bounds(node.state, node.episode);
                    remember(node, nullptr, value);
                    if (!consider(value)) {
                        complete = false;
                        break;
                    }
                    continue;
                }
                std::vector<Action> legal = LegalActions(node.state);
                if (options.selectiveReachPassPrune)
                    stats.reachPassPrunes += PruneReachPasses(node.state, legal, &stats.reachMaps);
                for (const Action& action : legal) {
                    if (action.type == ActionType::Move) continue;
                    candidates.push_back({0.0, ranked.second, action, candidates.size()});
                }
            }
            if (complete) {
                std::vector<EstimateJob> jobs;
                jobs.reserve(candidates.size());
                for (const Candidate& c : candidates) {
                    const TeamMovementState& node = movements.states[c.stateIndex];
                    const bool detailed = options.selectiveEndTurnPreview && c.action.type == ActionType::EndTurn;
                    jobs.push_back(ActionJob(node.state, node.episode, c.action, detailed));
                }
                RunJobs(jobs);
                for (std::size_t i = 0; i < candidates.size(); ++i) candidates[i].estimate = jobs[i].value;
            }
            if (!complete) {
                if (windowed && !stopped) openCut();
                if (!stopped && options.selectiveTransposition && !turnContinuation)
                    SelectiveStore(s, episode, TableDepth(depth), Intersect(base, best));
                return Intersect(base, best);
            }
            MemoryScope candidateFrame(*this, candidates.capacity() * sizeof(Candidate));
            if (!candidateFrame.admitted) return base;
            const auto betterCandidate = [&](const Candidate& a, const Candidate& b) {
                if (a.estimate != b.estimate)
                    return maximize ? a.estimate > b.estimate : a.estimate < b.estimate;
                return a.order < b.order;
            };
            std::size_t count = std::min(candidates.size(),
                                         static_cast<std::size_t>(BeamWidth()));
            if (options.selectiveKindDiversity) {
                std::vector<double> cheap;
                std::vector<int> kinds;
                std::vector<std::size_t> groups;
                for (const Candidate& c : candidates) {
                    cheap.push_back(c.estimate);
                    const GameState& from = movements.states[c.stateIndex].state;
                    int kind = options.selectivePartnerCategories && !from.ball.IsFree() &&
                                       TeamOf(from.ball.holder) == from.teamToMove
                                   ? PartnerCategory(from, c.action)
                                   : DecisionKind(from, c.action);
                    // Ball first (06.11): a pass or shot from the unmoved state - the
                    // movements follow it - is a decision of its own kind, as the
                    // root's first actions are; otherwise it competes with the same
                    // action after movements, which the static ranks higher.
                    if (options.selectiveBallFirstKinds && movements.states[c.stateIndex].count == 0 &&
                        c.action.type != ActionType::EndTurn)
                        kind += 16;
                    kinds.push_back(kind);
                    groups.push_back(c.stateIndex);
                }
                std::vector<double> kept;
                // With the race ranking a decision that frees the ball is valued
                // by who takes it, as its leaf would be.
                const bool raceRanking = options.selectiveBallRace && options.selectiveRaceRanking;
                std::vector<double> prefetched(candidates.size(), 0);
                std::vector<char> have(candidates.size(), 0);
                const std::function<void(const std::vector<std::size_t>&)> prefetch =
                    [&](const std::vector<std::size_t>& list) {
                        std::vector<EstimateJob> jobs;
                        jobs.reserve(list.size());
                        for (std::size_t i : list) {
                            const TeamMovementState& node = movements.states[candidates[i].stateIndex];
                            jobs.push_back(ActionJob(node.state, node.episode, candidates[i].action, true, true,
                                                     raceRanking));
                        }
                        RunJobs(jobs);
                        for (std::size_t k = 0; k < list.size(); ++k) {
                            prefetched[list[k]] = jobs[k].value;
                            have[list[k]] = 1;
                        }
                    };
                const std::vector<std::size_t> chosen = DiverseBeam(cheap, kinds, [&](std::size_t i) {
                    if (have[i]) return prefetched[i];
                    const TeamMovementState& node = movements.states[candidates[i].stateIndex];
                    return ActionEstimate(node.state, node.episode, candidates[i].action, true, true, raceRanking);
                }, maximize, count, &kept, options.selectiveStateDiversity ? &groups : nullptr,
                   &prefetch);
                if (diag && diagState >= 0) {
                    long at = -1;
                    for (std::size_t i = 0; i < candidates.size(); ++i)
                        if (candidates[i].stateIndex == static_cast<std::size_t>(diagState) &&
                            candidates[i].action.type == gNodeDiag.action.type &&
                            candidates[i].action.target == gNodeDiag.action.target &&
                            (candidates[i].action.type != ActionType::Attack ||
                             candidates[i].action.player == gNodeDiag.action.player)) at = static_cast<long>(i);
                    if (at < 0) std::fprintf(stderr, "diag: target action not among the candidates (%zu)\n", candidates.size());
                    else {
                        int sameKind = 0, betterKind = 0, betterAll = 0;
                        for (std::size_t i = 0; i < candidates.size(); ++i) {
                            const bool b = maximize ? cheap[i] > cheap[at] : cheap[i] < cheap[at];
                            if (b) ++betterAll;
                            if (kinds[i] == kinds[at]) { ++sameKind; if (b) ++betterKind; }
                        }
                        bool chosenTarget = false;
                        double worstKept = maximize ? 2 : -2;
                        for (std::size_t k = 0; k < chosen.size(); ++k) {
                            if (static_cast<long>(chosen[k]) == at) chosenTarget = true;
                            worstKept = maximize ? std::min(worstKept, kept[k]) : std::max(worstKept, kept[k]);
                        }
                        const TeamMovementState& node = movements.states[candidates[at].stateIndex];
                        const double detailed = ActionEstimate(node.state, node.episode, candidates[at].action, true, true, raceRanking);
                        std::fprintf(stderr, "diag: target decision %s; cheap %.4f rank %d of %zu, in its kind %d rank %d of %d; detailed %.4f, worst kept %.4f\n",
                                     chosenTarget ? "CHOSEN" : "dropped", cheap[at], betterAll + 1, candidates.size(),
                                     kinds[at], betterKind + 1, sameKind, detailed, worstKept);
                        for (std::size_t k = 0; k < chosen.size(); ++k) {
                            const TeamMovementState& n2 = movements.states[candidates[chosen[k]].stateIndex];
                            std::string line;
                            for (int m = 0; m < n2.count; ++m) line += FormatAction(n2.state, n2.moves[m]) + " ";
                            std::fprintf(stderr, "diag:   kept %zu kind %d %.4f: %s%s\n", k + 1, kinds[chosen[k]], kept[k], line.c_str(),
                                         FormatAction(n2.state, candidates[chosen[k]].action).c_str());
                        }
                    }
                }
                std::vector<Candidate> beam;
                for (std::size_t k = 0; k < chosen.size(); ++k) {
                    beam.push_back(candidates[chosen[k]]);
                    beam.back().estimate = kept[k];
                }
                // Ball first (06.11): the root tries the ball actions of the unmoved
                // state against each other (its first actions), an inner node only
                // against the same actions after movements, which the static ranks
                // higher. The best `selectiveBallFirstExtra` of them by the detailed
                // estimate join the beam beyond its width.
                if (options.selectiveBallFirstExtra > 0 && episodeExtensions == 0) {
                    std::vector<std::pair<double, std::size_t>> first;
                    for (std::size_t i = 0; i < candidates.size(); ++i)
                        if (movements.states[candidates[i].stateIndex].count == 0 &&
                            candidates[i].action.type != ActionType::EndTurn &&
                            std::find(chosen.begin(), chosen.end(), i) == chosen.end())
                            first.push_back({cheap[i], i});
                    std::stable_sort(first.begin(), first.end(), [&](const auto& a, const auto& b) {
                        return maximize ? a.first > b.first : a.first < b.first;
                    });
                    if (first.size() > static_cast<std::size_t>(2 * options.selectiveBallFirstExtra))
                        first.resize(2 * options.selectiveBallFirstExtra);
                    for (auto& f : first) {
                        const TeamMovementState& node = movements.states[candidates[f.second].stateIndex];
                        f.first = have[f.second] ? prefetched[f.second]
                                                 : ActionEstimate(node.state, node.episode, candidates[f.second].action,
                                                                  true, true, raceRanking);
                    }
                    std::stable_sort(first.begin(), first.end(), [&](const auto& a, const auto& b) {
                        return maximize ? a.first > b.first : a.first < b.first;
                    });
                    for (std::size_t k = 0; k < first.size() && k < static_cast<std::size_t>(options.selectiveBallFirstExtra); ++k) {
                        beam.push_back(candidates[first[k].second]);
                        beam.back().estimate = first[k].first;
                    }
                    std::stable_sort(beam.begin(), beam.end(), [&](const Candidate& a, const Candidate& b) {
                        if (a.estimate != b.estimate) return maximize ? a.estimate > b.estimate : a.estimate < b.estimate;
                        return a.order < b.order;
                    });
                }
                // Defence forced (09.11): every tackle, and the end of the turn of a
                // state with a player on the ball's path, joins the beam.
                if (forcedDefence) {
                    bool added = false;
                    for (std::size_t i = 0; i < candidates.size(); ++i) {
                        const Candidate& c = candidates[i];
                        const bool tackle = c.action.type == ActionType::Attack;
                        const bool intercept = c.action.type == ActionType::EndTurn && !forcedCells.empty() &&
                                               forcedStateOf[c.stateIndex];
                        if (!tackle && !intercept) continue;
                        if (std::any_of(beam.begin(), beam.end(), [&](const Candidate& b) {
                                return b.order == c.order; })) continue;
                        const TeamMovementState& node = movements.states[c.stateIndex];
                        beam.push_back(c);
                        beam.back().estimate = have[i] ? prefetched[i]
                                                        : ActionEstimate(node.state, node.episode, c.action, true, true,
                                                                         raceRanking);
                        ++stats.defenceForcedKept;
                        added = true;
                    }
                    if (added)
                        std::stable_sort(beam.begin(), beam.end(), [&](const Candidate& a, const Candidate& b) {
                            if (a.estimate != b.estimate) return maximize ? a.estimate > b.estimate : a.estimate < b.estimate;
                            return a.order < b.order;
                        });
                }
                count = beam.size();
                std::copy(beam.begin(), beam.end(), candidates.begin());
            } else {
                const std::size_t shortlist = std::min(candidates.size(),
                    static_cast<std::size_t>(BeamWidth()) * 2);
                std::partial_sort(candidates.begin(), candidates.begin() + shortlist,
                                  candidates.end(), betterCandidate);
                for (std::size_t i = 0; i < shortlist; ++i) {
                    Candidate& candidate = candidates[i];
                    const TeamMovementState& node = movements.states[candidate.stateIndex];
                    candidate.estimate = ActionEstimate(node.state, node.episode,
                                                        candidate.action, true, true);
                }
                std::sort(candidates.begin(), candidates.begin() + shortlist, betterCandidate);
            }
            stats.selectiveActionDrops += static_cast<long>(candidates.size() - count);
            // The leaves one turn ahead (depth 1: an End Turn ends the search's
            // horizon) are estimated together first; the search below then
            // finds them in the caches.
            if (depth == 1 && pool) {
                std::vector<GameState> leaves;
                std::vector<GoalEpisode> leafEpisodes;
                leaves.reserve(4 * count);
                for (std::size_t i = 0; i < count; ++i) {
                    const Candidate& candidate = candidates[i];
                    if (candidate.action.type != ActionType::EndTurn) continue;
                    const TeamMovementState& node = movements.states[candidate.stateIndex];
                    for (const Outcome& outcome : OutcomesUnchecked(node.state, candidate.action)) {
                        GameState child = node.state;
                        EventLog events;
                        ApplyUnchecked(child, candidate.action, outcome, &events);
                        leafEpisodes.push_back(AdvanceGoalEpisode(node.episode, node.state, child, events));
                        leaves.push_back(child);
                    }
                }
                std::vector<EstimateJob> jobs;
                for (std::size_t i = 0; i < leaves.size(); ++i)
                    if (!Terminal(leafEpisodes[i].status) && leaves[i].phase == Phase::Play)
                        jobs.push_back(StateJob(leaves[i], leafEpisodes[i], true, true, false, true));
                if (jobs.size() >= kMinParallelBatch) RunJobs(jobs);
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (!Budget()) { complete = false; break; }
                const Candidate& candidate = candidates[i];
                const TeamMovementState& node = movements.states[candidate.stateIndex];
                // Without alpha-beta every kept candidate is solved exactly.
                const GoalValueBounds value = ActionBounds(node.state, node.episode,
                    candidate.action, depth, windowed ? alpha : -1, windowed ? beta : 1);
                remember(node, &candidate.action, value);
                if (!consider(value) || stopped) {
                    complete = false;
                    if (windowed && !stopped) openCut();
                    break;
                }
            }
            if (!stopped && options.selectiveTransposition && !turnContinuation)
                SelectiveStore(s, episode, TableDepth(depth), Intersect(base, best));
        } else if (options.groupMovements && options.completeTurns && s.phase == Phase::Play) {
            const std::size_t available = memoryBudgetBytes > liveBytes ? memoryBudgetBytes - liveBytes : 0;
            if (options.maxMemoryBytes && !Reserve(sizeof(TeamMovementState))) return base;
            if (options.maxMemoryBytes) liveBytes -= sizeof(TeamMovementState);
            const std::size_t maxStates = options.maxMemoryBytes ? available / sizeof(TeamMovementState) : 0;
            TeamMovementList movements = EnumerateTeamMovements(s, episode, maxStates,
                                                                  [this] {
                                                                      if (!TakeExpansion()) return false;
                                                                      ++stats.moveExpansions;
                                                                      return true;
                                                                  }, options.frozenPlayers);
            MemoryScope movementFrame(*this, movements.states.capacity() * sizeof(TeamMovementState));
            if (!movementFrame.admitted) return base;
            if (!movements.complete) {
                if (Budget()) {
                    memoryLimited = true;
                    stopped = true;
                    if (sharedStop) sharedStop->store(true, std::memory_order_relaxed);
                }
                return base;
            }
            for (std::size_t movementIndex = 0; movementIndex < movements.states.size(); ++movementIndex) {
                const TeamMovementState& node = movements.states[movementIndex];
                if (!Budget()) { complete = false; break; }
                if (Terminal(node.episode.status)) {
                    if (!consider(Bounds(node.state, node.episode))) { complete = false; break; }
                    continue;
                }
                std::vector<Action> actions = LegalActions(node.state);
                MemoryScope actionFrame(*this, actions.capacity() * sizeof(Action) + 128);
                if (!actionFrame.admitted) { complete = false; break; }
                std::stable_sort(actions.begin(), actions.end(), [&](const Action& a, const Action& b) {
                    return ActionOrder(node.state, a) < ActionOrder(node.state, b);
                });
                for (const Action& action : actions) {
                    if (action.type == ActionType::Move) continue;
                    if (!Budget()) { complete = false; break; }
                    const GoalValueBounds value = ActionBounds(node.state, node.episode, action,
                                                                depth, alpha, beta);
                    if (!consider(value) || stopped) { complete = false; break; }
                }
                if (!complete) break;
            }
        } else {
            std::vector<Action> actions = LegalActions(s);
            MemoryScope frame(*this, actions.capacity() * sizeof(Action) + 128);
            if (!frame.admitted) return base;
            std::stable_sort(actions.begin(), actions.end(), [&](const Action& a, const Action& b) {
                return ActionOrder(s, a) < ActionOrder(s, b);
            });
            for (const Action& action : actions) {
                if (!Budget()) { complete = false; break; }
                const int actionDepth = options.completeTurns ? depth : depth - 1;
                const GoalValueBounds value = ActionBounds(s, episode, action, actionDepth, alpha, beta);
                if (!consider(value) || stopped) { complete = false; break; }
            }
        }
        if (!complete && options.selectiveWidth == 0) {
            best.lower = maximize ? std::max(best.lower, base.lower) : std::min(best.lower, base.lower);
            best.upper = maximize ? std::max(best.upper, base.upper) : std::min(best.upper, base.upper);
        }
        best = Intersect(base, best);
        if (options.selectiveWidth == 0) Store(s, episode, best);
        return best;
    }

    // ------------------------------------------------------------------
    // Best-first tree over the selective candidates (7.68).
    //
    // A decision node is a state in which a team decides: one candidate is a
    // set of its movements (a kept movement state of the beam) and one ball
    // action or EndTurn after them - exactly the candidates of the fixed-depth
    // selective tree, generated by the same beam. The engine's outcomes of
    // the action lead to the next decision nodes (the same team in the middle
    // of its turn, or the opponent). Values are those of the selective tree:
    // the episode team's goal objective, a maximum at its own nodes, a minimum
    // at the other team's, an expectation over the outcomes. A new node or an
    // outcome not yet searched is worth its static estimate. Which candidate
    // grows is decided by PUCT with the prior from the candidates' estimates
    // and progressive widening; which outcome by its probability.
    struct TreeNode;
    struct TreeBranch {
        Outcome outcome;
        double p = 0;
        GoalEpisode next;
        double estimate = 0;
        bool fixed = false;   // terminal, an episode exit, a rare outcome or no play: static for good
        int visits = 0;
        TreeNode* node = nullptr;   // owned by the pool; shared by every way to its state
    };
    struct TreeEdge {
        Action moves[kPlayersPerTeam]{};
        int count = 0;
        Action action = Action::EndTurn();
        GoalEpisode episode;   // after the movements
        bool terminalMoves = false;   // the movements end the episode: no action follows
        int category = -1;            // of the ball (AddBallCategoryEdges), -1: the team has no ball
        double estimate = 0, prior = 0, value = 0;
        int visits = 0;
        bool expanded = false, fixed = false;
        std::vector<TreeBranch> branches;
    };
    struct TreeNode {
        GameState s;
        GoalEpisode episode;
        bool maximize = true;
        unsigned outside = 0;   // outsiders of the node's attack episode: the ball reaching one ends the line
        double estimate = 0, value = 0;
        int visits = 0;
        int open = 0;
        bool expanded = false, fixed = false;
        bool elementary = false;   // the root's single actions are among the candidates
        std::vector<TreeEdge> edges;
    };
    // The nodes are owned by the pool and found by the table: a state reached
    // by different ways (one player's move and then the rest of the turn, or
    // the whole turn at once; other orders of the same decisions) is one node,
    // as in the transposition table of the beam tree. The value of a node is a
    // function of its state and episode only, so sharing it changes nothing
    // but the work. States along a line never repeat (the turn and the moves
    // made in it only go forward), so the graph has no cycles.
    TreeNode* treeRoot = nullptr;
    std::vector<std::unique_ptr<TreeNode>> treePool;
    std::unordered_map<uint64_t, std::vector<TreeNode*>> treeTable;
    long treeNodes = 0;
    long treeTranspositions = 0;   // links to a node that already existed, in this search
    long treeRepeats = 0;          // without transpositions: new nodes of a state already in the tree

    std::size_t TreeWidth() const {
        return static_cast<std::size_t>(options.selectiveTreeWidth > 0 ? options.selectiveTreeWidth
                                                                       : 2 * options.selectiveWidth);
    }

    // All candidates are open by default: their estimates are known from the
    // beam anyway, and with progressive widening a node deeper in the tree
    // (fewer visits) would weigh fewer candidates than its parent, which
    // biases every backed-up value towards the side moving higher up (7.68).
    int TreeOpen(const TreeNode& n) const {
        if (options.selectiveTreeWidenBase <= 0) return static_cast<int>(n.edges.size());
        const double k = options.selectiveTreeWidenBase *
                         std::pow(double(n.visits + 1), options.selectiveTreeWidenExponent);
        return std::max(1, std::min(static_cast<int>(n.edges.size()), static_cast<int>(k)));
    }

    // The state before the edge's action: the node's state after its movements.
    GameState TreePreAction(const TreeNode& n, const TreeEdge& e) const {
        GameState t = n.s;
        for (int i = 0; i < e.count; ++i) ApplyUnchecked(t, e.moves[i], Outcome::Certain());
        return t;
    }

    // ------------------------------------------------------------------
    // Candidates of the team with the ball by what happens to the ball this
    // turn (7.69): 0 - the holder keeps it, 1 - he shoots, 2..4 - a pass to the
    // first, second or third partner (team order, the holder left out); 5 -
    // anything else (a single move with the decision left to the next node, a
    // pass the opponent reaches first). Inside each category its own best
    // moves are found, the tree compares the categories.
    static constexpr int kTreeCategories = 6;

    bool BallTeamToMove(const GameState& s) const {
        return s.phase == Phase::Play && !s.ball.IsFree() && TeamOf(s.ball.holder) == s.teamToMove;
    }

    // In how many turns each player can stand on a cell, on one clock: an own
    // player who has not moved gets there this turn (1), the opponents in
    // their next turn (1, which comes after ours), an own player who has moved
    // only from his next turn on (as SupportValue counts).
    struct TreeReach {
        std::array<ReachMap, kNumPlayers> map;
        std::array<int, kNumPlayers> offset{};
        int Turns(int k, Cell c) const {
            const int t = map[k].Turns(c);
            if (t == kUnreachable) return kUnreachable;
            return t == 0 ? 0 : t + offset[k];
        }
    };
    TreeReach ReachOf(const GameState& s) const {
        TreeReach r;
        for (int k = 0; k < kNumPlayers; ++k) {
            const PlayerState& p = s.players[k];
            const bool fromNext = TeamOf(k) != s.teamToMove || p.moved;
            r.map[k] = ComputeReach(p.pos, fromNext ? p.Displacement() : p.Velocity());
            r.offset[k] = TeamOf(k) == s.teamToMove && p.moved ? 1 : 0;
        }
        return r;
    }

    // The partner a pass to `target` is for: the own player (not the holder)
    // who can stand there first, if no opponent is there earlier; -1 if one is.
    int PassReceiver(const GameState& s, Cell target, const TreeReach& reach) const {
        const int own = FirstPlayerOf(s.teamToMove), other = FirstPlayerOf(Opponent(s.teamToMove));
        int best = -1, bestTurns = kUnreachable;
        for (int k = own; k < own + kPlayersPerTeam; ++k) {
            if (k == s.ball.holder) continue;
            const int t = s.players[k].pos == target ? 0 : reach.Turns(k, target);
            if (t < bestTurns) { best = k; bestTurns = t; }
        }
        if (best < 0 || bestTurns == kUnreachable) return -1;
        for (int k = other; k < other + kPlayersPerTeam; ++k)
            if (reach.Turns(k, target) < bestTurns) return -1;
        return best;
    }

    int PartnerIndex(const GameState& s, int player) const {
        int index = 0;
        for (int k = FirstPlayerOf(s.teamToMove); k < player; ++k)
            if (k != s.ball.holder) ++index;
        return index;
    }

    // The category of `action` in `pre` (the state before it); -1 when the
    // team to move has no ball.
    int BallCategory(const GameState& pre, const Action& action, const TreeReach& reach) const {
        if (!BallTeamToMove(pre)) return -1;
        switch (action.type) {
            case ActionType::Shot: return 1;
            case ActionType::Pass: {
                const int receiver = PassReceiver(pre, action.target, reach);
                return receiver < 0 ? 5 : 2 + PartnerIndex(pre, receiver);
            }
            case ActionType::Move: return 5;
            default: return 0;
        }
    }

    // The best moves of each category: the holder's run before the action
    // (or none) and, for a pass to a partner's feet, the partner's step before
    // it; a pass into space goes to a cell the partner reaches first. Other
    // players do not move here: after a pass the team's next node decides
    // them, the attack episode's outsiders take their policy moves first as in
    // every movement state. Ranked by the cheap expectation over the exact
    // outcomes, the best few of each category by the detailed one; `quota`
    // per category with different targets join the node's candidates.
    void AddBallCategoryEdges(const TreeNode& n, const TeamMovementState& origin,
                              std::vector<TreeEdge>& edges) {
        const GameState& s0 = origin.state;
        if (!BallTeamToMove(s0) || s0.ballPlayed || Terminal(origin.episode.status)) return;
        const unsigned outsiders = options.selectiveAttackEpisode ? n.outside : 0;
        const int holder = s0.ball.holder;
        const TreeReach reach = ReachOf(s0);
        struct Option { double cheap; int category; GameState pre; GoalEpisode episode;
                        Action moves[kPlayersPerTeam]; int count; Action action; };
        std::vector<Option> options_;
        const auto add = [&](const GameState& pre, GoalEpisode e, const Action* moves, int count,
                             const Action& a, int category) {
            if (Validate(pre, a) != ActionError::None) return;
            Option o;
            // A first cut for the ball team, before any estimate: the chance
            // the pass gets through times what the ball is worth there - the
            // shot from there or the possession there, whichever is more.
            const int team = pre.teamToMove;
            o.cheap = a.type == ActionType::Shot ? ShotProbability(pre, holder)
                : PassThroughProbability(pre, a.target) *
                  std::max(OpenShotProbability(a.target, team), HoldValue(pre, a.target, team));
            o.category = category;
            o.pre = pre;
            o.episode = e;
            o.count = count;
            for (int i = 0; i < count; ++i) o.moves[i] = moves[i];
            o.action = a;
            options_.push_back(o);
        };
        std::vector<Cell> holderCells{s0.players[holder].pos};
        if (!s0.players[holder].moved && !(outsiders >> holder & 1u))
            for (Cell c : MoveDestinations(s0, holder))
                if (c != s0.players[holder].pos) holderCells.push_back(c);
        const int first = FirstPlayerOf(s0.teamToMove);
        for (Cell hc : holderCells) {
            if (!Budget()) return;
            GameState a = s0;
            GoalEpisode ea = origin.episode;
            Action moves[kPlayersPerTeam];
            int count = origin.count;
            for (int i = 0; i < count; ++i) moves[i] = origin.moves[i];
            if (hc != a.players[holder].pos) {
                EventLog events;
                const GameState before = a;
                const Action m = Action::Move(holder, hc);
                if (Validate(a, m) != ActionError::None) continue;
                ApplyUnchecked(a, m, Outcome::Certain(), &events);
                ea = AdvanceGoalEpisode(ea, before, a, events);
                if (Terminal(ea.status) || a.ball.holder != holder) continue;
                moves[count++] = m;
            }
            add(a, ea, moves, count, Action::Shot(), 1);
            // Passes into space and to a partner standing still.
            for (const Action& pass : LegalActions(a)) {
                if (pass.type != ActionType::Pass) continue;
                const int receiver = PassReceiver(a, pass.target, reach);
                if (receiver < 0) continue;
                add(a, ea, moves, count, pass, 2 + PartnerIndex(a, receiver));
            }
            // A pass to the feet of a partner after his step.
            for (int k = first; k < first + kPlayersPerTeam; ++k) {
                if (k == holder || a.players[k].moved || (outsiders >> k & 1u)) continue;
                for (Cell pc : MoveDestinations(a, k)) {
                    if (pc == a.players[k].pos) continue;
                    GameState b = a;
                    EventLog events;
                    const Action m = Action::Move(k, pc);
                    ApplyUnchecked(b, m, Outcome::Certain(), &events);
                    const GoalEpisode eb = AdvanceGoalEpisode(ea, a, b, events);
                    if (Terminal(eb.status) || !BallTeamToMove(b)) continue;
                    Action more[kPlayersPerTeam];
                    for (int i = 0; i < count; ++i) more[i] = moves[i];
                    more[count] = m;
                    add(b, eb, more, count + 1, Action::Pass(pc), 2 + PartnerIndex(b, k));
                }
            }
        }
        const bool maximize = n.maximize;
        const auto better = [maximize](double x, double y) { return maximize ? x > y : x < y; };
        const std::size_t quota[kTreeCategories] = {0, 2, 3, 3, 3, 0};
        const std::size_t shortlist[kTreeCategories] = {0, 3, 8, 8, 8, 0};
        for (int category = 1; category <= 4; ++category) {
            std::vector<std::size_t> order;
            for (std::size_t i = 0; i < options_.size(); ++i)
                if (options_[i].category == category) order.push_back(i);
            std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
                return options_[x].cheap > options_[y].cheap;   // for the ball team
            });
            if (order.size() > shortlist[category]) order.resize(shortlist[category]);
            std::vector<double> detailed;
            for (std::size_t i : order) {
                const Option& o = options_[i];
                detailed.push_back(ActionEstimate(o.pre, o.episode, o.action, true, true));
            }
            std::vector<std::size_t> rank(order.size());
            for (std::size_t i = 0; i < rank.size(); ++i) rank[i] = i;
            std::stable_sort(rank.begin(), rank.end(), [&](std::size_t x, std::size_t y) {
                return better(detailed[x], detailed[y]);
            });
            std::vector<Cell> targets;
            std::size_t taken = 0;
            for (std::size_t r : rank) {
                if (taken == quota[category]) break;
                const Option& o = options_[order[r]];
                const Cell target = o.action.type == ActionType::Pass ? o.action.target : o.pre.ball.pos;
                if (std::find(targets.begin(), targets.end(), target) != targets.end()) continue;
                // Already a candidate of the beam: the same state before the same action.
                bool known = false;
                for (const TreeEdge& e : edges) {
                    if (e.terminalMoves || e.action.type != o.action.type || !(e.action.target == o.action.target))
                        continue;
                    if (SameSearchState(TreePreAction(n, e), o.pre)) { known = true; break; }
                }
                targets.push_back(target);
                if (known) continue;
                TreeEdge e;
                e.count = o.count;
                for (int i = 0; i < o.count; ++i) e.moves[i] = o.moves[i];
                e.action = o.action;
                e.episode = o.episode;
                e.estimate = e.value = detailed[r];
                e.category = category;
                edges.push_back(std::move(e));
                ++taken;
                ++stats.selectiveCategoryEdges;
            }
        }
    }

    // The candidates of a node: the selective beam of Solve, with
    // `TreeWidth()` ball actions kept instead of `selectiveWidth`. False when
    // the budget ran out.
    bool ExpandTreeNode(TreeNode& n) {
        ScopedTreeTimer timer(options.profileTree ? &treeProfile.nodeNs : nullptr);
        ProfileStageScope stage(*this, 1);
        if (options.profileTree) ++treeProfile.nodeCalls;
        struct EpisodeScope {
            Impl& owner;
            const AttackEpisode* episode;
            unsigned mask;
            EpisodeScope(Impl& o) : owner(o), episode(o.nodeEpisode), mask(o.exitMask) {}
            ~EpisodeScope() { owner.nodeEpisode = episode; owner.exitMask = mask; }
        } episodeScope(*this);
        AttackEpisode attackEpisode;
        nodeEpisode = nullptr;
        exitMask = 0;
        if (options.selectiveAttackEpisode) {
            attackEpisode = AnalyzeAttackEpisode(n.s, false, options.selectiveEpisodeBallRace);
            nodeEpisode = &attackEpisode;
            exitMask = attackEpisode.outside;
            n.outside = attackEpisode.outside;
        }
        const bool maximize = n.maximize;
        TeamMovementList movements = [&] {
            ScopedTreeTimer movementTimer(options.profileTree ? &treeProfile.movementNs : nullptr);
            ProfileStageScope movementStage(*this, 2);
            return SelectiveMovements(n.s, n.episode);
        }();
        if (!movements.complete) return false;
        ++positions;
        using RankedState = std::pair<double, std::size_t>;
        std::vector<RankedState> rankedStates;
        rankedStates.reserve(movements.states.size());
        {
            ProfileStageScope rankingStage(*this, 6);
            for (std::size_t i = 0; i < movements.states.size(); ++i) {
                if (!Budget()) return false;
                const TeamMovementState& node = movements.states[i];
                double estimate = Estimate(node.state, node.episode, true, false, true);
                if (options.selectiveEndTurnPreview && !Terminal(node.episode.status)) {
                    const double ended = ActionEstimate(node.state, node.episode, Action::EndTurn(), true);
                    estimate = maximize ? std::max(estimate, ended) : std::min(estimate, ended);
                }
                rankedStates.push_back({estimate, i});
            }
        }
        auto better = [maximize](const auto& a, const auto& b) {
            if (a.first != b.first) return maximize ? a.first > b.first : a.first < b.first;
            return a.second < b.second;
        };
        // As many movement states as candidates: the defending side's
        // candidates are its movement states (EndTurn, an attack), and with
        // fewer of them than the attack has candidates the tree would favour
        // the attack in every deep line.
        const std::size_t stateCount = std::min(rankedStates.size(), TreeWidth());
        if (options.selectiveBallDiversity) {
            std::sort(rankedStates.begin(), rankedStates.end(), better);
            std::vector<RankedState> chosen;
            for (const RankedState& ranked : rankedStates) {
                const GameState& candidate = movements.states[ranked.second].state;
                bool represented = false;
                for (const RankedState& prior : chosen) {
                    const GameState& existing = movements.states[prior.second].state;
                    if (candidate.ball.holder == existing.ball.holder &&
                        candidate.ball.pos == existing.ball.pos) {
                        represented = true;
                        break;
                    }
                }
                if (!represented) chosen.push_back(ranked);
                if (chosen.size() == stateCount) break;
            }
            for (const RankedState& ranked : rankedStates) {
                if (chosen.size() == stateCount) break;
                if (std::none_of(chosen.begin(), chosen.end(), [&](const RankedState& prior) {
                        return prior.second == ranked.second;
                    })) chosen.push_back(ranked);
            }
            rankedStates = std::move(chosen);
        } else {
            std::partial_sort(rankedStates.begin(), rankedStates.begin() + stateCount,
                              rankedStates.end(), better);
            rankedStates.resize(stateCount);
        }
        if (stateCount && std::none_of(rankedStates.begin(), rankedStates.end(),
                                      [](const auto& entry) { return entry.second == 0; }))
            rankedStates.back() = {Estimate(n.s, n.episode), 0};

        auto edgeOf = [&](const TeamMovementState& node) {
            TreeEdge e;
            e.count = node.count;
            for (int i = 0; i < node.count; ++i) e.moves[i] = node.moves[i];
            e.episode = node.episode;
            return e;
        };
        std::vector<TreeEdge> edges;
        ProfileStageScope actionStage(*this, 7);
        struct Candidate { double estimate; std::size_t stateIndex; Action action; };
        std::vector<Candidate> candidates;
        for (const RankedState& ranked : rankedStates) {
            const TeamMovementState& node = movements.states[ranked.second];
            if (Terminal(node.episode.status)) {
                TreeEdge e = edgeOf(node);
                e.terminalMoves = true;
                e.expanded = e.fixed = true;
                e.estimate = e.value = Estimate(node.state, node.episode, true, true);
                edges.push_back(std::move(e));
                continue;
            }
            for (const Action& action : LegalActions(node.state)) {
                if (action.type == ActionType::Move) continue;
                const bool detailed = options.selectiveEndTurnPreview && action.type == ActionType::EndTurn;
                candidates.push_back({ActionEstimate(node.state, node.episode, action, detailed),
                                      ranked.second, action});
            }
            if (!Budget()) return false;
        }
        const std::size_t width = TreeWidth();
        std::vector<std::size_t> chosen;
        std::vector<double> kept;
        if (options.selectiveKindDiversity) {
            std::vector<double> cheap;
            std::vector<int> kinds;
            for (const Candidate& c : candidates) {
                cheap.push_back(c.estimate);
                kinds.push_back(DecisionKind(movements.states[c.stateIndex].state, c.action));
            }
            chosen = DiverseBeam(cheap, kinds, [&](std::size_t i) {
                const TeamMovementState& node = movements.states[candidates[i].stateIndex];
                return ActionEstimate(node.state, node.episode, candidates[i].action, true, true);
            }, maximize, std::min(width, candidates.size()), &kept);
        } else {
            std::vector<std::size_t> order(candidates.size());
            for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
            const auto cheaper = [&](std::size_t a, std::size_t b) {
                const double x = candidates[a].estimate, y = candidates[b].estimate;
                if (x != y) return maximize ? x > y : x < y;
                return a < b;
            };
            const std::size_t shortlist = std::min(order.size(), 2 * width);
            std::partial_sort(order.begin(), order.begin() + shortlist, order.end(), cheaper);
            order.resize(shortlist);
            std::vector<double> detailed(candidates.size(), 0);
            for (std::size_t i : order) {
                const TeamMovementState& node = movements.states[candidates[i].stateIndex];
                detailed[i] = ActionEstimate(node.state, node.episode, candidates[i].action, true, true);
            }
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return maximize ? detailed[a] > detailed[b] : detailed[a] < detailed[b];
            });
            order.resize(std::min(order.size(), width));
            chosen = order;
            for (std::size_t i : chosen) kept.push_back(detailed[i]);
        }
        stats.selectiveActionDrops += static_cast<long>(candidates.size() - chosen.size());
        for (std::size_t k = 0; k < chosen.size(); ++k) {
            const Candidate& c = candidates[chosen[k]];
            TreeEdge e = edgeOf(movements.states[c.stateIndex]);
            e.action = c.action;
            e.estimate = e.value = kept[k];
            edges.push_back(std::move(e));
        }
        if (options.selectiveTreeBallCategories) {
            ScopedTreeTimer categoryTimer(options.profileTree ? &treeProfile.categoryNs : nullptr);
            ProfileStageScope categoryStage(*this, 3);
            // From the start of the node's own decision: its outsiders' policy moves made.
            AddBallCategoryEdges(n, movements.states.front(), edges);
            const TreeReach reach = ReachOf(n.s);
            for (TreeEdge& e : edges)
                if (e.category < 0 && !e.terminalMoves)
                    e.category = BallCategory(TreePreAction(n, e), e.action, reach);
        }
        if (options.selectiveTreeTackleSlot) {
            // A defender's move into tackle range can be lost by the movement
            // beam even though the root's elementary moves would find it.
            // Keep one legal tackle in the existing decision width at every node.
            const TeamMovementState& origin = movements.states.front();
            if (!origin.state.ball.IsFree() &&
                TeamOf(origin.state.ball.holder) != origin.state.teamToMove) {
                bool found = false;
                double bestChance = -1;
                int bestPlayer = kNoPlayer;
                Cell bestCell{};
                GameState bestPre;
                const int first = FirstPlayerOf(origin.state.teamToMove);
                for (int k = first; k < first + kPlayersPerTeam; ++k) {
                    if (origin.state.players[k].attacked) continue;
                    std::vector<Cell> cells{origin.state.players[k].pos};
                    if (!origin.state.players[k].moved)
                        for (Cell c : MoveDestinations(origin.state, k))
                            if (c != origin.state.players[k].pos) cells.push_back(c);
                    for (Cell c : cells) {
                        GameState pre = origin.state;
                        if (c != origin.state.players[k].pos)
                            ApplyUnchecked(pre, Action::Move(k, c), Outcome::Certain());
                        if (GetAttackKind(pre, k) != AttackKind::Tackle) continue;
                        const double chance = TackleChance(pre.ball.pos, c, pre.teamToMove);
                        if (!found || chance > bestChance) {
                            found = true;
                            bestChance = chance;
                            bestPlayer = k;
                            bestCell = c;
                            bestPre = pre;
                        }
                    }
                }
                if (found) {
                    bool known = false;
                    for (const TreeEdge& existing : edges)
                        if (!existing.terminalMoves && existing.action.type == ActionType::Attack &&
                            existing.action.player == bestPlayer &&
                            SameSearchState(TreePreAction(n, existing), bestPre)) {
                            known = true;
                            break;
                        }
                    if (!known) {
                        TreeEdge e;
                        e.episode = origin.episode;
                        e.count = origin.count;
                        for (int i = 0; i < origin.count; ++i) e.moves[i] = origin.moves[i];
                        if (bestCell != origin.state.players[bestPlayer].pos)
                            e.moves[e.count++] = Action::Move(bestPlayer, bestCell);
                        e.action = Action::Attack(bestPlayer);
                        // The complete chance branch is evaluated only if PUCT
                        // visits this edge; the prior costs one static estimate.
                        e.estimate = e.value = Estimate(bestPre, e.episode);
                        if (edges.size() >= TreeWidth() && !edges.empty()) {
                            std::size_t worst = 0;
                            for (std::size_t i = 1; i < edges.size(); ++i)
                                if (n.maximize ? edges[i].estimate < edges[worst].estimate
                                               : edges[i].estimate > edges[worst].estimate)
                                    worst = i;
                            edges[worst] = std::move(e);
                        } else edges.push_back(std::move(e));
                    }
                }
            }
        }
        if (!Budget()) return false;
        n.edges = std::move(edges);
        SortTreeEdges(n);
        treeBytes += n.edges.capacity() * sizeof(TreeEdge);
        return true;
    }

    // Best first for the team to move; the prior is the softmax of the estimates.
    void SortTreeEdges(TreeNode& n) const {
        const bool maximize = n.maximize;
        std::stable_sort(n.edges.begin(), n.edges.end(), [maximize](const TreeEdge& a, const TreeEdge& b) {
            return maximize ? a.estimate > b.estimate : a.estimate < b.estimate;
        });
        // With the ball's categories the prior is shared equally by the
        // categories present and by the softmax inside each: the static's
        // errors must not starve a whole category before the tree has seen it.
        const auto categoryOf = [&](const TreeEdge& e) {
            return options.selectiveTreeBallCategories && e.category >= 0 ? e.category : kTreeCategories;
        };
        double top[kTreeCategories + 1], sum[kTreeCategories + 1];
        for (int c = 0; c <= kTreeCategories; ++c) top[c] = -2, sum[c] = 0;
        for (const TreeEdge& e : n.edges)
            top[categoryOf(e)] = std::max(top[categoryOf(e)], maximize ? e.estimate : -e.estimate);
        for (TreeEdge& e : n.edges)
            sum[categoryOf(e)] += e.prior = std::exp(((maximize ? e.estimate : -e.estimate) - top[categoryOf(e)]) /
                                                     options.selectiveTreeTemperature);
        int present = 0;
        for (int c = 0; c <= kTreeCategories; ++c) if (sum[c] > 0) ++present;
        for (TreeEdge& e : n.edges) e.prior /= sum[categoryOf(e)] * present;
    }

    // The root's single actions, as the root of the beam tree ranks them: a
    // move of one player (the rest of the turn is then decided in the child,
    // the same team's node), a ball action or EndTurn. The movement states of
    // a node are near variants of the few best ones; the single actions give
    // the root the different first steps (7.68). Outsiders of the attack
    // episode move by their policy in the child and have no single moves here.
    bool AddElementaryEdges(TreeNode& n) {
        ScopedTreeTimer timer(options.profileTree ? &treeProfile.elementaryNs : nullptr);
        ProfileStageScope stage(*this, 5);
        std::vector<Action> actions;
        const unsigned outsiders = options.selectiveAttackEpisode ? n.outside : 0;
        for (const Action& a : LegalActions(n.s))
            if (!(a.type == ActionType::Move && (outsiders >> a.player & 1u))) actions.push_back(a);
        std::vector<double> cheap;
        std::vector<int> kinds;
        for (const Action& a : actions) {
            if (!Budget()) return false;
            cheap.push_back(ActionEstimate(n.s, n.episode, a));
            kinds.push_back(DecisionKind(n.s, a));
        }
        std::vector<double> kept;
        const std::vector<std::size_t> chosen = DiverseBeam(cheap, kinds, [&](std::size_t i) {
            return ActionEstimate(n.s, n.episode, actions[i], true, true);
        }, n.maximize, std::min(TreeWidth(), actions.size()), &kept);
        if (!Budget()) return false;
        for (std::size_t k = 0; k < chosen.size(); ++k) {
            const Action& a = actions[chosen[k]];
            bool known = false;   // a ball action or EndTurn with no movement is a candidate already
            for (const TreeEdge& e : n.edges)
                if (e.count == 0 && !e.terminalMoves && e.action.type == a.type &&
                    e.action.player == a.player && e.action.target == a.target)
                    known = true;
            if (known) continue;
            TreeEdge e;
            e.action = a;
            e.episode = n.episode;
            e.estimate = e.value = kept[k];
            if (options.selectiveTreeBallCategories) e.category = BallCategory(n.s, a, ReachOf(n.s));
            n.edges.push_back(std::move(e));
        }
        n.elementary = true;
        SortTreeEdges(n);
        n.open = TreeOpen(n);
        TreeNodeValue(n);
        return true;
    }

    // The outcomes of the edge's action with their static estimates.
    void ExpandTreeEdge(const TreeNode& n, TreeEdge& e) {
        ScopedTreeTimer timer(options.profileTree ? &treeProfile.edgeNs : nullptr);
        ProfileStageScope stage(*this, 4);
        if (options.profileTree) ++treeProfile.edgeCalls;
        const GameState pre = TreePreAction(n, e);
        const unsigned savedMask = exitMask;
        exitMask = options.selectiveAttackEpisode ? n.outside : 0;
        for (const Outcome& outcome : OutcomesUnchecked(pre, e.action)) {
            ++stats.outcomesProcessed;
            GameState child = pre;
            EventLog events;
            ApplyUnchecked(child, e.action, outcome, &events);
            TreeBranch b;
            b.outcome = outcome;
            b.p = outcome.probability;
            b.next = AdvanceGoalEpisode(e.episode, pre, child, events);
            b.estimate = Estimate(child, b.next, true, true);
            bool inPlay = child.phase == Phase::Play;
            if (child.phase == Phase::Lineup && options.selectiveRestartPosition)
                inPlay = StandardRestart(child).phase == Phase::Play;
            b.fixed = Terminal(b.next.status) || !inPlay || EpisodeExit(child, b.next) ||
                      (options.selectiveRareOutcomeProbability > 0 &&
                       b.p < options.selectiveRareOutcomeProbability);
            maxDepth = std::max(maxDepth, child.halfTurns - rootHalfTurns);
            e.branches.push_back(std::move(b));
        }
        exitMask = savedMask;
        std::stable_sort(e.branches.begin(), e.branches.end(),
                         [](const TreeBranch& a, const TreeBranch& b) { return a.p > b.p; });
        e.expanded = true;
        treeBytes += e.branches.capacity() * sizeof(TreeBranch);
    }

    TreeNode* LookupTreeNode(const GameState& s, GoalEpisode episode) {
        const auto found = treeTable.find(Key(s, episode));
        if (found == treeTable.end()) return nullptr;
        for (TreeNode* n : found->second)
            if (n->episode.team == episode.team && n->episode.status == episode.status &&
                SameSearchState(n->s, s))
                return n;
        return nullptr;
    }

    // The node of this state: the one already in the tree (with the
    // transpositions) or a new one valued by `estimate`.
    TreeNode* TreeNodeFor(GameState s, GoalEpisode episode, double estimate) {
        ScopedTreeTimer timer(options.profileTree ? &treeProfile.nodeForNs : nullptr);
        if (s.phase == Phase::Lineup && options.selectiveRestartPosition) s = StandardRestart(s);
        if (TreeNode* known = LookupTreeNode(s, episode)) {
            if (options.selectiveTreeTranspositions) {
                ++treeTranspositions;
                return known;
            }
            ++treeRepeats;   // without transpositions: the same state searched again
        }
        std::unique_ptr<TreeNode> node(new TreeNode);
        node->s = s;
        node->episode = episode;
        node->maximize = node->s.teamToMove == episode.team;
        node->estimate = node->value = estimate;
        ++treeNodes;
        treeBytes += sizeof(TreeNode) + 3 * sizeof(void*);
        maxDepth = std::max(maxDepth, node->s.halfTurns - rootHalfTurns);
        TreeNode* raw = node.get();
        treePool.push_back(std::move(node));
        treeTable[Key(raw->s, episode)].push_back(raw);
        return raw;
    }

    // A shared node's new value reaches its other parents only when they are
    // visited again. Before the root compares its candidates, every value is
    // recomputed from the leaves up (each node once; the graph has no cycles).
    void RefreshTree(TreeNode* root) {
        std::unordered_set<const TreeNode*> done;
        std::function<void(TreeNode&)> refresh = [&](TreeNode& n) {
            if (!done.insert(&n).second) return;
            for (TreeEdge& e : n.edges) {
                for (TreeBranch& b : e.branches)
                    if (b.node) refresh(*b.node);
                e.value = TreeEdgeValue(e);
            }
            if (n.expanded) TreeNodeValue(n);
        };
        refresh(*root);
    }

    // Keeps only the nodes reachable from `root` (the rest of the old tree is
    // another game now).
    void KeepTreeFrom(TreeNode* root) {
        std::unordered_set<const TreeNode*> reachable;
        std::vector<TreeNode*> stack{root};
        while (!stack.empty()) {
            TreeNode* n = stack.back();
            stack.pop_back();
            if (!reachable.insert(n).second) continue;
            for (TreeEdge& e : n->edges)
                for (TreeBranch& b : e.branches)
                    if (b.node) stack.push_back(b.node);
        }
        std::vector<std::unique_ptr<TreeNode>> kept;
        treeTable.clear();
        treeBytes = 0;
        for (std::unique_ptr<TreeNode>& n : treePool)
            if (reachable.count(n.get())) {
                treeTable[Key(n->s, n->episode)].push_back(n.get());
                treeBytes += sizeof(TreeNode) + 3 * sizeof(void*) + n->edges.capacity() * sizeof(TreeEdge);
                for (const TreeEdge& e : n->edges) treeBytes += e.branches.capacity() * sizeof(TreeBranch);
                kept.push_back(std::move(n));
            }
        treePool = std::move(kept);
        treeNodes = static_cast<long>(treePool.size());
    }

    static double TreeEdgeValue(const TreeEdge& e) {
        if (!e.expanded || e.terminalMoves) return e.value;
        double v = 0;
        for (const TreeBranch& b : e.branches) v += b.p * (b.node ? b.node->value : b.estimate);
        return v;
    }

    static void TreeNodeValue(TreeNode& n) {
        if (n.open == 0) { n.value = n.estimate; return; }
        double v = n.maximize ? -2 : 2;
        for (int i = 0; i < n.open; ++i)
            v = n.maximize ? std::max(v, n.edges[i].value) : std::min(v, n.edges[i].value);
        n.value = v;
    }

    // One simulation: down the tree to a node not yet expanded, expand it,
    // and back the values up. False when the budget ran out.
    bool TreeSimulate(TreeNode& n) {
        if (n.fixed) return true;
        if (!n.expanded) {
            if (!ExpandTreeNode(n)) return false;
            n.expanded = true;
            ++n.visits;
            if (n.edges.empty()) {
                n.fixed = true;
                return true;
            }
            n.open = TreeOpen(n);
            TreeNodeValue(n);
            return true;
        }
        n.open = std::max(n.open, TreeOpen(n));
        TreeEdge* best = nullptr;
        for (;;) {
            double bestScore = 0;
            const double root = std::sqrt(double(n.visits + 1));
            for (int i = 0; i < n.open; ++i) {
                TreeEdge& e = n.edges[i];
                if (e.fixed) continue;
                const double q = n.maximize ? e.value : -e.value;
                const double score = q + options.selectiveTreeCPuct * e.prior * root / (1.0 + e.visits);
                if (!best || score > bestScore) {
                    best = &e;
                    bestScore = score;
                }
            }
            if (best || n.open >= static_cast<int>(n.edges.size())) break;
            ++n.open;   // every open candidate is exact: open the next one
        }
        if (!best) {
            n.fixed = true;
            TreeNodeValue(n);
            return true;
        }
        const bool ok = TreeSimulateEdge(n, *best);
        if (ok) {
            ++n.visits;
            ++best->visits;
        }
        best->value = TreeEdgeValue(*best);
        TreeNodeValue(n);
        return ok;
    }

    bool TreeSimulateEdge(const TreeNode& n, TreeEdge& e) {
        if (!e.expanded) ExpandTreeEdge(n, e);
        TreeBranch* best = nullptr;
        double bestScore = 0;
        const double total = e.visits + 1;
        for (TreeBranch& b : e.branches) {
            if (b.fixed || (b.node && b.node->fixed)) continue;
            // The outcome furthest behind its share of the visits.
            const double score = b.p - b.visits / total;
            if (!best || score > bestScore) {
                best = &b;
                bestScore = score;
            }
        }
        if (!best) {
            e.fixed = true;
            return true;
        }
        if (!best->node) {
            GameState child = TreePreAction(n, e);
            ApplyUnchecked(child, e.action, best->outcome);
            best->node = TreeNodeFor(child, best->next, best->estimate);
        }
        const bool ok = TreeSimulate(*best->node);
        if (ok) ++best->visits;
        return ok;
    }

    // The chosen candidate: the best value among those with at least the
    // final share of the most visited one's visits.
    const TreeEdge* TreeChoice(const TreeNode& n) const {
        int most = 0;
        for (int i = 0; i < n.open; ++i) most = std::max(most, n.edges[i].visits);
        const TreeEdge* chosen = nullptr;
        for (int i = 0; i < n.open; ++i) {
            const TreeEdge& e = n.edges[i];
            if (most > 0 && !e.fixed && e.visits < options.selectiveTreeFinalShare * most) continue;
            if (!chosen || (n.maximize ? e.value > chosen->value + 1e-12 : e.value < chosen->value - 1e-12) ||
                (std::abs(e.value - chosen->value) <= 1e-12 && e.visits > chosen->visits))
                chosen = &e;
        }
        return chosen;
    }

    std::string TreeEdgeText(const TreeNode& n, const TreeEdge& e) const {
        GameState t = n.s;
        std::string text;
        for (int i = 0; i < e.count; ++i) {
            text += (text.empty() ? "" : " ") + FormatAction(t, e.moves[i]);
            ApplyUnchecked(t, e.moves[i], Outcome::Certain());
        }
        if (!e.terminalMoves) text += (text.empty() ? "" : " ") + FormatAction(t, e.action);
        return text;
    }

    bool TreeRoom(long simulations) {
        if (!Budget()) return false;
        if (options.selectiveTreeSimulations > 0 && simulations >= options.selectiveTreeSimulations)
            return false;
        if (options.maxMemoryBytes && treeBytes > options.maxMemoryBytes) {
            memoryLimited = true;
            return false;
        }
        return true;
    }

    // One simulation through the root's candidate `e`.
    bool TreeSimulateRootEdge(TreeNode& root, TreeEdge& e) {
        const bool ok = TreeSimulateEdge(root, e);
        if (ok) {
            ++root.visits;
            ++e.visits;
        }
        e.value = TreeEdgeValue(e);
        TreeNodeValue(root);
        return ok;
    }

    // Sequential halving at the root (as in Gumbel AlphaZero): the open
    // candidates get the same number of simulations in each phase (round
    // robin, PUCT below them), then the better half by value goes on. A value
    // backed up from a deeper tree is lower for the side that has the move at
    // the leaves' end (it has no move in hand there, 7.65), so candidates
    // compared at very different depths would favour the one searched least
    // or most; equal simulations keep the finalists at comparable depth. The
    // budget (time or simulations) is split evenly over the phases; what is
    // left goes to the last ones. Returns the survivors, best first.
    std::vector<int> TreeRootHalving(TreeNode& root, long& simulations) {
        std::vector<int> alive;
        for (int i = 0; i < root.open; ++i) alive.push_back(i);
        const auto better = [&](int a, int b) {
            const TreeEdge& x = root.edges[a];
            const TreeEdge& y = root.edges[b];
            if (x.value != y.value) return root.maximize ? x.value > y.value : x.value < y.value;
            return a < b;
        };
        int phases = 1;
        while ((std::size_t(1) << phases) < alive.size()) ++phases;
        const long simulationLimit = options.selectiveTreeSimulations;
        bool room = TreeRoom(simulations);
        for (int phase = 0; room; ++phase) {
            const int left = std::max(1, phases - phase);
            const Clock::time_point phaseEnd = Clock::now() + (deadline - Clock::now()) / left;
            const long phaseSimulations = simulationLimit > 0
                ? std::max(1L, (simulationLimit - simulations) / left) : 0;
            const long phaseStart = simulations;
            bool progressed = true;
            while (room && progressed) {
                progressed = false;
                for (int i : alive) {
                    TreeEdge& e = root.edges[i];
                    if (e.fixed) continue;
                    if (!(room = TreeRoom(simulations))) break;
                    if (timeLimit > 0 && Clock::now() >= phaseEnd && simulations > phaseStart) break;
                    if (simulationLimit > 0 && simulations - phaseStart >= phaseSimulations) break;
                    if (!TreeSimulateRootEdge(root, e)) { room = false; break; }
                    ++simulations;
                    progressed = true;
                }
                if (timeLimit > 0 && Clock::now() >= phaseEnd && simulations > phaseStart) break;
                if (simulationLimit > 0 && simulations - phaseStart >= phaseSimulations) break;
            }
            if (options.selectiveTreeTranspositions) RefreshTree(&root);
            std::stable_sort(alive.begin(), alive.end(), better);
            if (!progressed) break;   // every survivor is exact
            if (alive.size() > 1 && phase + 1 < phases) alive.resize((alive.size() + 1) / 2);
            else if (alive.size() > 2) alive.resize(2);
        }
        std::stable_sort(alive.begin(), alive.end(), better);
        return alive;
    }

    GoalSearchResult TreeSearch(const GameState& s, double seconds) {
        treeProfile = {};
        profileStage = 0;
        start = Clock::now();
        timeLimit = seconds >= 0 ? seconds : options.timeLimit;
        deadline = start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeLimit));
        rootHalfTurns = s.halfTurns;
        positions = expansions = maxDepth = 0;
        stopped = memoryLimited = false;
        stats = {};
        episodeExtensionCount = 0;
        breakawayLeaves = breakawayRaised = 0;
        tableLeaves = tableRaised = tableIncomplete = 0;
        twoVsOneLeaves = twoVsOneRaised = 0;
        episodeExtensions = raceExtensions = 0;
        for (long& count : raceLeaves) count = 0;
        nodeEpisode = nullptr;
        exitMask = 0;
        const GoalEpisode episode{s.ball.attackTeam, GoalEpisodeStatus::OurAttack};
        treeTranspositions = treeRepeats = 0;
        TreeNode* reused = options.selectiveTreeReuse ? LookupTreeNode(s, episode) : nullptr;
        if (reused) {
            KeepTreeFrom(reused);
            treeRoot = reused;
        } else {
            treePool.clear();
            treeTable.clear();
            treeNodes = 0;
            treeBytes = 0;
            treeRoot = TreeNodeFor(s, episode, Estimate(s, episode, true, true));
        }
        long simulationsAtStart = 0;
        const int reusedVisits = treeRoot->visits;
        if (!treeRoot->expanded && !treeRoot->fixed && Budget()) {
            if (TreeSimulate(*treeRoot)) ++simulationsAtStart;
        }
        if (treeRoot->expanded && !treeRoot->elementary && !treeRoot->edges.empty() && Budget())
            AddElementaryEdges(*treeRoot);
        if (timeLimit <= 0 && options.expansions <= 0 && options.selectiveTreeSimulations <= 0) stopped = true;
        long simulations = simulationsAtStart;
        std::vector<int> survivors;
        if (options.selectiveTreeRootHalving && treeRoot->expanded && !treeRoot->fixed)
            survivors = TreeRootHalving(*treeRoot, simulations);
        else
            while (!treeRoot->fixed && TreeRoom(simulations)) {
                if (!TreeSimulate(*treeRoot)) break;
                ++simulations;
            }
        if (options.selectiveTreeTranspositions) RefreshTree(treeRoot);
        if (!survivors.empty()) {
            const TreeNode& r = *treeRoot;
            std::stable_sort(survivors.begin(), survivors.end(), [&](int a, int b) {
                const TreeEdge& x = r.edges[a];
                const TreeEdge& y = r.edges[b];
                if (x.value != y.value) return r.maximize ? x.value > y.value : x.value < y.value;
                return a < b;
            });
        }
        const TreeNode& root = *treeRoot;
        GoalSearchResult result;
        const bool maximize = root.maximize;
        for (int i = 0; i < static_cast<int>(root.edges.size()); ++i) {
            const TreeEdge& e = root.edges[i];
            const Action first = e.count > 0 ? e.moves[0] : e.action;
            result.moves.push_back({first, GoalValueBounds{e.value, e.value}, i < root.open && e.visits > 0});
        }
        // The plan: the chosen candidate, and on through certain outcomes
        // within the turn while the tree has decided there.
        const TreeNode* node = &root;
        int principal = 0;
        while (node && node->expanded) {
            const TreeEdge* e = node == &root && !survivors.empty()
                ? &root.edges[survivors.front()] : TreeChoice(*node);
            if (!e) break;
            for (int i = 0; i < e->count; ++i) result.turnPlan.push_back(e->moves[i]);
            if (e->terminalMoves) break;
            result.turnPlan.push_back(e->action);
            if (node == &root) result.value = {e->value, e->value};
            const TreeNode* nextNode = nullptr;
            if (e->action.type != ActionType::EndTurn && e->branches.size() == 1 &&
                e->branches[0].p == 1.0 && e->branches[0].node &&
                e->branches[0].node->s.turnId == node->s.turnId && e->branches[0].node->expanded &&
                !e->branches[0].node->edges.empty())
                nextNode = e->branches[0].node;
            node = nextNode;
        }
        if (result.turnPlan.empty()) result.turnPlan.push_back(Action::EndTurn());
        result.action = result.turnPlan.front();
        if (root.open == 0) result.value = {root.value, root.value};
        // The principal line: the most visited candidate and outcome.
        for (const TreeNode* p = &root; p && p->expanded;) {
            const TreeEdge* most = nullptr;
            for (int i = 0; i < p->open; ++i)
                if (!most || p->edges[i].visits > most->visits) most = &p->edges[i];
            if (!most || most->visits == 0) break;
            const TreeBranch* b = nullptr;
            for (const TreeBranch& x : most->branches)
                if (x.node && (!b || x.visits > b->visits)) b = &x;
            if (!b) break;
            p = b->node;
            principal = std::max(principal, p->s.halfTurns - rootHalfTurns);
        }
        stats.completedTurnDepth = principal;
        result.rootBounds = InitialGoalBounds(GoalEpisodeStatus::OurAttack);
        result.regretBound = 2;
        result.proven = false;
        result.positions = positions;
        result.expansions = expansions;
        result.maxDepth = maxDepth;
        result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
        result.estimatedMemoryMB = treeBytes / 1048576.0;
        result.memoryLimited = memoryLimited;
        stats.peakMemoryMB = result.estimatedMemoryMB;
        char message[400];
        std::snprintf(message, sizeof message,
                      "goal-sel-tree %.3f | %ld simulations (%d reused), %ld nodes (%ld transpositions, %ld repeats), depth %d (line %d) "
                      "half-turns, %ld expansions, %.1f s, %.1f MB%s | width %d, tree width %zu, "
                      "c %.3g, T %.3g, widen %.3g x N^%.3g; no proof",
                      result.value.lower, simulations, reusedVisits, treeNodes, treeTranspositions, treeRepeats,
                      maxDepth, principal,
                      expansions, result.seconds, result.estimatedMemoryMB,
                      memoryLimited ? " (memory limit)" : "", options.selectiveWidth, TreeWidth(),
                      options.selectiveTreeCPuct, options.selectiveTreeTemperature,
                      options.selectiveTreeWidenBase, options.selectiveTreeWidenExponent);
        result.note = message;
        if (options.profileTree) {
            char detail[512];
            const auto ms = [](uint64_t ns) { return ns / 1e6; };
            std::snprintf(detail, sizeof detail,
                          "\nprofile_ms total=%.3f node=%.3f movement=%.3f category=%.3f "
                          "edge=%.3f elementary=%.3f node_for=%.3f estimate=%.3f "
                          "estimate_calls=%llu node_calls=%llu edge_calls=%llu (nested times overlap)",
                          result.seconds * 1000, ms(treeProfile.nodeNs), ms(treeProfile.movementNs),
                          ms(treeProfile.categoryNs), ms(treeProfile.edgeNs),
                          ms(treeProfile.elementaryNs), ms(treeProfile.nodeForNs),
                          ms(treeProfile.estimateNs),
                          static_cast<unsigned long long>(treeProfile.estimateCalls),
                          static_cast<unsigned long long>(treeProfile.nodeCalls),
                          static_cast<unsigned long long>(treeProfile.edgeCalls));
            result.note += detail;
            std::snprintf(detail, sizeof detail,
                          "\nprofile_action_scores attack=%llu pass=%llu shot=%llu end=%llu "
                          "outcomes_attack=%llu outcomes_pass=%llu outcomes_shot=%llu outcomes_end=%llu "
                          "ms_attack=%.3f ms_pass=%.3f ms_shot=%.3f ms_end=%.3f",
                          static_cast<unsigned long long>(treeProfile.actionScoreCalls[2]),
                          static_cast<unsigned long long>(treeProfile.actionScoreCalls[3]),
                          static_cast<unsigned long long>(treeProfile.actionScoreCalls[4]),
                          static_cast<unsigned long long>(treeProfile.actionScoreCalls[5]),
                          static_cast<unsigned long long>(treeProfile.actionScoreOutcomes[2]),
                          static_cast<unsigned long long>(treeProfile.actionScoreOutcomes[3]),
                          static_cast<unsigned long long>(treeProfile.actionScoreOutcomes[4]),
                          static_cast<unsigned long long>(treeProfile.actionScoreOutcomes[5]),
                          ms(treeProfile.actionScoreNs[2]), ms(treeProfile.actionScoreNs[3]),
                          ms(treeProfile.actionScoreNs[4]), ms(treeProfile.actionScoreNs[5]));
            result.note += detail;
            std::snprintf(detail, sizeof detail,
                          "\nprofile_estimate_calls other=%llu node=%llu movement=%llu "
                          "category=%llu edge=%llu elementary=%llu ranking=%llu actions=%llu",
                          static_cast<unsigned long long>(treeProfile.estimateStageCalls[0]),
                          static_cast<unsigned long long>(treeProfile.estimateStageCalls[1]),
                          static_cast<unsigned long long>(treeProfile.estimateStageCalls[2]),
                          static_cast<unsigned long long>(treeProfile.estimateStageCalls[3]),
                          static_cast<unsigned long long>(treeProfile.estimateStageCalls[4]),
                          static_cast<unsigned long long>(treeProfile.estimateStageCalls[5]),
                          static_cast<unsigned long long>(treeProfile.estimateStageCalls[6]),
                          static_cast<unsigned long long>(treeProfile.estimateStageCalls[7]));
            result.note += detail;
            std::snprintf(detail, sizeof detail,
                          "\nprofile_estimate_ms other=%.3f node=%.3f movement=%.3f "
                          "category=%.3f edge=%.3f elementary=%.3f ranking=%.3f actions=%.3f",
                          ms(treeProfile.estimateStageNs[0]), ms(treeProfile.estimateStageNs[1]),
                          ms(treeProfile.estimateStageNs[2]), ms(treeProfile.estimateStageNs[3]),
                          ms(treeProfile.estimateStageNs[4]), ms(treeProfile.estimateStageNs[5]),
                          ms(treeProfile.estimateStageNs[6]), ms(treeProfile.estimateStageNs[7]));
            result.note += detail;
        }
        // The principal line with the values along it.
        result.note += "\n  line:";
        for (const TreeNode* p = &root; p && p->expanded;) {
            const TreeEdge* most = nullptr;
            for (int i = 0; i < p->open; ++i)
                if (!most || p->edges[i].visits > most->visits) most = &p->edges[i];
            if (!most || most->visits == 0) break;
            char head[96];
            std::snprintf(head, sizeof head, " | %s%d %.3f (%d/%d open, %d) ",
                          p->s.teamToMove == kWhite ? "W" : "B", p->s.halfTurns + 1, p->value, p->open,
                          static_cast<int>(p->edges.size()), p->visits);
            result.note += head + TreeEdgeText(*p, *most);
            const TreeBranch* b = nullptr;
            for (const TreeBranch& x : most->branches)
                if (x.node && (!b || x.visits > b->visits)) b = &x;
            if (!b) break;
            if (most->branches.size() > 1) result.note += " {" + FormatOutcome(b->outcome) + "}";
            p = b->node;
        }
        // The root's candidates, best first by value among the searched.
        std::vector<int> order;
        for (int i = 0; i < static_cast<int>(root.edges.size()); ++i) order.push_back(i);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const TreeEdge& x = root.edges[a];
            const TreeEdge& y = root.edges[b];
            if ((x.visits > 0) != (y.visits > 0)) return x.visits > 0;
            return maximize ? x.value > y.value : x.value < y.value;
        });
        for (std::size_t k = 0; k < order.size() && k < 12; ++k) {
            const TreeEdge& e = root.edges[order[k]];
            char line[160];
            std::snprintf(line, sizeof line, "\n  %6d  %7.3f  est %7.3f  prior %.3f  ", e.visits, e.value,
                          e.estimate, e.prior);
            result.note += line + TreeEdgeText(root, e);
        }
        return result;
    }

    static int Best(const std::vector<GoalRootMove>& moves, bool maximize) {
        int best = -1;
        for (int i = 0; i < static_cast<int>(moves.size()); ++i)
            if (best < 0 || (maximize ? moves[i].bounds.lower > moves[best].bounds.lower
                                      : moves[i].bounds.upper < moves[best].bounds.upper)) best = i;
        return best;
    }

    static double Regret(const std::vector<GoalRootMove>& moves, int best, bool maximize) {
        if (best < 0) return 2;
        double rival = maximize ? -1 : 1;
        for (int i = 0; i < static_cast<int>(moves.size()); ++i) {
            if (i == best) continue;
            rival = maximize ? std::max(rival, moves[i].bounds.upper)
                             : std::min(rival, moves[i].bounds.lower);
        }
        return std::max(0.0, maximize ? rival - moves[best].bounds.lower
                                      : moves[best].bounds.upper - rival);
    }

    void InitializeWorker(const GameState& s, Clock::time_point searchStart,
                          Clock::time_point searchDeadline, double seconds, std::size_t memoryBytes,
                          std::atomic<long>* expansionCounter, std::atomic<bool>* stopFlag) {
        start = searchStart;
        deadline = searchDeadline;
        timeLimit = seconds;
        rootHalfTurns = s.halfTurns;
        positions = expansions = maxDepth = 0;
        stopped = memoryLimited = false;
        stats = {};
        episodeExtensionCount = 0;
        breakawayLeaves = breakawayRaised = 0;
        tableLeaves = tableRaised = tableIncomplete = 0;
        twoVsOneLeaves = twoVsOneRaised = 0;
        episodeExtensions = raceExtensions = 0;
        for (long& count : raceLeaves) count = 0;
        memoryBudgetBytes = memoryBytes;
        sharedExpansions = expansionCounter;
        sharedStop = stopFlag;
        const bool selectiveTable = options.selectiveWidth > 0 && options.selectiveTransposition;
        const std::size_t entries = selectiveTable ? 0 : memoryBytes / 2 / sizeof(Entry);
        cache.clear();
        cache.resize(entries);
        // Selective entries depend only on (state, episode, remaining depth), so
        // the table stays valid from one search (and one move) to the next.
        const std::size_t selectiveEntries = selectiveTable ? memoryBytes / 2 / sizeof(SelectiveEntry) : 0;
        if (selectiveCache.size() != selectiveEntries) {
            selectiveCache.clear();
            selectiveCache.shrink_to_fit();
            selectiveCache.resize(selectiveEntries);
        }
        stats.cacheEntries = selectiveTable ? selectiveCache.size() : entries;
        liveBytes = peakBytes = cache.capacity() * sizeof(Entry) +
                                selectiveCache.capacity() * sizeof(SelectiveEntry);
    }

    // With the attack episode (7.67) the root's outsiders take their policy
    // moves first; the search starts after them and the plan begins with them.
    GoalSearchResult Search(const GameState& root, double seconds) {
        if (options.selectiveWidth > 0 && options.selectiveTree && root.phase == Phase::Play)
            return TreeSearch(root, seconds);
        if (options.selectiveWidth == 0 || !options.selectiveAttackEpisode || root.phase != Phase::Play)
            return SearchFrom(root, seconds);
        const AttackEpisode rootEpisode = AnalyzeAttackEpisode(root, false, options.selectiveEpisodeBallRace);
        const std::vector<Action> prefix = OutsiderMoves(root, rootEpisode);
        GameState s = root;
        for (const Action& move : prefix) ApplyUnchecked(s, move, Outcome::Certain());
        nodeEpisode = &rootEpisode;
        exitMask = rootEpisode.outside;
        GoalSearchResult result = SearchFrom(s, seconds);
        nodeEpisode = nullptr;
        exitMask = 0;
        if (!prefix.empty()) {
            result.turnPlan.insert(result.turnPlan.begin(), prefix.begin(), prefix.end());
            result.action = result.turnPlan.front();
        }
        GameState t = root;
        std::string moved;
        for (const Action& move : prefix) {
            moved += " " + FormatAction(t, move);
            ApplyUnchecked(t, move, Outcome::Certain());
        }
        result.note += std::string(" | attack episode ") + AttackEpisodeName(rootEpisode.kind) +
                       ", outsiders' moves:" + (moved.empty() ? " none" : moved);
        return result;
    }

    GoalSearchResult SearchFrom(const GameState& s, double seconds) {
        const auto searchStart = Clock::now();
        const double limit = seconds >= 0 ? seconds : options.timeLimit;
        const auto searchDeadline = searchStart +
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(limit));
        GoalSearchResult result;
        GoalEpisode episode{s.ball.attackTeam, GoalEpisodeStatus::OurAttack};
        // With profileRoot a one-slot pool measures the parallel share on one thread.
        if (options.selectiveWidth > 0 && (options.threads > 1 || options.profileRoot) && !options.selectiveRootSplit) {
            const int slots = std::max(1, options.threads);
            if (!pool || pool->size != slots) {
                pool.reset(new EstimatePool(options, slots));
                if (options.selectiveStaticCacheBits > 0 && options.selectiveStaticCacheBits <= 26) {
                    sharedStatic = std::make_shared<SharedStaticCache>(options.selectiveStaticCacheBits);
                    sharedRace = std::make_shared<SharedStaticCache>(std::max(1, options.selectiveStaticCacheBits - 3));
                    sharedLeaf = std::make_shared<SharedStaticCache>(options.selectiveStaticCacheBits);
                    for (auto& h : pool->helpers) {
                        h->sharedStatic = sharedStatic;
                        h->sharedRace = sharedRace;
                        h->sharedLeaf = sharedLeaf;
                    }
                }
            }
            pool->batches = pool->parallelJobs = 0;
        } else {
            pool.reset();
            sharedStatic.reset();
            sharedRace.reset();
            sharedLeaf.reset();
        }
        const GoalValueBounds base = Bounds(s, episode);
        std::vector<Action> actions = LegalActions(s);
        if (options.selectiveWidth > 0 && options.selectiveReachPassPrune)
            stats.reachPassPrunes += PruneReachPasses(s, actions, &stats.reachMaps);
        if (options.frozenPlayers)
            actions.erase(std::remove_if(actions.begin(), actions.end(), [&](const Action& a) {
                return a.type == ActionType::Move && (options.frozenPlayers >> a.player & 1u);
            }), actions.end());
        std::stable_sort(actions.begin(), actions.end(), [&](const Action& a, const Action& b) {
            return ActionOrder(s, a) < ActionOrder(s, b);
        });
        result.moves.reserve(actions.size());
        for (const Action& action : actions) result.moves.push_back({action, base, false});
        const unsigned available = std::max(1u, std::thread::hardware_concurrency());
        const int requested = options.threads > 0 ? options.threads : static_cast<int>(available);
        const int threadCount = options.selectiveWidth > 0 ? 1 :
            std::max(1, std::min(requested, static_cast<int>(actions.size())));
        std::atomic<long> expansionCounter{0};
        std::atomic<bool> stopFlag{false};
        std::vector<std::unique_ptr<Impl>> ownedWorkers;
        std::vector<Impl*> workers{this};
        auto workerBudget = [&](int index) {
            return options.maxMemoryBytes / threadCount +
                   (static_cast<std::size_t>(index) < options.maxMemoryBytes % threadCount ? 1 : 0);
        };
        InitializeWorker(s, searchStart, searchDeadline, limit, workerBudget(0),
                         threadCount > 1 ? &expansionCounter : nullptr,
                         threadCount > 1 ? &stopFlag : nullptr);
        for (int i = 1; i < threadCount; ++i) {
            ownedWorkers.emplace_back(new Impl(options));
            Impl* worker = ownedWorkers.back().get();
            worker->InitializeWorker(s, searchStart, searchDeadline, limit, workerBudget(i),
                                     &expansionCounter, &stopFlag);
            workers.push_back(worker);
        }
        const int depthLimit = options.completeTurns ? options.maxTurnDepth : options.maxActionDepth;
        if (limit <= 0 && options.expansions <= 0 && depthLimit <= 0) stopped = true;
        MemoryScope root(*this, result.moves.capacity() * sizeof(GoalRootMove) +
                               actions.capacity() * sizeof(Action) + sizeof(GameState) + 256);
        if (!root.admitted) stopped = true;
        const bool maximize = s.teamToMove == episode.team;
        std::vector<std::size_t> selectedRoot;
        std::vector<std::vector<Action>> rootPlans(actions.size());
        // The root as an ordinary node (bot_design.md 3.2, 29.10.2026): whole
        // team movements plus a ball action, exactly as below the root, instead
        // of single first actions ranked by the estimate after one action.
        // Category root (bot_design.md 3.2, 29.10.2026): when the team to move
        // holds the ball, every complete team movement with its ball action is
        // a root decision. Decisions are grouped by channel category (keep,
        // shot, pass to each partner); the best few of each category by the
        // cheap estimate are refined by a short search with the opponent's
        // reply, then every category keeps its best and the remaining places
        // go to the best refined decisions.
        const bool categoryRoot = options.selectiveWidth > 0 && options.selectiveCategoryRoot &&
                                  s.phase == Phase::Play && !s.ball.IsFree() &&
                                  TeamOf(s.ball.holder) == s.teamToMove;
        bool haveCategoryResult = false;
        GoalValueBounds categoryValue = base;
        std::vector<Action> categoryPlan;
        if (categoryRoot && root.admitted && !stopped) {
            AttackEpisode rootEpisode;
            if (options.selectiveAttackEpisode) {
                rootEpisode = AnalyzeAttackEpisode(s, false, options.selectiveEpisodeBallRace);
                nodeEpisode = &rootEpisode;
                exitMask = rootEpisode.outside;
            }
            TeamMovementList all = EnumerateTeamMovements(s, episode, 0, [this] {
                if (!TakeExpansion()) return false;
                ++stats.moveExpansions;
                return true;
            });
            struct Decision { std::size_t state; Action action; int category; double cheap; double refined; };
            std::vector<Decision> decisions;
            for (std::size_t i = 0; i < all.states.size() && Budget(); ++i) {
                const TeamMovementState& node = all.states[i];
                if (Terminal(node.episode.status)) continue;
                std::vector<Action> legal = LegalActions(node.state);
                if (options.selectiveReachPassPrune)
                    stats.reachPassPrunes += PruneReachPasses(node.state, legal, &stats.reachMaps);
                for (const Action& action : legal) {
                    if (action.type == ActionType::Move) continue;
                    decisions.push_back({i, action, PartnerCategory(node.state, action),
                                         ActionEstimate(node.state, node.episode, action), 0});
                }
            }
            const auto better = [maximize](double a, double b) { return maximize ? a > b : a < b; };
            std::stable_sort(decisions.begin(), decisions.end(), [&](const Decision& a, const Decision& b) {
                return better(a.cheap, b.cheap);
            });
            std::vector<std::size_t> shortlist;
            int perCategory[8] = {};
            for (std::size_t i = 0; i < decisions.size(); ++i)
                if (perCategory[decisions[i].category]++ < options.selectiveCategoryShortlist)
                    shortlist.push_back(i);
            stats.selectiveActionDrops += static_cast<long>(decisions.size() - shortlist.size());
            for (std::size_t i : shortlist) {
                if (!Budget()) break;
                const TeamMovementState& node = all.states[decisions[i].state];
                const GoalValueBounds v = ActionBounds(node.state, node.episode, decisions[i].action,
                                                       options.selectiveCategoryRefineDepth, -1, 1);
                decisions[i].refined = maximize ? v.lower : v.upper;
            }
            std::stable_sort(shortlist.begin(), shortlist.end(), [&](std::size_t a, std::size_t b) {
                return better(decisions[a].refined, decisions[b].refined);
            });
            std::vector<std::size_t> rootSet;
            bool represented[8] = {};
            for (std::size_t i : shortlist)
                if (!represented[decisions[i].category] && rootSet.size() < static_cast<std::size_t>(options.selectiveWidth)) {
                    represented[decisions[i].category] = true;
                    rootSet.push_back(i);
                }
            for (std::size_t i : shortlist) {
                if (rootSet.size() >= static_cast<std::size_t>(options.selectiveWidth)) break;
                if (std::find(rootSet.begin(), rootSet.end(), i) == rootSet.end()) rootSet.push_back(i);
            }
            for (int depth = 1; (depthLimit <= 0 || depth <= depthLimit) && Budget(); ++depth) {
                bool complete = true, have = false;
                double bestValue = 0;
                std::vector<Action> bestPlan;
                GoalValueBounds bestBounds = base;
                for (std::size_t i : rootSet) {
                    if (!Budget()) { complete = false; break; }
                    const TeamMovementState& node = all.states[decisions[i].state];
                    std::vector<Action> continuation;
                    const GoalValueBounds v = ActionBounds(node.state, node.episode, decisions[i].action,
                                                           depth, -1, 1, &continuation);
                    if (stopped) { complete = false; break; }
                    const double value = maximize ? v.lower : v.upper;
                    if (!have || better(value, bestValue)) {
                        have = true;
                        bestValue = value;
                        bestBounds = v;
                        bestPlan.assign(node.moves, node.moves + node.count);
                        bestPlan.push_back(decisions[i].action);
                        bestPlan.insert(bestPlan.end(), continuation.begin(), continuation.end());
                    }
                }
                if (!complete) {
                    if (!haveCategoryResult && have) { categoryPlan = bestPlan; categoryValue = bestBounds; }
                    break;
                }
                haveCategoryResult = true;
                categoryPlan = std::move(bestPlan);
                categoryValue = bestBounds;
                stats.completedTurnDepth = depth;
            }
            nodeEpisode = nullptr;
            exitMask = 0;
        }
        const bool nodeRoot = !categoryRoot && options.selectiveWidth > 0 && options.selectiveRootAsNode;
        bool haveNodeResult = false;
        GoalValueBounds nodeValue = base;
        std::vector<Action> nodePlan;
        if (nodeRoot && root.admitted && !stopped) {
            for (int depth = 1; (depthLimit <= 0 || depth <= depthLimit) && Budget(); ++depth) {
                std::vector<Action> plan;
                const GoalValueBounds value = Solve(s, episode, depth, -1, 1, &plan);
                if (stopped) {
                    if (!haveNodeResult && !plan.empty()) { nodePlan = plan; nodeValue = value; }
                    break;
                }
                haveNodeResult = true;
                nodeValue = value;
                nodePlan = std::move(plan);
                stats.completedTurnDepth = depth;
            }
        } else if (!categoryRoot && options.selectiveWidth > 0 && root.admitted && !stopped) {
            std::vector<int> reachPassKinds(actions.size(), -1);
            if (options.selectiveReachPassRootKind && s.phase == Phase::Play &&
                !s.ball.IsFree() && TeamOf(s.ball.holder) == s.teamToMove) {
                const ReachFront front = ComputeReachFront(s);
                if (front.stop != ReachFrontStop::InvalidStart)
                    for (std::size_t i = 0; i < actions.size(); ++i)
                        if (actions[i].type == ActionType::Pass && PlayerAt(s, actions[i].target) == kNoPlayer) {
                            GameState after = s;
                            ApplyUnchecked(after, actions[i], Outcome{OutcomeKind::BallThrough});
                            const BallRaceResult race = SolveBallRace(after);
                            reachPassKinds[i] = ReachPassFirstTeam(s, front, race.windows);
                        }
            }
            std::vector<std::pair<double, std::size_t>> ranked;
            ranked.reserve(actions.size());
            for (std::size_t i = 0; i < actions.size(); ++i)
                ranked.push_back({ActionEstimate(s, episode, actions[i], false, false,
                                                 options.selectiveRaceMovements), i});
            const auto betterRoot = [maximize](const auto& a, const auto& b) {
                if (a.first != b.first) return maximize ? a.first > b.first : a.first < b.first;
                return a.second < b.second;
            };
            std::size_t count = std::min(ranked.size(),
                                         static_cast<std::size_t>(options.selectiveWidth));
            if (options.selectiveKindDiversity) {
                std::vector<double> cheap, kept;
                std::vector<int> kinds;
                for (std::size_t i = 0; i < actions.size(); ++i) {
                    cheap.push_back(ranked[i].first);
                    const int kind = DecisionKind(s, actions[i], options.selectivePickupDiversity >= 1);
                    kinds.push_back(kind == 3 && reachPassKinds[i] == s.teamToMove ? 9 : kind);
                }
                const std::vector<std::size_t> chosen = DiverseBeam(cheap, kinds, [&](std::size_t i) {
                    return ActionEstimate(s, episode, actions[i], true, true, options.selectiveRaceMovements);
                }, maximize, count, &kept);
                count = chosen.size();
                for (std::size_t i : chosen) selectedRoot.push_back(i);
                // Defence forced (09.11): every tackle and every first move that lets
                // its player tackle or puts him on the ball's path, beyond the width.
                if (options.selectiveDefenceForced && DefendingTurn(s)) {
                    const std::vector<Cell> cells = InterceptCells(s);
                    for (std::size_t i = 0; i < actions.size(); ++i) {
                        if (std::find(selectedRoot.begin(), selectedRoot.end(), i) != selectedRoot.end()) continue;
                        bool forced = actions[i].type == ActionType::Attack;
                        if (actions[i].type == ActionType::Move) {
                            GameState after = s;
                            ApplyUnchecked(after, actions[i], Outcome::Certain());
                            forced = ForcedPlayer(after, actions[i].player, cells);
                        }
                        if (!forced) continue;
                        selectedRoot.push_back(i);
                        ++stats.defenceForcedKept;
                    }
                    count = selectedRoot.size();
                }
            } else {
                const std::size_t shortlist = std::min(ranked.size(),
                    static_cast<std::size_t>(options.selectiveWidth) * 2);
                std::partial_sort(ranked.begin(), ranked.begin() + shortlist,
                                  ranked.end(), betterRoot);
                for (std::size_t i = 0; i < shortlist; ++i)
                    ranked[i].first = ActionEstimate(s, episode, actions[ranked[i].second], true, true);
                std::sort(ranked.begin(), ranked.begin() + shortlist, betterRoot);
                for (std::size_t i = 0; i < count; ++i) selectedRoot.push_back(ranked[i].second);
            }
            stats.selectiveActionDrops += static_cast<long>(actions.size() - count);
            // Search order: the previous iteration's best first (alpha-beta only).
            // The final choice still scans `selectedRoot` in rank order, so ties
            // resolve exactly as in the plain beam minimax.
            std::vector<std::size_t> searchOrder(selectedRoot.size());
            for (std::size_t i = 0; i < searchOrder.size(); ++i) searchOrder[i] = i;
            // Root split (02.11.2026): with options.threads > 1 the first
            // decision of an iteration (the previous best) is searched alone,
            // then the others in parallel by helper searches, each with the
            // window of the best value known when it starts.
            const int rootThreads = options.selectiveRootSplit
                ? std::max(1, std::min(options.threads, static_cast<int>(selectedRoot.size()))) : 1;
            while (static_cast<int>(selectiveHelpers.size()) < rootThreads - 1)
                selectiveHelpers.emplace_back(new Impl(options));
            std::atomic<bool> helperStop{false};
            for (int i = 0; i + 1 < rootThreads; ++i) {
                Impl& h = *selectiveHelpers[i];
                h.InitializeWorker(s, searchStart, searchDeadline, limit, options.maxMemoryBytes, nullptr, &helperStop);
                h.nodeEpisode = nodeEpisode;
                h.exitMask = exitMask;
            }
            rootProfile.clear();
            for (int depth = 1; (depthLimit <= 0 || depth <= depthLimit) && Budget() && rootThreads > 1;
                 depth = NextRootDepth(depth)) {
                bool complete = true;
                std::vector<GoalValueBounds> iteration(selectedRoot.size());
                std::vector<std::vector<Action>> iterationPlans(selectedRoot.size());
                std::vector<double> slotSeconds(selectedRoot.size(), 0);
                const unsigned rootFrozen = FrozenPlayers();
                std::vector<char> skipped(selectedRoot.size(), 0);
                std::mutex bestMutex;
                bool haveBest = false;
                double bestValue = 0;
                std::atomic<bool> incomplete{false};
                auto runSlot = [&](Impl& worker, std::size_t slot) {
                    const std::size_t index = selectedRoot[slot];
                    if (actions[index].type == ActionType::Move && (rootFrozen >> actions[index].player & 1u)) {
                        skipped[slot] = 1;
                        return;
                    }
                    double a = -1, b = 1;
                    {
                        std::lock_guard<std::mutex> lock(bestMutex);
                        if (options.selectiveAlphaBeta && haveBest) {
                            if (maximize) a = Down(bestValue);
                            else b = Up(bestValue);
                        }
                    }
                    const auto t0 = Clock::now();
                    std::vector<Action> continuation;
                    const GoalValueBounds value = worker.ActionBounds(s, episode, actions[index], depth, a, b,
                                                                      &continuation);
                    slotSeconds[slot] = std::chrono::duration<double>(Clock::now() - t0).count();
                    if (worker.stopped) { incomplete.store(true); return; }
                    iteration[slot] = value;
                    iterationPlans[slot] = std::move(continuation);
                    const double v = maximize ? value.lower : value.upper;
                    std::lock_guard<std::mutex> lock(bestMutex);
                    if (!haveBest || (maximize ? v > bestValue : v < bestValue)) {
                        haveBest = true;
                        bestValue = v;
                    }
                };
                // The first decision alone, then the rest in parallel.
                std::size_t firstAt = 0;
                runSlot(*this, searchOrder[0]);
                firstAt = 1;
                if (!incomplete.load() && Budget()) {
                    std::atomic<std::size_t> next{firstAt};
                    auto loop = [&](Impl& worker) {
                        while (!incomplete.load() && worker.Budget()) {
                            const std::size_t k = next.fetch_add(1);
                            if (k >= searchOrder.size()) return;
                            runSlot(worker, searchOrder[k]);
                        }
                        if (next.load() < searchOrder.size()) incomplete.store(true);
                    };
                    std::vector<std::thread> threads;
                    for (int i = 0; i + 1 < rootThreads; ++i)
                        threads.emplace_back(loop, std::ref(*selectiveHelpers[i]));
                    loop(*this);
                    for (std::thread& t : threads) t.join();
                } else {
                    incomplete.store(true);
                }
                if (options.profileRoot) {
                    char line[64];
                    std::snprintf(line, sizeof line, " | d%d:", depth);
                    rootProfile += line;
                    for (std::size_t slot : searchOrder) {
                        std::snprintf(line, sizeof line, " %.2f", slotSeconds[slot]);
                        rootProfile += line;
                    }
                }
                complete = !incomplete.load() && !stopped;
                if (complete) {
                    // Round depth (09.11): a depth whose leaves follow the root
                    // side's own turn is not the decision (only depth 1, as a
                    // fallback); it still orders the next iteration.
                    if (CommitRootDepth(depth)) {
                        for (std::size_t i = 0; i < selectedRoot.size(); ++i) {
                            result.moves[selectedRoot[i]].bounds = iteration[i];
                            result.moves[selectedRoot[i]].opened = !skipped[i];
                            rootPlans[selectedRoot[i]] = std::move(iterationPlans[i]);
                        }
                        stats.completedTurnDepth = depth;
                    }
                    if (options.selectiveAlphaBeta) {
                        std::stable_sort(searchOrder.begin(), searchOrder.end(),
                                         [&](std::size_t x, std::size_t y) {
                            const double vx = maximize ? iteration[x].lower : iteration[x].upper;
                            const double vy = maximize ? iteration[y].lower : iteration[y].upper;
                            return maximize ? vx > vy : vx < vy;
                        });
                    }
                } else {
                    break;
                }
            }
            for (int i = 0; i + 1 < rootThreads; ++i) {
                Impl& h = *selectiveHelpers[i];
                expansions += h.expansions;
                positions += h.positions;
                maxDepth = std::max(maxDepth, h.maxDepth);
                workers.push_back(&h);
            }
            // Root depth average (09.11.2026, the user): a root decision's value is
            // the mean of its values at this depth and the one before, so the
            // optimism of a depth ending on the root side's own turn and the
            // pessimism of one ending on the reply cancel. With alpha-beta a
            // decision is searched against the incumbent's mean: it beats it
            // only if its value here exceeds 2 * mean - its previous value.
            const bool rootAverage = options.selectiveRootDepthAverage;
            std::vector<GoalValueBounds> previous(selectedRoot.size());
            std::vector<char> lastSkipped(selectedRoot.size(), 1);
            bool havePrevious = false;
            for (int depth = 1; (depthLimit <= 0 || depth <= depthLimit) && Budget() && rootThreads == 1;
                 depth = NextRootDepth(depth)) {
                bool complete = true;
                std::vector<GoalValueBounds> iteration(selectedRoot.size()), rawIteration(selectedRoot.size());
                std::vector<std::vector<Action>> iterationPlans(selectedRoot.size());
                std::vector<double> slotSeconds(selectedRoot.size(), 0);
                const std::vector<char> skippedBefore = havePrevious ? lastSkipped : std::vector<char>(selectedRoot.size(), 1);
                bool haveBest = false;
                double bestValue = 0;
                // An outsider's first move is the same search as standing still.
                const unsigned rootFrozen = FrozenPlayers();
                std::vector<char> skipped(selectedRoot.size(), 0);
                for (std::size_t slot : searchOrder) {
                    if (!Budget()) { complete = false; break; }
                    const std::size_t index = selectedRoot[slot];
                    if (actions[index].type == ActionType::Move && (rootFrozen >> actions[index].player & 1u)) {
                        skipped[slot] = 1;
                        continue;
                    }
                    std::vector<Action> continuation;
                    // A value equal to the incumbent must stay exact (ties are
                    // broken by rank), hence the window just outside it.
                    double a = -1, b = 1;
                    // Average: `before` is the decision's value at the previous depth; a
                    // bound there (it failed against the incumbent) is used on its
                    // favourable side for the window and made exact only when this
                    // depth may let the decision win.
                    const bool averaged = rootAverage && havePrevious && !skippedBefore[slot];
                    const bool beforeExact = averaged && previous[slot].lower == previous[slot].upper;
                    double before = averaged ? (maximize ? previous[slot].upper : previous[slot].lower) : 0;
                    double need = 0;
                    if (options.selectiveAlphaBeta && haveBest) {
                        if (averaged) {
                            need = std::max(-1.0, std::min(1.0, 2 * bestValue - before));
                            if (maximize) a = Down(need);
                            else b = Up(need);
                        } else if (maximize) a = Down(bestValue);
                        else b = Up(bestValue);
                    }
                    const auto t0 = Clock::now();
                    const GoalValueBounds raw = ActionBounds(s, episode, actions[index], depth, a, b,
                                                             &continuation);
                    if (stopped) { complete = false; break; }
                    rawIteration[slot] = raw;
                    GoalValueBounds value = raw;
                    if (averaged) {
                        const bool mayWin = !haveBest || (maximize ? raw.lower > need : raw.upper < need);
                        if (!beforeExact && mayWin) {
                            const GoalValueBounds exact = ActionBounds(s, episode, actions[index], depth - 1, -1, 1);
                            if (stopped) { complete = false; break; }
                            before = maximize ? exact.lower : exact.upper;
                        }
                        value.lower = (raw.lower + before) / 2;
                        value.upper = (raw.upper + before) / 2;
                    }
                    slotSeconds[slot] = std::chrono::duration<double>(Clock::now() - t0).count();
                    iteration[slot] = value;
                    iterationPlans[slot] = std::move(continuation);
                    const double v = maximize ? value.lower : value.upper;
                    if (!haveBest || (maximize ? v > bestValue : v < bestValue)) {
                        haveBest = true;
                        bestValue = v;
                    }
                }
                if (options.profileRoot) {
                    char line[64];
                    std::snprintf(line, sizeof line, " | d%d:", depth);
                    rootProfile += line;
                    for (std::size_t slot : searchOrder) {
                        std::snprintf(line, sizeof line, " %.2f", slotSeconds[slot]);
                        rootProfile += line;
                    }
                }
                if (complete && rootAverage) {
                    previous = rawIteration;
                    lastSkipped = skipped;
                    havePrevious = true;
                }
                if (complete) {
                    // Round depth (09.11): a depth whose leaves follow the root
                    // side's own turn is not the decision (only depth 1, as a
                    // fallback); it still orders the next iteration.
                    if (CommitRootDepth(depth)) {
                        for (std::size_t i = 0; i < selectedRoot.size(); ++i) {
                            result.moves[selectedRoot[i]].bounds = iteration[i];
                            result.moves[selectedRoot[i]].opened = !skipped[i];
                            rootPlans[selectedRoot[i]] = std::move(iterationPlans[i]);
                        }
                        stats.completedTurnDepth = depth;
                    }
                    if (options.selectiveAlphaBeta) {
                        std::stable_sort(searchOrder.begin(), searchOrder.end(),
                                         [&](std::size_t x, std::size_t y) {
                            const double vx = maximize ? iteration[x].lower : iteration[x].upper;
                            const double vy = maximize ? iteration[y].lower : iteration[y].upper;
                            return maximize ? vx > vy : vx < vy;
                        });
                    }
                }
            }
        }
        for (int depth = 1; options.selectiveWidth == 0 && root.admitted && !stopped; ++depth) {
            if (depthLimit > 0 && depth > depthLimit) break;
            bool complete = true;
            if (threadCount == 1) {
                for (GoalRootMove& move : result.moves) {
                    if (!Budget()) { complete = false; break; }
                    const int incumbent = Best(result.moves, maximize);
                    const double threshold = maximize ? result.moves[incumbent].bounds.lower
                                                      : result.moves[incumbent].bounds.upper;
                    if ((maximize && move.bounds.upper <= threshold && &move != &result.moves[incumbent]) ||
                        (!maximize && move.bounds.lower >= threshold && &move != &result.moves[incumbent])) {
                        ++stats.cutoffs;
                        continue;
                    }
                    const double alpha = maximize ? threshold : -1;
                    const double beta = maximize ? 1 : threshold;
                    const int actionDepth = options.completeTurns ? depth : depth - 1;
                    move.bounds = Intersect(move.bounds, ActionBounds(s, episode, move.action, actionDepth, alpha, beta));
                    move.opened = true;
                    if (stopped) { complete = false; break; }
                }
            } else {
                std::atomic<std::size_t> next{0};
                std::atomic<bool> finished{true};
                std::mutex movesMutex;
                auto visit = [&](Impl& worker) {
                    while (worker.Budget()) {
                        const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                        if (i >= result.moves.size()) return;
                        GoalRootMove move;
                        double threshold;
                        bool skip;
                        {
                            std::lock_guard<std::mutex> lock(movesMutex);
                            const int incumbent = Best(result.moves, maximize);
                            threshold = maximize ? result.moves[incumbent].bounds.lower
                                                 : result.moves[incumbent].bounds.upper;
                            move = result.moves[i];
                            skip = static_cast<int>(i) != incumbent &&
                                   (maximize ? move.bounds.upper <= threshold
                                             : move.bounds.lower >= threshold);
                        }
                        if (skip) { ++worker.stats.cutoffs; continue; }
                        const GoalValueBounds value = worker.ActionBounds(
                            s, episode, move.action, options.completeTurns ? depth : depth - 1,
                            maximize ? threshold : -1, maximize ? 1 : threshold);
                        {
                            std::lock_guard<std::mutex> lock(movesMutex);
                            result.moves[i].bounds = Intersect(result.moves[i].bounds, value);
                            result.moves[i].opened = true;
                        }
                        if (worker.stopped || stopFlag.load(std::memory_order_relaxed)) {
                            finished.store(false, std::memory_order_relaxed);
                            return;
                        }
                    }
                    finished.store(false, std::memory_order_relaxed);
                };
                std::vector<std::thread> threads;
                threads.reserve(threadCount - 1);
                for (int i = 1; i < threadCount; ++i) threads.emplace_back(visit, std::ref(*workers[i]));
                visit(*this);
                for (std::thread& thread : threads) thread.join();
                complete = finished.load(std::memory_order_relaxed) &&
                           !stopFlag.load(std::memory_order_relaxed);
            }
            if (complete) {
                if (options.completeTurns) stats.completedTurnDepth = depth;
                else stats.completedActionDepth = depth;
            }
            const int chosen = Best(result.moves, maximize);
            if (chosen < 0 || Regret(result.moves, chosen, maximize) <= 0 || stopped ||
                stopFlag.load(std::memory_order_relaxed)) break;
        }
        for (std::size_t i = 1; i < workers.size(); ++i) {
            const Impl& worker = *workers[i];
            positions += worker.positions;
            expansions += worker.expansions;
            maxDepth = std::max(maxDepth, worker.maxDepth);
            memoryLimited = memoryLimited || worker.memoryLimited;
            peakBytes += worker.peakBytes;
            stats.cutoffs += worker.stats.cutoffs;
            stats.moveExpansions += worker.stats.moveExpansions;
            stats.passExpansions += worker.stats.passExpansions;
            stats.shotExpansions += worker.stats.shotExpansions;
            stats.attackExpansions += worker.stats.attackExpansions;
            stats.endTurnExpansions += worker.stats.endTurnExpansions;
            stats.ballStepExpansions += worker.stats.ballStepExpansions;
            stats.placeExpansions += worker.stats.placeExpansions;
            stats.outcomesProcessed += worker.stats.outcomesProcessed;
            stats.forcedPickupPrunes += worker.stats.forcedPickupPrunes;
            stats.nextStepPickupPrunes += worker.stats.nextStepPickupPrunes;
            stats.noRecoveryPrunes += worker.stats.noRecoveryPrunes;
            stats.cacheReplacements += worker.stats.cacheReplacements;
            stats.cacheHits += worker.stats.cacheHits;
            stats.selectiveMovementDrops += worker.stats.selectiveMovementDrops;
            stats.defenceForcedKept += worker.stats.defenceForcedKept;
            stats.selectiveActionDrops += worker.stats.selectiveActionDrops;
            stats.reachPassPrunes += worker.stats.reachPassPrunes;
            stats.reachMaps += worker.stats.reachMaps;
            stats.reachExtensions += worker.stats.reachExtensions;
            stats.lossChecks += worker.stats.lossChecks;
            stats.cacheEntries += worker.stats.cacheEntries;
            stats.selectiveTableCuts += worker.stats.selectiveTableCuts;
        }
        int chosen = Best(result.moves, maximize);
        if (options.selectiveWidth > 0) {
            chosen = -1;
            for (std::size_t index : selectedRoot) {
                if (!result.moves[index].opened) continue;
                if (chosen < 0 || (maximize ? result.moves[index].bounds.lower > result.moves[chosen].bounds.lower
                                           : result.moves[index].bounds.upper < result.moves[chosen].bounds.upper))
                    chosen = static_cast<int>(index);
            }
            if (chosen < 0 && !selectedRoot.empty()) chosen = static_cast<int>(selectedRoot.front());
        }
        if (chosen >= 0) {
            result.action = result.moves[chosen].action;
            if (options.selectiveWidth > 0) {
                result.turnPlan.push_back(result.action);
                const std::vector<Action>& continuation = rootPlans[chosen];
                result.turnPlan.insert(result.turnPlan.end(), continuation.begin(), continuation.end());
                if (!result.turnPlan.empty()) result.action = result.turnPlan.front();
            }
            result.value = result.moves[chosen].bounds;
            result.regretBound = options.selectiveWidth > 0 ? 2 : Regret(result.moves, chosen, maximize);
            result.proven = options.selectiveWidth == 0 && result.regretBound <= 0;
        }
        if (categoryRoot && !categoryPlan.empty()) {
            result.turnPlan = categoryPlan;
            result.action = categoryPlan.front();
            result.value = categoryValue;
            result.regretBound = 2;
            result.proven = false;
        }
        if (nodeRoot && !nodePlan.empty()) {
            result.turnPlan = nodePlan;
            result.action = nodePlan.front();
            result.value = nodeValue;
            result.regretBound = 2;
            result.proven = false;
        }
        result.rootBounds = base;
        if (!result.moves.empty()) {
            result.rootBounds = maximize ? GoalValueBounds{-1, -1} : GoalValueBounds{1, 1};
            for (const GoalRootMove& move : result.moves) {
                if (maximize) {
                    result.rootBounds.lower = std::max(result.rootBounds.lower, move.bounds.lower);
                    result.rootBounds.upper = std::max(result.rootBounds.upper, move.bounds.upper);
                } else {
                    result.rootBounds.lower = std::min(result.rootBounds.lower, move.bounds.lower);
                    result.rootBounds.upper = std::min(result.rootBounds.upper, move.bounds.upper);
                }
            }
        }
        if (options.selectiveWidth > 0) result.rootBounds = base;
        result.positions = positions;
        result.expansions = expansions;
        result.maxDepth = maxDepth;
        result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
        result.estimatedMemoryMB = peakBytes / 1048576.0;
        result.memoryLimited = memoryLimited;
        stats.peakMemoryMB = result.estimatedMemoryMB;
        char message[360];
        std::snprintf(message, sizeof message,
                      "%s [%.3f, %.3f] regret <= %.6g | %s, depth %d %s, %ld expansions, "
                      "%ld positions, %ld cuts, %ld pickup prunes, %ld next-step pickup prunes, "
                      "%ld no-recovery prunes, %ld cache replacements, %d threads, %.1f s, %.1f MB%s",
                      options.selectiveWidth > 0 ? "goal-team-selective" :
                      options.groupMovements ? "goal-team-moves" :
                      options.completeTurns ? "goal-team-dfs" : "goal-dfs",
                      result.value.lower, result.value.upper, result.regretBound,
                      result.proven ? "proven" : "unproven",
                      options.completeTurns ? stats.completedTurnDepth : stats.completedActionDepth,
                      options.completeTurns ? "team turns" : "actions",
                      expansions, positions, stats.cutoffs, stats.forcedPickupPrunes,
                      stats.nextStepPickupPrunes, stats.noRecoveryPrunes,
                      stats.cacheReplacements,
                      options.selectiveWidth > 0 ? std::max(1, options.threads) : threadCount,
                      result.seconds, result.estimatedMemoryMB, memoryLimited ? " (path memory limit)" : "");
        // The evaluation as in chess: positive - White is better, negative -
        // Black (the search's own value is the attacking team's).
        char eval[48];
        std::snprintf(eval, sizeof eval, "eval %+.3f | ",
                      episode.team == kWhite ? result.value.lower : -result.value.lower);
        result.note = std::string(eval) + message;
        if (options.profileRoot && !rootProfile.empty()) result.note += " | root seconds" + rootProfile;
        if (options.profileRoot) {
            char line[160];
            std::snprintf(line, sizeof line, " | batches: %ld (%ld jobs) %.2f s, small batches %.2f s, total %.2f s",
                          batchCount, batchJobs, batchSeconds, smallBatchSeconds, result.seconds);
            result.note += line;
            batchSeconds = smallBatchSeconds = 0;
            batchJobs = batchCount = 0;
        }
        if (pool)
            result.note += " | parallel estimates: " + std::to_string(pool->size) + " slots, " +
                           std::to_string(pool->batches) + " batches, " + std::to_string(pool->parallelJobs) + " jobs";
        if (options.selectiveGeometricStatic && options.selectiveStaticCacheBits > 0) {
            long calls = staticCalls, hits = staticHits;
            for (std::size_t i = 1; i < workers.size(); ++i) {
                calls += workers[i]->staticCalls;
                hits += workers[i]->staticHits;
            }
            char cacheNote[96];
            std::snprintf(cacheNote, sizeof cacheNote, " | static cache: %ld of %ld hits", hits, calls);
            result.note += cacheNote;
        }
        if (options.selectiveBreakawayBound) {
            long leaves = breakawayLeaves, raised = breakawayRaised;
            for (std::size_t i = 1; i < workers.size(); ++i) {
                leaves += workers[i]->breakawayLeaves;
                raised += workers[i]->breakawayRaised;
            }
            result.note += " | breakaway leaves " + std::to_string(leaves) + ", raised " + std::to_string(raised);
        }
        if (options.selectiveEpisodeTable) {
            long leaves = tableLeaves, raised = tableRaised, incomplete = tableIncomplete;
            for (std::size_t i = 1; i < workers.size(); ++i) {
                leaves += workers[i]->tableLeaves;
                raised += workers[i]->tableRaised;
                incomplete += workers[i]->tableIncomplete;
            }
            result.note += " | episode table leaves " + std::to_string(leaves) + ", raised " + std::to_string(raised) +
                           ", incomplete " + std::to_string(incomplete);
        }
        if (options.selectiveTwoVsOneBound) {
            long leaves = twoVsOneLeaves, raised = twoVsOneRaised;
            for (std::size_t i = 1; i < workers.size(); ++i) {
                leaves += workers[i]->twoVsOneLeaves;
                raised += workers[i]->twoVsOneRaised;
            }
            result.note += " | 2 v 1 leaves " + std::to_string(leaves) + ", above the 1 v 0 bound " + std::to_string(raised);
        }
        if (options.selectiveEpisodeExtension) {
            long extended = episodeExtensionCount;
            for (std::size_t i = 1; i < workers.size(); ++i) extended += workers[i]->episodeExtensionCount;
            result.note += " | episode extensions " + std::to_string(extended);
        }
        if (options.selectiveBallRace) {
            long counts[5] = {raceLeaves[0], raceLeaves[1], raceLeaves[2], raceLeaves[3], raceLeaves[4]};
            for (std::size_t i = 1; i < workers.size(); ++i)
                for (int j = 0; j < 5; ++j) counts[j] += workers[i]->raceLeaves[j];
            char race[200];
            std::snprintf(race, sizeof race,
                          " | ball race leaves: taken %ld (late %ld), restart %ld, contested %ld, extended %ld",
                          counts[0], counts[4], counts[1], counts[2], counts[3]);
            result.note += race;
            long hits = raceCacheHits, calls = raceCacheCalls;
            for (std::size_t i = 1; i < workers.size(); ++i) {
                hits += workers[i]->raceCacheHits;
                calls += workers[i]->raceCacheCalls;
            }
            result.note += " | race cache: " + std::to_string(hits) + " of " + std::to_string(calls) + " hits";
        }
        if (options.selectiveWidth > 0) {
            result.note += std::string(" | heuristic ") +
                (options.selectiveGreedyMovements ? "greedy-team-movement" :
                 "partial-movement") +
                ", preview " + (options.selectiveEndTurnPreview ? (options.selectiveEndTurnPreviewTop > 0 ? "top " + std::to_string(options.selectiveEndTurnPreviewTop) : std::string("on")) : std::string("off")) +
                ", ball diversity " + (options.selectiveBallDiversity ? "on" : "off") +
                ", alpha-beta " + (options.selectiveAlphaBeta ? "on" : "off") +
                ", transpositions " + (options.selectiveTransposition ? "on" : "off") +
                ", restart " + (options.selectiveRestartPosition ? "position" : "0") +
                ", ball step " + (options.selectiveBallStepPreview ? "on" : "off") +
                ", kind diversity " + (options.selectiveKindDiversity ? "on" : "off") +
                ", race " + (options.selectiveRaceFreeBall ? "on" : "off") +
                ", reply-aware " + (options.selectiveReplyAware ? "on" : "off") +
                ", rare outcomes static below " + std::to_string(options.selectiveRareOutcomeProbability) +
                ", continuation " + (options.selectiveContinuation ? "on" : "off") +
                ", support horizon " + std::to_string(options.selectiveSupportHorizon) +
                ", attack episode " + (options.selectiveAttackEpisode ? "on" : "off") +
                ", race defence " + (options.selectiveRaceDefence ? "on" : "off") +
                ", " + (options.selectiveTree ? std::string("tree") : std::string("beam")) +
                " width " + std::to_string(options.selectiveWidth) +
                ", dropped movement states " + std::to_string(stats.selectiveMovementDrops) +
                ", dropped actions " + std::to_string(stats.selectiveActionDrops) +
                (options.selectiveReachPassPrune ? ", reach-pruned passes " + std::to_string(stats.reachPassPrunes) : std::string()) +
                (options.selectiveDefenceForced ? ", defence forced " + std::to_string(stats.defenceForcedKept) : std::string()) +
                (options.selectiveReachExtension ? ", reach extensions " + std::to_string(stats.reachExtensions) : std::string()) +
                (options.selectiveLossReachCheck ? ", lost balls searched " + std::to_string(stats.lossChecks) : std::string()) +
                (stats.reachMaps ? ", reach maps " + std::to_string(stats.reachMaps) : std::string()) +
                ", table hits " + std::to_string(stats.cacheHits) +
                ", table cuts " + std::to_string(stats.selectiveTableCuts) +
                "; no proof";
        }
        return result;
    }
};

void SetNodeDiag(const NodeDiagTarget& target) { gNodeDiag = target; }
GoalDfsSearch::GoalDfsSearch(const GoalDfsOptions& options) : impl_(new Impl(options)) {}
GoalDfsSearch::~GoalDfsSearch() = default;
GoalSearchResult GoalDfsSearch::Search(const GameState& s, double seconds) { return impl_->Search(s, seconds); }
GoalDfsStats GoalDfsSearch::Stats() const { return impl_->stats; }
double GoalDfsSearch::SelectiveEstimate(const GameState& s, int team, bool afterEndTurn) const {
    const GoalEpisode e{team, GoalEpisodeStatus::OurAttack};
    return afterEndTurn ? impl_->ActionEstimate(s, e, Action::EndTurn(), true, true) : impl_->Estimate(s, e, true, true);
}
double GoalDfsSearch::SelectiveLeafEstimate(const GameState& s, int team, bool afterEndTurn) const {
    const GoalEpisode e{team, GoalEpisodeStatus::OurAttack};
    return afterEndTurn ? impl_->ActionEstimate(s, e, Action::EndTurn(), true, true, true)
                        : impl_->Estimate(s, e, true, true, false, true);
}

}  // namespace bot
}  // namespace soccer
