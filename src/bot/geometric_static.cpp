#include "bot/geometric_static.h"
#include "bot/goal_dfs_search.h"

#include "engine/rules.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace soccer {
namespace bot {
namespace {

struct Area {
    Cell cells[10];
    int count = 0;
};

// Ranking a partial team turn (bot_design.md 4.3, 29.10.2026): the team to
// move is credited only with the moves it has made; its players who have not
// moved yet stay where they stand, so a move that prepares a tackle or a shot
// is worth more than the mere possibility of making it.
thread_local bool tRealizedOnly = false;
// Possession value (07.11.2026, variant 4 of the user's choice; 0 = off): on
// the scale "who scores next" a held ball is worth t + (1 - t) W on top of the
// chance t the static finds: when the chance does not come off, the
// possession goes on (small chances keep their differences). W is weighted by the chance k to keep the ball
// until the holder's next action: 2k - 1 (1 when his team moves and has not
// played the ball; with the other team to move, the tackles it can make on
// him where he stands). A shot gives the ball away: p (1 + W) - W. Set per
// estimate by the caller (SetGeometricPossessionValue).
thread_local double tPossession = 0;

Area PlayerArea(const GameState& s, int player) {
    Area area;
    const PlayerState& p = s.players[player];
    area.cells[area.count++] = p.pos;
    if (TeamOf(player) == s.teamToMove && (p.moved || tRealizedOnly)) return area;
    // A player who moved on the previous team turn is free to move again on
    // the next one. His new inertia comes from that completed displacement.
    const Cell velocity = p.moved ? p.Displacement() : p.Velocity();
    const Cell center{p.pos.x + Sign(velocity.x),
                      p.pos.y + Sign(velocity.y)};
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const Cell c{center.x + dx, center.y + dy};
            if (InField(c) && c != p.pos) area.cells[area.count++] = c;
        }
    return area;
}

double SegmentDistanceSquared(Cell a, Cell b, Cell p) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double length2 = dx * dx + dy * dy;
    if (length2 == 0) {
        const double x = p.x - a.x, y = p.y - a.y;
        return x * x + y * y;
    }
    const double t = std::max(0.0, std::min(1.0,
        ((p.x - a.x) * dx + (p.y - a.y) * dy) / length2));
    const double x = p.x - a.x - t * dx, y = p.y - a.y - t * dy;
    return x * x + y * y;
}

// ---- Precomputed geometry (same values, computed once) ----------------------
// Both quantities depend only on cell differences, so the tables hold exactly
// the values the direct formulas give; arguments outside a table are computed
// directly.

// 1 - InterceptionWeight(distance from d to the segment a-b) for a pass a-b.
struct CrossTable {
    static constexpr int kPass = kMaxPassDistance, kSpan = 12;
    double v[2 * kPass + 1][2 * kPass + 1][2 * kSpan + 1][2 * kSpan + 1];
    CrossTable() {
        const Cell a{0, 0};
        for (int dx = -kPass; dx <= kPass; ++dx)
            for (int dy = -kPass; dy <= kPass; ++dy)
                for (int ex = -kSpan; ex <= kSpan; ++ex)
                    for (int ey = -kSpan; ey <= kSpan; ++ey)
                        v[dx + kPass][dy + kPass][ex + kSpan][ey + kSpan] =
                            1 - InterceptionWeight(std::sqrt(SegmentDistanceSquared(a, {dx, dy}, {ex, ey})));
    }
};
const CrossTable kCrossTable;

// The crossing of a pass a-b by a player on d, as CoverageLink counts it: 0 on
// the target cell, 1 beside the segment's span, otherwise CrossFactor.
struct PassCrossingTable {
    double v[2 * CrossTable::kPass + 1][2 * CrossTable::kPass + 1][2 * CrossTable::kSpan + 1][2 * CrossTable::kSpan + 1];
    PassCrossingTable() {
        constexpr int P = CrossTable::kPass, S = CrossTable::kSpan;
        for (int dx = -P; dx <= P; ++dx)
            for (int dy = -P; dy <= P; ++dy)
                for (int ex = -S; ex <= S; ++ex)
                    for (int ey = -S; ey <= S; ++ey) {
                        double& out = v[dx + P][dy + P][ex + S][ey + S];
                        const int dot = ex * dx + ey * dy;
                        if (ex == dx && ey == dy) out = 0.0;
                        else if (dot < 0 || dot > dx * dx + dy * dy) out = 1.0;
                        else out = 1 - InterceptionWeight(std::sqrt(SegmentDistanceSquared({0, 0}, {dx, dy}, {ex, ey})));
                    }
    }
};

inline double CrossFactor(Cell a, Cell b, Cell d) {
    const int dx = b.x - a.x, dy = b.y - a.y, ex = d.x - a.x, ey = d.y - a.y;
    constexpr int P = CrossTable::kPass, S = CrossTable::kSpan;
    if (dx >= -P && dx <= P && dy >= -P && dy <= P && ex >= -S && ex <= S && ey >= -S && ey <= S)
        return kCrossTable.v[dx + P][dy + P][ex + S][ey + S];
    return 1 - InterceptionWeight(std::sqrt(SegmentDistanceSquared(a, b, d)));
}

const PassCrossingTable kPassCrossingTable;

inline double PassCrossing(Cell a, Cell b, Cell d) {
    const int dx = b.x - a.x, dy = b.y - a.y, ex = d.x - a.x, ey = d.y - a.y;
    constexpr int P = CrossTable::kPass, S = CrossTable::kSpan;
    if (dx >= -P && dx <= P && dy >= -P && dy <= P && ex >= -S && ex <= S && ey >= -S && ey <= S)
        return kPassCrossingTable.v[dx + P][dy + P][ex + S][ey + S];
    if (d == b) return 0.0;
    const int dot = ex * dx + ey * dy;
    if (dot < 0 || dot > dx * dx + dy * dy) return 1.0;
    return 1 - InterceptionWeight(std::sqrt(SegmentDistanceSquared(a, b, d)));
}

// Squared distance from p to the segment from c to the goal cell in front of
// it, {clamp(c.x, posts), goal row}, and BlockFactor of that distance.
struct GoalRouteTable {
    double d2[kNumTeams][kFieldWidth][kGridHeight][kFieldWidth][kGridHeight];
    double block[kNumTeams][kFieldWidth][kGridHeight][kFieldWidth][kGridHeight];
    GoalRouteTable() {
        for (int team = 0; team < kNumTeams; ++team)
            for (int cx = 0; cx < kFieldWidth; ++cx)
                for (int cy = kFirstRow; cy <= kLastRow; ++cy) {
                    const Cell c{cx, cy};
                    const Cell goal{std::max(kGoalLeft, std::min(kGoalRight, cx)), team == kWhite ? kFirstRow : kLastRow};
                    for (int px = 0; px < kFieldWidth; ++px)
                        for (int py = kFirstRow; py <= kLastRow; ++py) {
                            const double value = SegmentDistanceSquared(c, goal, {px, py});
                            d2[team][cx][cy][px][py] = value;
                            block[team][cx][cy][px][py] = BlockFactor(std::sqrt(value));
                        }
                }
    }
};
const GoalRouteTable kGoalRouteTable;

// The team whose goal route from c ends at `goal`, or -1 if `goal` is not the
// tabulated goal cell of c.
inline int GoalRouteTeam(Cell c, Cell goal) {
    if (!InField(c) || goal.x != std::max(kGoalLeft, std::min(kGoalRight, c.x))) return -1;
    return goal.y == kFirstRow ? kWhite : goal.y == kLastRow ? kBlack : -1;
}
inline double GoalSegmentDistanceSquared(Cell c, Cell goal, Cell p) {
    const int team = GoalRouteTeam(c, goal);
    if (team < 0 || !InField(p)) return SegmentDistanceSquared(c, goal, p);
    return kGoalRouteTable.d2[team][c.x][c.y][p.x][p.y];
}
// BlockFactor(sqrt(GoalSegmentDistanceSquared(c, goal, p))).
inline double GoalSegmentBlock(Cell c, Cell goal, Cell p) {
    const int team = GoalRouteTeam(c, goal);
    if (team < 0 || !InField(p)) return BlockFactor(std::sqrt(SegmentDistanceSquared(c, goal, p)));
    return kGoalRouteTable.block[team][c.x][c.y][p.x][p.y];
}

// The products f_0 * f_1 * ... over the defenders for every mask of acting
// defenders (bit d: defender d acts), multiplied in the order d = 0, 1, ...
// as a plain loop would, sharing the common prefixes.
inline void MaskProducts(double start, const double stay[kPlayersPerTeam], const double act[kPlayersPerTeam],
                         double out[1 << kPlayersPerTeam]) {
    double level[1 << kPlayersPerTeam];
    out[0] = start;
    int size = 1;
    for (int d = 0; d < kPlayersPerTeam; ++d) {
        for (int m = 0; m < size; ++m) {
            level[m] = out[m] * stay[d];
            level[m | size] = out[m] * act[d];
        }
        size <<= 1;
        for (int m = 0; m < size; ++m) out[m] = level[m];
    }
}

// A future defender may vacate his current cell. One inertia step gives a
// conservative indication of whether he can enter the runner's route.
double RouteDefence(int runner, Cell from, Cell goal, const Area areas[kNumPlayers]) {
    const int team = TeamOf(runner);
    double factor = 1;
    for (int k = FirstPlayerOf(Opponent(team));
         k < FirstPlayerOf(Opponent(team)) + kPlayersPerTeam; ++k) {
        const Area& area = areas[k];
        double nearestSquared = 100;
        for (int i = 0; i < area.count; ++i) {
            const Cell d = area.cells[i];
            const bool ahead = team == kWhite ? d.y <= from.y : d.y >= from.y;
            if (ahead) nearestSquared = std::min(nearestSquared,
                                                 GoalSegmentDistanceSquared(from, goal, d));
        }
        if (nearestSquared <= 1) factor *= BlockFactor(std::sqrt(nearestSquared));
    }
    return factor;
}

double PlayerThreat(const GameState& s, int player, const Area areas[kNumPlayers]) {
    const int team = TeamOf(player);
    const Area& area = areas[player];
    const int goalRow = team == kWhite ? kFirstRow : kLastRow;
    double best = 0;
    for (int i = 0; i < area.count; ++i) {
        const Cell c = area.cells[i];
        double shot = OpenShotProbability(c, team);
        for (int k = 0; k < kNumPlayers; ++k)
            if (k != player) shot *= ShotBlockFactor(c, team, s.players[k].pos);
        const Cell goal{std::max(kGoalLeft, std::min(kGoalRight, c.x)), goalRow};
        const int cells = ChebyshevDistance(c, goal);
        const int turns = (cells + 1) / 2;  // at most two cells per own move
        const double run = OpenShotProbability(goal, team) *
                           RouteDefence(player, c, goal, areas) / (1.0 + turns);
        best = std::max(best, std::max(shot, run));
    }
    return best;
}

// A qualitative area-to-area pass. The pass must be legal from the chosen
// source to the chosen receiving cell. Only an opponent touching the first
// segment destroys this particular link; teammate touches create other links.
double PassLink(const GameState& s, int source,
                const Area& from, const Area& to) {
    double best = 0;
    const int defending = Opponent(TeamOf(source));
    for (int i = 0; i < from.count && best < 1; ++i)
        for (int j = 0; j < to.count; ++j) {
            const Cell a = from.cells[i], b = to.cells[j];
            if (a == b || ChebyshevDistance(a, b) > kMaxPassDistance) continue;
            double through = 1;
            for (int k = FirstPlayerOf(defending);
                 k < FirstPlayerOf(defending) + kPlayersPerTeam; ++k) {
                const Cell d = s.players[k].pos;
                if (d == b) { through = 0; break; }
                const int dx = b.x - a.x, dy = b.y - a.y;
                const int dot = (d.x - a.x) * dx + (d.y - a.y) * dy;
                if (dot < 0 || dot > dx * dx + dy * dy) continue;
                through *= CrossFactor(a, b, d);
            }
            // A receiver already in tackle range may lose the ball on reply.
            double receive = 1;
            for (int k = FirstPlayerOf(defending);
                 k < FirstPlayerOf(defending) + kPlayersPerTeam; ++k)
                if (ChebyshevDistance(s.players[k].pos, b) == 1)
                    receive *= 1 - TackleChance(b, s.players[k].pos, defending);
            best = std::max(best, through * receive);
            if (best >= 1) break;
        }
    return best;
}

struct AttackGraph {
    double threat[4] = {};
    double link[4][4] = {};
};

AttackGraph BuildGraph(const GameState& s, int team) {
    AttackGraph graph;
    Area area[kNumPlayers];
    const int first = FirstPlayerOf(team);
    for (int k = 0; k < kNumPlayers; ++k) area[k] = PlayerArea(s, k);
    for (int i = 0; i < 4; ++i) {
        graph.threat[i] = PlayerThreat(s, first + i, area);
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (i != j) graph.link[i][j] = PassLink(s, first + i,
                                                    area[first + i], area[first + j]);
    return graph;
}

double AttackValue(const AttackGraph& graph, int holder, bool exclusiveChannels) {
    if (exclusiveChannels) {
        // A team can play the ball once this turn. Choose one current channel;
        // a second pass needs another turn and a reply from the opponent.
        double best = graph.threat[holder];
        for (int j = 0; j < 4; ++j)
            if (j != holder)
                best = std::max(best, graph.link[holder][j] * graph.threat[j]);
        return best;
    }
    double miss = 1 - graph.threat[holder];
    for (int j = 0; j < 4; ++j) {
        if (j == holder) continue;
        double delivery = graph.link[holder][j];
        for (int k = 0; k < 4; ++k)
            if (k != holder && k != j)
                delivery = std::max(delivery,
                                    graph.link[holder][k] * graph.link[k][j]);
        miss *= 1 - delivery * graph.threat[j];
    }
    return 1 - miss;
}

double TackleRisk(const GameState& s, int defender) {
    const Cell ball = s.ball.pos;
    const PlayerState& p = s.players[defender];
    const bool nextTurn = TeamOf(defender) != s.teamToMove;
    const Cell velocity = nextTurn && p.moved ? p.Displacement() : p.Velocity();
    const Cell start = nextTurn ? p.pos : p.turnStart;
    const Area area = PlayerArea(s, defender);
    double best = 0;
    for (int i = 0; i < area.count; ++i) {
        const Cell c = area.cells[i];
        if (ChebyshevDistance(c, ball) != 1) continue;
        if (!AttackDirectionAllowed(c, ball, velocity,
                                    {c.x - start.x, c.y - start.y})) continue;
        best = std::max(best, TackleChance(ball, c, TeamOf(defender)));
    }
    return best;
}

// The opponent's next turn begins with the same movement state: the engine
// resets a team's flags at the end of its own turn. Check the actual move
// corridor and attack direction instead of the unblocked geometric area.
double LegalTackleRisk(const GameState& s, int defender) {
    if (s.players[defender].attacked) return 0;
    GameState pre = s;
    pre.teamToMove = TeamOf(defender);
    double best = 0;
    const auto consider = [&](const GameState& position, Cell cell, double& result) {
        if (GetAttackKind(position, defender) == AttackKind::Tackle)
            result = std::max(result, TackleChance(position.ball.pos, cell, position.teamToMove));
    };
    consider(pre, pre.players[defender].pos, best);
    if (s.players[defender].moved ||
        ChebyshevDistance(s.players[defender].pos, s.ball.pos) > 3) return best;
    for (Cell cell : MoveDestinations(pre, defender)) {
        if (cell == pre.players[defender].pos) continue;
        GameState moved = pre;
        ApplyUnchecked(moved, Action::Move(defender, cell), Outcome::Certain());
        consider(moved, cell, best);
    }
    return best;
}

// ---- Turn order (bot_design.md 4.3, 28.10.2026) ----------------------------
// When the defence moves before the attack can act again, each defender may
// first take one inertia step. Every quantity below lets each defender choose
// his best cell of that step for the particular channel; one defender cannot be
// in two places, so the union of channels remains a qualitative estimate.

// The shot from c after the defence's reply: opponents block from their best
// reachable cell (occupying c removes the cell), teammates stay where they are.
double ReplyShot(const GameState& s, int player, Cell c, const Area areas[kNumPlayers]) {
    const int team = TeamOf(player);
    double shot = OpenShotProbability(c, team);
    for (int k = 0; k < kNumPlayers && shot > 0; ++k) {
        if (k == player) continue;
        if (TeamOf(k) == team) { shot *= ShotBlockFactor(c, team, s.players[k].pos); continue; }
        double factor = 1;
        for (int i = 0; i < areas[k].count; ++i) {
            const Cell d = areas[k].cells[i];
            factor = std::min(factor, d == c ? 0.0 : ShotBlockFactor(c, team, d));
        }
        shot *= factor;
    }
    return shot;
}

// PlayerThreat with the shot evaluated after the defence's reply.
double ReplyThreat(const GameState& s, int player, const Area areas[kNumPlayers]) {
    const int team = TeamOf(player);
    const Area& area = areas[player];
    const int goalRow = team == kWhite ? kFirstRow : kLastRow;
    double best = 0;
    for (int i = 0; i < area.count; ++i) {
        const Cell c = area.cells[i];
        const Cell goal{std::max(kGoalLeft, std::min(kGoalRight, c.x)), goalRow};
        const int turns = (ChebyshevDistance(c, goal) + 1) / 2;
        const double run = OpenShotProbability(goal, team) *
                           RouteDefence(player, c, goal, areas) / (1.0 + turns);
        best = std::max(best, std::max(ReplyShot(s, player, c, areas), run));
    }
    return best;
}

// The best tackle of one defender against the ball at cell `ball` after his
// next inertia step (the cheap geometric area, direction rule included).
double TackleRiskAt(const GameState& s, int defender, Cell ball) {
    const PlayerState& p = s.players[defender];
    const bool nextTurn = TeamOf(defender) != s.teamToMove;
    const Cell velocity = nextTurn && p.moved ? p.Displacement() : p.Velocity();
    const Cell start = nextTurn ? p.pos : p.turnStart;
    const Area area = PlayerArea(s, defender);
    double best = 0;
    for (int i = 0; i < area.count; ++i) {
        const Cell c = area.cells[i];
        if (ChebyshevDistance(c, ball) != 1) continue;
        if (!AttackDirectionAllowed(c, ball, velocity,
                                    {c.x - start.x, c.y - start.y})) continue;
        best = std::max(best, TackleChance(ball, c, TeamOf(defender)));
    }
    return best;
}

// A pass from area to area. The first segment is crossed by defenders where
// they stand (`throughNow`, a pass played before the reply) or from their best
// reachable cell (a pass played after the reply). The receiver is always
// exposed to the reply that follows the delivery.
double ReplyPassLink(const GameState& s, int source, const Area& from, const Area& to,
                     const Area areas[kNumPlayers], bool throughNow) {
    double best = 0;
    const int defending = Opponent(TeamOf(source));
    const int first = FirstPlayerOf(defending);
    for (int i = 0; i < from.count && best < 1; ++i)
        for (int j = 0; j < to.count; ++j) {
            const Cell a = from.cells[i], b = to.cells[j];
            if (a == b || ChebyshevDistance(a, b) > kMaxPassDistance) continue;
            const int dx = b.x - a.x, dy = b.y - a.y;
            double through = 1;
            for (int k = first; k < first + kPlayersPerTeam && through > 0; ++k) {
                const int n = throughNow ? 1 : areas[k].count;
                double pass = 1;  // the defender's best cell for this segment
                for (int m = 0; m < n; ++m) {
                    const Cell d = throughNow ? s.players[k].pos : areas[k].cells[m];
                    if (d == b) { pass = 0; break; }
                    const int dot = (d.x - a.x) * dx + (d.y - a.y) * dy;
                    if (dot < 0 || dot > dx * dx + dy * dy) continue;
                    pass = std::min(pass, CrossFactor(a, b, d));
                }
                through *= pass;
            }
            double receive = 1;
            for (int k = first; k < first + kPlayersPerTeam; ++k) {
                double tackle = 0;
                for (int m = 0; m < areas[k].count; ++m)
                    if (ChebyshevDistance(areas[k].cells[m], b) == 1)
                        tackle = std::max(tackle, TackleChance(b, areas[k].cells[m], defending));
                receive *= 1 - tackle;
            }
            best = std::max(best, through * receive);
            if (best >= 1) break;
        }
    return best;
}

double TurnOrderEstimate(const GameState& s, GoalEpisode episode, int attack,
                         bool exclusiveChannels) {
    const int defending = Opponent(attack);
    const int firstA = FirstPlayerOf(attack), firstD = FirstPlayerOf(defending);
    const int h = s.ball.holder - firstA;
    Area areas[kNumPlayers];
    for (int k = 0; k < kNumPlayers; ++k) areas[k] = PlayerArea(s, k);

    // The attack moves first: the holder is not tackled before acting and ends
    // the turn on the cell of his area that is least exposed to the reply.
    // Otherwise he stays where he is and faces the reply now.
    const bool attackMoves = s.teamToMove == attack;
    const bool passNow = attackMoves && !s.ballPlayed;
    double won[kPlayersPerTeam] = {};
    double keep = 1;
    {
        Area escape;
        if (attackMoves) escape = areas[s.ball.holder];
        else escape.cells[escape.count++] = s.ball.pos;
        double bestKeep = -1;
        for (int i = 0; i < escape.count; ++i) {
            double w[kPlayersPerTeam], k = 1;
            for (int d = 0; d < kPlayersPerTeam; ++d) {
                w[d] = TackleRiskAt(s, firstD + d, escape.cells[i]);
                if (w[d] > 0) k *= 1 - w[d] - kKnockAwayChance;
            }
            if (k > bestKeep) {
                bestKeep = k;
                std::copy(w, w + kPlayersPerTeam, won);
            }
        }
        keep = std::max(0.0, bestKeep);
    }

    double threat[4], now[4][4] = {}, later[4][4] = {};
    for (int i = 0; i < 4; ++i) threat[i] = ReplyThreat(s, firstA + i, areas);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            if (i == j) continue;
            later[i][j] = ReplyPassLink(s, firstA + i, areas[firstA + i], areas[firstA + j],
                                        areas, false);
            if (passNow && i == h)
                now[i][j] = ReplyPassLink(s, firstA + i, areas[firstA + i], areas[firstA + j],
                                          areas, true);
        }

    const double hold = keep * threat[h];
    double best = hold, miss = 1 - hold;
    for (int j = 0; j < 4; ++j) {
        if (j == h) continue;
        double delivery = passNow ? now[h][j] : later[h][j];
        for (int k = 0; k < 4; ++k)
            if (k != h && k != j)
                delivery = std::max(delivery, (passNow ? now[h][k] : later[h][k]) * later[k][j]);
        if (!passNow) delivery *= keep;
        best = std::max(best, delivery * threat[j]);
        miss *= 1 - delivery * threat[j];
    }
    const double opportunity = exclusiveChannels ? best : 1 - miss;

    double counter = 0;
    if (episode.status == GoalEpisodeStatus::OurAttack) {
        double noCounter = 1;
        bool haveReply = false;
        AttackGraph reply;
        for (int d = 0; d < kPlayersPerTeam; ++d) {
            if (won[d] == 0) continue;
            if (!haveReply) { reply = BuildGraph(s, defending); haveReply = true; }
            noCounter *= 1 - won[d] * AttackValue(reply, d, exclusiveChannels);
        }
        counter = 1 - noCounter;
    }
    return opportunity - counter;
}

// ---- Coverage (bot_design.md 4.3, 29.10.2026) -------------------------------
// The defence covers only what the attack can do before the defence moves
// again. When the defence moves first, it commits without knowing which ball
// action follows: each defender takes one task — tackle the holder, block the
// holder's shot or run, or stand in one passing lane — reaching his best cell
// of one inertia step only for that task; elsewhere he stays an obstacle where
// he stands. The attack then takes its best remaining channel. Whatever comes
// after the next reply (the receiver's shot, a second pass) meets every
// defender, because the defence has seen the ball action. When the attack
// moves first, its ball action precedes the reply and every defender answers
// it. Masks: bit d = defender d acts on this channel.

constexpr int kMasks = 1 << kPlayersPerTeam;

// ---- Participation in time (bot_design.md 4.3, 29.10.2026) -----------------
// Per axis a move changes the displacement by at most one from the direction
// of the previous one (|displacement| <= 2), see MoveDestinations. The table
// gives the fewest own moves to shift a coordinate by D starting with axis
// velocity v; the axes are taken independently (a lower bound).
constexpr int kAxisReach = 40;
struct AxisTable {
    unsigned char turns[5][2 * kAxisReach + 1];
    AxisTable() {
        for (int v0 = -2; v0 <= 2; ++v0) {
            unsigned char* row = turns[v0 + 2];
            for (int d = 0; d <= 2 * kAxisReach; ++d) row[d] = 255;
            bool seen[2 * kAxisReach + 1][5] = {};
            std::vector<std::pair<int, int>> frontier{{0, v0}};
            seen[kAxisReach][v0 + 2] = true;
            row[kAxisReach] = 0;
            for (int step = 1; step < 255 && !frontier.empty(); ++step) {
                std::vector<std::pair<int, int>> next;
                for (const auto& [x, v] : frontier) {
                    const int base = Sign(v);
                    for (int nv = base - 1; nv <= base + 1; ++nv) {
                        if (nv < -2 || nv > 2) continue;
                        const int nx = x + nv;
                        if (nx < -kAxisReach || nx > kAxisReach || seen[nx + kAxisReach][nv + 2]) continue;
                        seen[nx + kAxisReach][nv + 2] = true;
                        if (row[nx + kAxisReach] == 255) row[nx + kAxisReach] = static_cast<unsigned char>(step);
                        next.push_back({nx, nv});
                    }
                }
                frontier.swap(next);
            }
        }
    }
    int Turns(int v, int d) const {
        v = std::max(-2, std::min(2, v));
        d = std::max(-kAxisReach, std::min(kAxisReach, d));
        return turns[v + 2][d + kAxisReach];
    }
};
const AxisTable kAxisTable;
inline const AxisTable& Axes() { return kAxisTable; }

// Moment of a player's k-th next move as 2*round + order: the team to move
// acts first in every round. A player who already moved this turn waits for
// the next round. Returns the moment of arrival at `target` (0 if he is there).
struct Mover { Cell from; Cell velocity; int first; };
Mover MoverOf(const GameState& s, int k) {
    const PlayerState& p = s.players[k];
    const bool toMove = TeamOf(k) == s.teamToMove;
    const int order = toMove ? 0 : 1;
    const int round = toMove && p.moved ? 2 : 1;
    return {p.pos, p.moved ? p.Displacement() : p.Velocity(), 2 * round + order};
}
int ArrivalMoment(const Mover& m, Cell target) {
    if (m.from == target) return 0;
    const int turns = std::max(Axes().Turns(m.velocity.x, target.x - m.from.x),
                               Axes().Turns(m.velocity.y, target.y - m.from.y));
    return m.first + 2 * (turns - 1);
}

// Threat of attacker `player` (shot or run from his area) for every set of
// acting defenders.
void CoverageThreat(const GameState& s, int player, const Area areas[kNumPlayers],
                    double out[kMasks], bool timedRoute = false) {
    const int team = TeamOf(player);
    const int firstD = FirstPlayerOf(Opponent(team));
    const int goalRow = team == kWhite ? kFirstRow : kLastRow;
    std::fill(out, out + kMasks, 0.0);
    const Area& area = areas[player];
    for (int i = 0; i < area.count; ++i) {
        const Cell c = area.cells[i];
        double open = OpenShotProbability(c, team);
        for (int k = 0; k < kNumPlayers; ++k)
            if (k != player && TeamOf(k) == team)
                open *= ShotBlockFactor(c, team, s.players[k].pos);
        const Cell goal{std::max(kGoalLeft, std::min(kGoalRight, c.x)), goalRow};
        const int turns = (ChebyshevDistance(c, goal) + 1) / 2;
        const double runOpen = OpenShotProbability(goal, team) / (1.0 + turns);
        double shotStay[kPlayersPerTeam], shotAct[kPlayersPerTeam];
        double runStay[kPlayersPerTeam], runAct[kPlayersPerTeam];
        const auto route = [&](Cell x) {
            const bool ahead = team == kWhite ? x.y <= c.y : x.y >= c.y;
            if (!ahead) return 1.0;
            const double d2 = GoalSegmentDistanceSquared(c, goal, x);
            return d2 <= 1 ? GoalSegmentBlock(c, goal, x) : 1.0;
        };
        // The runner reaches his area cell c on his first move and continues
        // with the velocity of that move.
        std::vector<std::pair<Cell, int>> routeCells;  // cell near the route ahead, runner's moment
        if (timedRoute) {
            const Mover runner = MoverOf(s, player);
            const Mover onward{c, {c.x - runner.from.x, c.y - runner.from.y},
                               c == runner.from ? runner.first : runner.first + 2};
            const int lowY = std::min(c.y, goal.y), highY = std::max(c.y, goal.y);
            const int lowX = std::max(0, std::min(c.x, goal.x) - 1), highX = std::min(kFieldWidth - 1, std::max(c.x, goal.x) + 1);
            for (int y = lowY; y <= highY; ++y)
                for (int x = lowX; x <= highX; ++x) {
                    const Cell r{x, y};
                    if (!InField(r) || r == c) continue;
                    if (GoalSegmentDistanceSquared(c, goal, r) > 1) continue;
                    routeCells.push_back({r, ArrivalMoment(onward, r)});
                }
        }
        for (int d = 0; d < kPlayersPerTeam; ++d) {
            const int k = firstD + d;
            const Cell pos = s.players[k].pos;
            shotStay[d] = pos == c ? 0.0 : ShotBlockFactor(c, team, pos);
            runStay[d] = route(pos);
            shotAct[d] = shotStay[d];
            runAct[d] = runStay[d];
            for (int m = 0; m < areas[k].count; ++m) {
                const Cell x = areas[k].cells[m];
                shotAct[d] = std::min(shotAct[d], x == c ? 0.0 : ShotBlockFactor(c, team, x));
                runAct[d] = std::min(runAct[d], route(x));
            }
            if (timedRoute) {
                // A defender blocks the run where he arrives no later than the
                // runner. Kept on his task elsewhere, he loses his first move
                // when the defence moves first.
                const Mover defender = MoverOf(s, k);
                const int delay = s.teamToMove == TeamOf(k) && !s.players[k].moved ? 2 : 0;
                double act = 1, stay = 1;
                for (const auto& [r, runnerMoment] : routeCells) {
                    const int moment = ArrivalMoment(defender, r);
                    const double factor = GoalSegmentBlock(c, goal, r);
                    if (moment < runnerMoment) act = std::min(act, factor);
                    if (moment == 0 || moment + delay < runnerMoment) stay = std::min(stay, factor);
                }
                runAct[d] = act;
                runStay[d] = stay;
            }
        }
        double shot[kMasks], run[kMasks];
        MaskProducts(open, shotStay, shotAct, shot);
        MaskProducts(runOpen, runStay, runAct, run);
        for (int mask = 0; mask < kMasks; ++mask) out[mask] = std::max(out[mask], std::max(shot[mask], run[mask]));
    }
}

// A pass from area to area for every set of defenders acting on its flight.
// `beforeReply`: the ball travels now, past defenders where they stand. The
// reply after the delivery is always made by every defender (tackle on the
// receiver), because the defence has seen the pass.
void CoverageLink(const GameState& s, int source, const Area& from, const Area& to,
                  const Area areas[kNumPlayers], bool beforeReply, double out[kMasks]) {
    const int defending = Opponent(TeamOf(source));
    const int firstD = FirstPlayerOf(defending);
    std::fill(out, out + kMasks, 0.0);
    // The receiver's exposure to tackles depends only on the receiving cell.
    double receives[10];
    for (int j = 0; j < to.count; ++j) {
        const Cell b = to.cells[j];
        double receive = 1;
        for (int d = 0; d < kPlayersPerTeam; ++d) {
            const int k = firstD + d;
            double tackle = 0;
            for (int m = 0; m < areas[k].count; ++m) {
                const Cell x = areas[k].cells[m];
                if (ChebyshevDistance(x, b) == 1) tackle = std::max(tackle, TackleChance(b, x, defending));
            }
            receive *= 1 - tackle;
        }
        receives[j] = receive;
    }
    for (int i = 0; i < from.count; ++i)
        for (int j = 0; j < to.count; ++j) {
            const Cell a = from.cells[i], b = to.cells[j];
            if (a == b || ChebyshevDistance(a, b) > kMaxPassDistance) continue;
            // The table slice of this pass: the pass is legal (|b - a| <= 5)
            // and both cells are in the field, so only the row offset of the
            // defender can leave the table.
            constexpr int P = CrossTable::kPass, S = CrossTable::kSpan;
            const double* slice = &kPassCrossingTable.v[b.x - a.x + P][b.y - a.y + P][0][0];
            const auto crossing = [&](Cell x) {
                const int ey = x.y - a.y;
                if (ey < -S || ey > S || x.x - a.x < -S || x.x - a.x > S) return PassCrossing(a, b, x);
                return slice[(x.x - a.x + S) * (2 * S + 1) + ey + S];
            };
            double stay[kPlayersPerTeam], act[kPlayersPerTeam];
            double bound = receives[j];
            for (int d = 0; d < kPlayersPerTeam; ++d) {
                stay[d] = crossing(s.players[firstD + d].pos);
                bound *= stay[d];
            }
            // Every product is at most the one with no defender acting (act <=
            // stay), and out[kMasks - 1] is the smallest entry: such a pair
            // can not raise any entry.
            if (bound <= out[kMasks - 1]) continue;
            for (int d = 0; d < kPlayersPerTeam; ++d) {
                const int k = firstD + d;
                act[d] = stay[d];
                if (!beforeReply)
                    for (int m = 0; m < areas[k].count; ++m)
                        act[d] = std::min(act[d], crossing(areas[k].cells[m]));
            }
            double v[kMasks];
            MaskProducts(receives[j], stay, act, v);
            for (int mask = 0; mask < kMasks; ++mask) out[mask] = std::max(out[mask], v[mask]);
        }
}

double CoverageEstimate(const GameState& s, GoalEpisode episode, int attack, bool timedRoute) {
    const int defending = Opponent(attack);
    const int firstA = FirstPlayerOf(attack), firstD = FirstPlayerOf(defending);
    const int h = s.ball.holder - firstA;
    constexpr int kAll = kMasks - 1;
    Area areas[kNumPlayers];
    for (int k = 0; k < kNumPlayers; ++k) areas[k] = PlayerArea(s, k);
    const bool attackMoves = s.teamToMove == attack;
    const bool passNow = attackMoves && !s.ballPlayed;

    double threat[4][kMasks];
    for (int i = 0; i < 4; ++i) CoverageThreat(s, firstA + i, areas, threat[i], timedRoute);
    // link[i][j]: flight covered by the mask; for the holder's pass played now
    // the flight passes defenders where they stand.
    double link[4][4][kMasks] = {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (i != j) CoverageLink(s, firstA + i, areas[firstA + i], areas[firstA + j],
                                     areas, passNow && i == h, link[i][j]);

    // Tackles on the holder: where he stands when the defence moves first; on
    // his least exposed cell of the area when the attack moves first.
    Area escape;
    if (attackMoves) escape = areas[s.ball.holder];
    else escape.cells[escape.count++] = s.ball.pos;
    double won[10][kPlayersPerTeam];
    bool anyTackle = false;
    for (int i = 0; i < escape.count; ++i)
        for (int d = 0; d < kPlayersPerTeam; ++d) {
            won[i][d] = TackleRiskAt(s, firstD + d, escape.cells[i]);
            anyTackle = anyTackle || won[i][d] > 0;
        }
    double counterAttack[kPlayersPerTeam] = {};
    if (anyTackle && episode.status == GoalEpisodeStatus::OurAttack) {
        const AttackGraph reply = BuildGraph(s, defending);
        for (int d = 0; d < kPlayersPerTeam; ++d) counterAttack[d] = AttackValue(reply, d, false);
    }
    // keep and counter-attack loss for every set of tacklers (the holder picks
    // his cell knowing who may tackle).
    double keep[kMasks], loss[kMasks];
    for (int mask = 0; mask < kMasks; ++mask) {
        double best = -2;
        keep[mask] = 1;
        loss[mask] = 0;
        for (int i = 0; i < escape.count; ++i) {
            double k = 1, noCounter = 1;
            for (int d = 0; d < kPlayersPerTeam; ++d) {
                if (!(mask >> d & 1) || won[i][d] == 0) continue;
                k *= 1 - won[i][d] - kKnockAwayChance;
                noCounter *= 1 - won[i][d] * counterAttack[d];
            }
            k = std::max(0.0, k);
            const double score = k * threat[h][kAll] - (1 - noCounter);
            if (score > best) { best = score; keep[mask] = k; loss[mask] = 1 - noCounter; }
        }
    }

    // A pass channel: the first flight covered by `mask`; the receiver, and a
    // second pass after a reply, meet every defender.
    const auto passChannel = [&](int j, int mask) {
        double delivery = link[h][j][mask];
        for (int k = 0; k < 4; ++k)
            if (k != h && k != j)
                delivery = std::max(delivery, link[h][k][mask] * link[k][j][kAll]);
        return delivery * threat[j][kAll];
    };

    if (attackMoves) {
        // The ball action comes first; every defender answers it.
        double value = keep[kAll] * threat[h][kAll] - loss[kAll];
        for (int j = 0; j < 4; ++j)
            if (j != h) value = std::max(value, passChannel(j, kAll));
        return value;
    }

    // The defence commits first. Tasks: 0 = tackle the holder, 1 = cover his
    // shot or run, 2.. = cover one passing lane.
    int partners[3], np = 0;
    for (int j = 0; j < 4; ++j) if (j != h) partners[np++] = j;
    double pass[3][kMasks];
    for (int c = 0; c < np; ++c)
        for (int mask = 0; mask < kMasks; ++mask) pass[c][mask] = passChannel(partners[c], mask);
    const int tasks = 2 + np;
    int assignment[kPlayersPerTeam] = {};
    double best = 2;
    for (;;) {
        int masks[5] = {};
        for (int d = 0; d < kPlayersPerTeam; ++d) masks[assignment[d]] |= 1 << d;
        double open = threat[h][masks[1]];
        for (int c = 0; c < np; ++c) open = std::max(open, pass[c][masks[c + 2]]);
        best = std::min(best, keep[masks[0]] * open - loss[masks[0]]);
        int d = 0;
        while (d < kPlayersPerTeam && ++assignment[d] == tasks) assignment[d++] = 0;
        if (d == kPlayersPerTeam) break;
    }
    return best;
}

bool FreeBallWinner(const GameState& s, int* winner, Cell* cell, int* moment, bool stopAtOccupiedCell);

double FreeBallValue(const GameState& s, GoalEpisode episode) {
    Cell target = s.ball.pos;
    if (s.ball.vel != Cell{0, 0}) {
        const auto path = PredictBallPath(s);
        if (!path.empty()) target = path.back();
    }
    const int attack = episode.status == GoalEpisodeStatus::OurAttack
                           ? episode.team : Opponent(episode.team);
    int nearAttack = kGridHeight, nearOther = kGridHeight;
    int runner = kNoPlayer;
    for (int k = 0; k < kNumPlayers; ++k) {
        const int d = ChebyshevDistance(s.players[k].pos, target);
        if (TeamOf(k) == attack && d < nearAttack) { nearAttack = d; runner = k; }
        if (TeamOf(k) != attack) nearOther = std::min(nearOther, d);
    }
    if (runner == kNoPlayer) return 0;
    const double control = double(nearOther + 1) / (nearAttack + nearOther + 2);
    Area areas[kNumPlayers];
    for (int k = 0; k < kNumPlayers; ++k) areas[k] = PlayerArea(s, k);
    const double sign = attack == episode.team ? 1.0 : -1.0;
    double owned = 2 * control - 1;
    if (tPossession > 0) {
        // With the possession value: the team of the first player on the ball's
        // path by time of arrival (FreeBallWinner) gets it; else the share `control`.
        int winner, moment;
        Cell cell;
        if (FreeBallWinner(s, &winner, &cell, &moment, false)) owned = TeamOf(winner) == attack ? 1 : -1;
    }
    const double chance = control * PlayerThreat(s, runner, areas);
    return sign * (chance + (1 - std::max(0.0, chance)) * tPossession * owned);
}

// Free ball by time of arrival (bot_design.md 4.3, 29.10.2026). The ball
// stays on each cell of its rolling path until its next step; a step happens
// at every second End Turn. A player takes it on the first cell he can reach
// while the ball is still there (moments as in ArrivalMoment: the team to
// move acts first in each round). The earliest player wins; the position is
// then valued as his possession at that cell with his team to move. A path
// ending out of play or in the mouth leaves only the cells before the end.
// The rolling ball's unclipped end: each step adds the velocity, which then
// loses one per axis, so an axis with speed n travels n(n+1)/2 cells.
bool BallLeavesPlay(const GameState& s) {
    const Cell v = s.ball.vel;
    const Cell end{s.ball.pos.x + Sign(v.x) * Abs(v.x) * (Abs(v.x) + 1) / 2,
                   s.ball.pos.y + Sign(v.y) * Abs(v.y) * (Abs(v.y) + 1) / 2};
    return !InField(end);
}

bool FreeBallWinner(const GameState& s, int* winner, Cell* cell, int* moment,
                    bool stopAtOccupiedCell = false) {
    std::vector<Cell> path{s.ball.pos};
    bool lostAtEnd = false;
    if (s.ball.vel != Cell{0, 0}) {
        const std::vector<Cell> rest = PredictBallPath(s);
        path.insert(path.end(), rest.begin(), rest.end());
        // The ball leaves the field (the last path cell is the clipped one) or
        // reaches the keeper in the mouth.
        lostAtEnd = BallLeavesPlay(s) || InGoalMouth(path.back().x, path.back().y);
    }
    std::size_t occupiedStep = path.size();
    if (stopAtOccupiedCell)
        for (std::size_t j = 1; j < path.size(); ++j)
            if (PlayerAt(s, path[j]) != kNoPlayer) {
                occupiedStep = j;
                lostAtEnd = false;
                break;
            }
    // Step j (1-based) happens right after moment firstStep + 2(j-1).
    const int firstStep = s.ball.stepPhase == 0 ? 2 : 3;
    int bestMoment = 1 << 20;
    *winner = kNoPlayer;
    for (int k = 0; k < kNumPlayers; ++k) {
        const Mover m = MoverOf(s, k);
        for (std::size_t j = 0; j < path.size() && j <= occupiedStep; ++j) {
            const bool last = j + 1 == path.size();
            if (last && lostAtEnd) break;
            const Cell c = path[j];
            if (!InField(c)) break;
            if (j == occupiedStep && k != PlayerAt(s, c)) break;
            // In half-moments: a move at moment m is 2m; the ball step after
            // moment m is 2m + 1, so a player already waiting on the cell takes
            // the ball with the step, before anyone moves again.
            const int step = j == 0 ? -1 : firstStep + 2 * static_cast<int>(j - 1);
            const int until = j == occupiedStep ? step :
                              (last ? (1 << 20) : firstStep + 2 * static_cast<int>(j));
            const int arrival = ArrivalMoment(m, c);
            if (arrival > until) continue;
            const int take = j == 0 ? 2 * arrival
                                      : (arrival <= step ? 2 * step + 1 : 2 * arrival);
            if (take < bestMoment) { bestMoment = take; *winner = k; *cell = c; }
            break;  // his earliest reachable cell on the path
        }
    }
    *moment = bestMoment;
    return *winner != kNoPlayer;
}

// The position after the take: the winner holds the ball at his cell.
GameState PossessionAt(const GameState& s, int winner, Cell cell, int moment) {
    GameState t = s;
    for (int k = 0; k < kNumPlayers; ++k) {
        PlayerState& p = t.players[k];
        if (p.moved) { p.prevTurnStart = p.turnStart; p.turnStart = p.pos; }
        p.moved = false;
        p.attacked = false;
    }
    PlayerState& w = t.players[winner];
    w.pos = w.turnStart = w.prevTurnStart = cell;
    w.moved = moment > 0 && moment % 2 == 0;  // a take by a move is his move; he may still pass or shoot
    // Half-moments: a take by a move (even) leaves the winner's team in its
    // turn; a take by a ball step (odd) is followed by the next mover.
    if (moment % 2 == 0) {
        t.teamToMove = TeamOf(winner);
    } else {
        const int nextOrder = ((moment - 1) / 2 + 1) % 2;
        t.teamToMove = nextOrder == 0 ? s.teamToMove : Opponent(s.teamToMove);
    }
    t.ballPlayed = false;
    t.ball.holder = winner;
    t.ball.pos = cell;
    t.ball.vel = {0, 0};
    t.ball.stepPhase = 0;
    return t;
}

// The team to move may still change the next ball segment. Certify a pickup
// only when every one of its possible player destinations stays away from the
// segment and the current ball cell. The opponent cannot move before EndTurn.
bool ForcedNextPickup(const GameState& s, GameState* pickup, EventLog* events) {
    const bool realized = tRealizedOnly;  // the certificate needs every possible move
    tRealizedOnly = false;
    struct Restore { bool value; ~Restore() { tRealizedOnly = value; } } restore{realized};
    if (s.ball.stepPhase != 0 || s.ball.vel == Cell{0, 0}) return false;
    for (const Action& action : LegalActions(s))
        if (action.type != ActionType::Move && action.type != ActionType::EndTurn)
            return false;

    const std::vector<Outcome> outcomes = OutcomesUnchecked(s, Action::EndTurn());
    if (outcomes.size() != 1 || outcomes.front().kind != OutcomeKind::BallThrough)
        return false;
    GameState next = s;
    ApplyUnchecked(next, Action::EndTurn(), outcomes.front(), events);
    if (next.phase != Phase::Play || next.ball.IsFree()) return false;

    const Cell from = s.ball.pos, to = next.ball.pos;
    const int left = std::min(from.x, to.x), right = std::max(from.x, to.x);
    const int low = std::min(from.y, to.y), high = std::max(from.y, to.y);
    const int first = FirstPlayerOf(s.teamToMove);
    for (int k = first; k < first + kPlayersPerTeam; ++k) {
        const Area area = PlayerArea(s, k);
        for (int i = 0; i < area.count; ++i) {
            const Cell c = area.cells[i];
            if (ChebyshevDistance(c, from) <= 1 ||
                (c.x >= left && c.x <= right && c.y >= low && c.y <= high))
                return false;
        }
    }
    *pickup = next;
    return true;
}

}  // namespace

void SetGeometricPossessionValue(double w) { tPossession = w; }

double GeometricStaticEstimate(const GameState& s, GoalEpisode episode,
                               bool legalTackleZones, double threatenedRetention,
                               bool forcedPickupPreview, bool exclusiveAttackChannels,
                               bool turnOrder, bool coverage, bool realizedOnly,
                               bool timedRoute, bool noShotFloor, bool timedFreeBall,
                               bool terminalFreeBall) {
    const bool previous = tRealizedOnly;
    tRealizedOnly = realizedOnly;
    struct Restore { bool value; ~Restore() { tRealizedOnly = value; } } restore{previous};
    if (episode.status == GoalEpisodeStatus::OurGoal ||
        episode.status == GoalEpisodeStatus::TheirGoal ||
        episode.status == GoalEpisodeStatus::NoGoal)
        return InitialGoalBounds(episode.status).lower;
    if (s.phase != Phase::Play) return 0;
    if (s.ball.IsFree()) {
        if (forcedPickupPreview) {
            GameState pickup;
            EventLog events;
            if (ForcedNextPickup(s, &pickup, &events))
                return GeometricStaticEstimate(pickup, AdvanceGoalEpisode(episode, s, pickup, events),
                                               legalTackleZones, threatenedRetention, false,
                                               exclusiveAttackChannels, turnOrder, coverage,
                                               realizedOnly, timedRoute, noShotFloor, timedFreeBall,
                                               terminalFreeBall);
        }
        bool terminalPath = false;
        // Partial turns are only being ranked for the beam. Keep that ranking
        // cheap; use the terminal-path model on completed states and leaves.
        if (terminalFreeBall && coverage && !realizedOnly && s.ball.vel != Cell{0, 0}) {
            const std::vector<Cell> path = PredictBallPath(s);
            terminalPath = BallLeavesPlay(s) ||
                           (!path.empty() && InGoalMouth(path.back().x, path.back().y) &&
                            PlayerAt(s, path.back()) == kNoPlayer);
            for (Cell c : path)
                if (PlayerAt(s, c) != kNoPlayer) {
                    terminalPath = true;
                    break;
                }
        }
        if (terminalPath) {
            int winner, moment; Cell cell;
            if (!FreeBallWinner(s, &winner, &cell, &moment, true)) return 0;
            const int attackNow = episode.status == GoalEpisodeStatus::OurAttack
                                      ? episode.team : Opponent(episode.team);
            if (TeamOf(winner) != attackNow && episode.status != GoalEpisodeStatus::OurAttack)
                return TeamOf(winner) == episode.team ? tPossession : -tPossession;  // the counterattack ends
            const GameState pickup = PossessionAt(s, winner, cell, moment);
            Area areas[kNumPlayers];
            for (int k = 0; k < kNumPlayers; ++k) areas[k] = PlayerArea(pickup, k);
            const double chance = PlayerThreat(pickup, winner, areas);
            const double threat = chance + (1 - chance) * tPossession;
            return TeamOf(winner) == episode.team ? threat : -threat;
        }
        if (timedFreeBall && coverage) {
            int winner, moment; Cell cell;
            if (!FreeBallWinner(s, &winner, &cell, &moment)) return 0;
            const int attackNow = episode.status == GoalEpisodeStatus::OurAttack
                                      ? episode.team : Opponent(episode.team);
            GoalEpisode next = episode;
            if (TeamOf(winner) != attackNow) {
                if (episode.status != GoalEpisodeStatus::OurAttack)   // the counterattack ends
                    return TeamOf(winner) == episode.team ? tPossession : -tPossession;
                next.status = GoalEpisodeStatus::TheirCounterattack;
            }
            return GeometricStaticEstimate(PossessionAt(s, winner, cell, moment), next, legalTackleZones,
                                           threatenedRetention, false, exclusiveAttackChannels,
                                           turnOrder, coverage, realizedOnly, timedRoute,
                                           noShotFloor, false, false);
        }
        // A ball that leaves play before anyone reaches it is a restart.
        if (coverage && s.ball.vel != Cell{0, 0} && BallLeavesPlay(s)) {
            int winner, moment; Cell cell;
            if (!FreeBallWinner(s, &winner, &cell, &moment)) return 0;
        }
        return FreeBallValue(s, episode);
    }

    const int attack = episode.status == GoalEpisodeStatus::OurAttack
                           ? episode.team : Opponent(episode.team);
    if (TeamOf(s.ball.holder) != attack)
        return TeamOf(s.ball.holder) == episode.team ? tPossession : -tPossession;
    const double sign = attack == episode.team ? 1.0 : -1.0;
    if (turnOrder || coverage) {
        const double value = coverage ? CoverageEstimate(s, episode, attack, timedRoute)
                                      : TurnOrderEstimate(s, episode, attack, exclusiveAttackChannels);
        const double shotNow = s.teamToMove == attack ? HolderShotChanceThisTurn(s) : 0;
        // A shot is the attack's own option only when it moves now; with the
        // defence to move a losing position keeps its negative value.
        const bool canShoot = s.teamToMove == attack && !s.ballPlayed;
        if (tPossession > 0) {
            // The possession: kept until the holder acts with the chance k.
            double keep = 1;
            if (!canShoot) {
                const int firstD = FirstPlayerOf(Opponent(attack));
                for (int d = firstD; d < firstD + kPlayersPerTeam; ++d) {
                    const double won = TackleRiskAt(s, d, s.ball.pos);
                    if (won > 0) keep *= std::max(0.0, 1 - won - kKnockAwayChance);
                }
            }
            // The chance found by the static, and when it does not come off the
            // possession goes on: t + (1 - t) W (2k - 1), never above 1.
            const double held = value + (1 - std::max(0.0, value)) * tPossession * (2 * keep - 1);
            const double choice = canShoot ? std::max(shotNow * (1 + tPossession) - tPossession, held) : held;
            return sign * std::max(-1.0, std::min(1.0, choice));
        }
        const double choice = noShotFloor && !canShoot ? value : std::max(shotNow, value);
        return sign * std::max(-1.0, std::min(1.0, choice));
    }
    const AttackGraph own = BuildGraph(s, attack);
    const double opportunity = AttackValue(own, s.ball.holder - FirstPlayerOf(attack),
                                           exclusiveAttackChannels);
    double keep = 1, noCounter = 1;
    const int defending = Opponent(attack);
    AttackGraph reply;
    bool haveReply = false;
    bool threatened = false;
    for (int k = FirstPlayerOf(defending);
         k < FirstPlayerOf(defending) + kPlayersPerTeam; ++k) {
        const double won = legalTackleZones ? LegalTackleRisk(s, k) : TackleRisk(s, k);
        if (won == 0) continue;
        threatened = true;
        keep *= 1 - won - kKnockAwayChance;
        if (episode.status == GoalEpisodeStatus::OurAttack) {
            if (!haveReply) { reply = BuildGraph(s, defending); haveReply = true; }
            noCounter *= 1 - won * AttackValue(reply, k - FirstPlayerOf(defending),
                                             exclusiveAttackChannels);
        }
    }
    // Before a ball action the owner may still pass or shoot. Penalize
    // possession exposed to the defender's reply, not that earlier decision.
    if (threatened && (s.teamToMove != attack || s.ballPlayed))
        keep *= threatenedRetention;
    const double value = keep * opportunity -
                         (episode.status == GoalEpisodeStatus::OurAttack ? 1 - noCounter : 0);
    const double shotNow = s.teamToMove == attack ? HolderShotChanceThisTurn(s) : 0;
    const double choice = std::max(shotNow, value);
    return sign * std::max(-1.0, std::min(1.0, choice));
}

}  // namespace bot
}  // namespace soccer
