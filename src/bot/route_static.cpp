#include "bot/route_static.h"

#include "bot/ball_race.h"
#include "bot/bot.h"
#include "bot/reach_front.h"
#include "engine/rules.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <vector>

namespace soccer::bot {
namespace {

constexpr int kNever = 1 << 20;
constexpr int kMasks = 1 << kPlayersPerTeam;

Cell CellOf(int index) { return {index % kFieldWidth, kFirstRow + index / kFieldWidth}; }

// The blocking zone of a shooting cell, per attacking team: cells and the
// factor a defender there puts on the shot (ShotBlockFactor).
struct ZoneCell { int cell; float factor; };
const std::vector<ZoneCell>& ShotZone(int team, int cell) {
    static const auto zones = [] {
        std::array<std::vector<std::vector<ZoneCell>>, kNumTeams> z;
        for (int t = 0; t < kNumTeams; ++t) {
            z[t].resize(kReachFrontCells);
            for (int i = 0; i < kReachFrontCells; ++i)
                for (int j = 0; j < kReachFrontCells; ++j) {
                    if (j == i) continue;
                    const double f = ShotBlockFactor(CellOf(i), t, CellOf(j));
                    if (f < 1) z[t][i].push_back({j, static_cast<float>(f)});
                }
        }
        return z;
    }();
    return zones[team][cell];
}

// Cells of a pass's bounding box (endpoints excluded) and the factor a
// defender there puts on the pass going through: 1 - InterceptionWeight of
// his distance to the line (ball.cpp).
std::vector<ZoneCell> PassLine(Cell a, Cell b) {
    std::vector<ZoneCell> line;
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len = std::sqrt(dx * dx + dy * dy);
    for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
        for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y) {
            const Cell n{x, y};
            if (n == a || n == b) continue;
            const double dist = std::fabs(dx * (y - a.y) - dy * (x - a.x)) / len;
            line.push_back({ReachFrontIndex(n), static_cast<float>(1 - 0.6 * std::exp(-2.0 * dist * dist))});
        }
    return line;
}

struct Route {
    float v;
    int16_t shotCell;
    int8_t shotFrame;
    int8_t passFrame;   // the pass line's deadline (-1: no pass)
    int16_t line;       // index into the pass lines (-1: no pass)
    int16_t waitCell;   // a pass ahead waits here for its receiver (-1: none)
    int8_t take;        // ... until this frame
    int8_t shooter;
};

}  // namespace

// The value for the team of `holder` of his ball: held now (startFrame < 0),
// or taken on `startCell` in frame `startFrame` of the map (a free ball).
static RouteStatic Evaluate(const GameState& s, const ReachFront& front, int holder, int startCell, int startFrame,
                     bool loss) {
    RouteStatic r;
    const int team = TeamOf(holder);
    const int defending = Opponent(team);
    const int firstAttacker = FirstPlayerOf(team), firstDefender = FirstPlayerOf(defending);
    const int horizon = static_cast<int>(front.frames.size()) - 1;
    const int firstOwn = team == s.teamToMove ? 1 : 2;
    const int defenceFirst = 3 - firstOwn;

    std::array<std::array<int, kReachFrontCells>, kNumPlayers> arrival;
    for (auto& a : arrival) a.fill(kNever);
    for (int f = 0; f <= horizon; ++f)
        for (int i = 0; i < kReachFrontCells; ++i)
            for (int k = 0; k < kNumPlayers; ++k)
                if ((front.frames[f].players[i] >> k & 1u) && arrival[k][i] == kNever) arrival[k][i] = f;
    const auto own = [&](int k, int i) {
        const int a = arrival[k][i];
        return a == 0 ? firstOwn : a;
    };
    // The holder's frame on a cell: from his start, or from where he took a free
    // ball (as a receiver's dribble), never before his area gets there.
    const int base = startFrame < 0 ? firstOwn : startFrame;
    const auto holderAt = [&](int p) {
        if (startFrame < 0) return arrival[holder][p] == kNever ? kNever : own(holder, p);
        if (p == startCell) return startFrame;
        if (arrival[holder][p] == kNever) return kNever;
        const int turns = RaceTurns(CellOf(startCell), {0, 0}, CellOf(p));
        return turns >= 1000 ? kNever : std::max(arrival[holder][p], startFrame + 2 * turns);
    };

    // Routes of the attack on its own map areas: the holder's dribble to a
    // shot, or his dribble (this or his next own turn) and one pass to a
    // partner (to his feet or ahead), the partner's dribble and shot.
    std::vector<Route> routes;
    std::vector<std::vector<ZoneCell>> lines;
    for (int p = 0; p < kReachFrontCells; ++p) {
        const int passFrame = holderAt(p);
        if (passFrame > horizon) continue;
        const Cell pc = CellOf(p);
        routes.push_back({static_cast<float>(OpenShotProbability(pc, team)), static_cast<int16_t>(p),
                          static_cast<int8_t>(passFrame), -1, -1, -1, 0, static_cast<int8_t>(holder)});
        if (passFrame > base + 2) continue;
        for (int x = pc.x - kMaxPassDistance; x <= pc.x + kMaxPassDistance; ++x)
            for (int y = pc.y - kMaxPassDistance; y <= pc.y + kMaxPassDistance; ++y) {
                const Cell cc{x, y};
                if (!InField(cc) || cc == pc) continue;
                const int c = ReachFrontIndex(cc);
                int lineId = -1;
                for (int k = firstAttacker; k < firstAttacker + kPlayersPerTeam; ++k) {
                    if (k == holder || arrival[k][c] == kNever) continue;
                    const int take = std::max(passFrame, own(k, c));
                    if (take > horizon) continue;
                    if (lineId < 0) {
                        lineId = static_cast<int>(lines.size());
                        lines.push_back(PassLine(pc, cc));
                    }
                    const int16_t wait = take > passFrame ? static_cast<int16_t>(c) : int16_t(-1);
                    for (int j = 0; j < kReachFrontCells; ++j) {
                        if (arrival[k][j] == kNever) continue;
                        const int turns = RaceTurns(cc, {0, 0}, CellOf(j));
                        if (turns >= 1000) continue;
                        const int shotFrame = std::max(own(k, j), take + 2 * turns);
                        if (shotFrame > horizon) continue;
                        routes.push_back({static_cast<float>(OpenShotProbability(CellOf(j), team)),
                                          static_cast<int16_t>(j), static_cast<int8_t>(shotFrame),
                                          static_cast<int8_t>(passFrame), static_cast<int16_t>(lineId), wait,
                                          static_cast<int8_t>(take), static_cast<int8_t>(k)});
                    }
                }
            }
    }
    std::stable_sort(routes.begin(), routes.end(), [](const Route& a, const Route& b) { return a.v > b.v; });
    r.routes = static_cast<long>(routes.size());
    const std::size_t count = routes.size();

    // The defence commits each defender to the cell of his next move (the
    // square around his inertia centre); from there he can be on a cell n by
    // frame defenceFirst + 2 * turns (0 on his own cell). On a route he puts
    // the factor of the best thing he can do in time: take a pass ahead before
    // its receiver (0), stand by the pass line before the pass, or in the
    // shot's blocking zone before the shot. The route's value is its open shot
    // times the factors of all four.
    std::array<std::vector<std::vector<float>>, kPlayersPerTeam> choices;
    const Cell holderCell = s.players[holder].pos;
    // Per defender and commitment: the chance the ball survives him on a route,
    // and the other side's goal chance where he would win it.
    std::array<std::vector<std::vector<float>>, kPlayersPerTeam> losses, keepers;
    // The other side's goal chance with the ball at `cell` from `frame` on: its
    // best dribble to a shot, our players blocking as the map lets them.
    std::vector<double> counterCache(static_cast<std::size_t>(kReachFrontCells) * (horizon + 2), -1);
    const auto counter = [&](int cell, int frame) {
        double& c = counterCache[static_cast<std::size_t>(cell) * (horizon + 2) + std::min(frame, horizon + 1)];
        if (c >= 0) return c;
        int start = frame + 1;
        if ((start & 1) != (defenceFirst & 1)) ++start;
        c = 0;
        for (int j = 0; j < kReachFrontCells; ++j) {
            const int turns = j == cell ? 0 : RaceTurns(CellOf(cell), {0, 0}, CellOf(j));
            if (turns >= 1000) continue;
            const int shot = start + 2 * turns;
            if (shot > horizon) continue;
            double v = OpenShotProbability(CellOf(j), defending);
            if (v <= c) continue;
            for (int a = firstAttacker; a < firstAttacker + kPlayersPerTeam && v > c; ++a) {
                float block = 1;
                for (const ZoneCell& z : ShotZone(defending, j))
                    if (arrival[a][z.cell] < shot) block = std::min(block, z.factor);
                // Our player who can stand on the cell before him takes it away.
                if (arrival[a][j] < shot) block = 0;
                v *= block;
            }
            c = std::max(c, v);
        }
        return c;
    };
    for (int d = 0; d < kPlayersPerTeam; ++d) {
        const int k = firstDefender + d;
        const PlayerState& pl = s.players[k];
        // His next move's cells by the rules (with the others where they stand).
        GameState t = s;
        t.teamToMove = defending;
        t.players[k].moved = false;
        std::vector<Cell> next = MoveDestinations(t, k);
        if (next.empty()) next.push_back(pl.pos);
        for (const Cell q : next) {
                const Cell sign{q.x - pl.pos.x, q.y - pl.pos.y};
                // Not before the map lets him (contact with the attack's areas),
                // nor before he gets there from his committed cell.
                std::array<int, kReachFrontCells> reach;
                for (int n = 0; n < kReachFrontCells; ++n) {
                    const int turns = RaceTurns(q, sign, CellOf(n));
                    const int fromQ = turns >= 1000 ? kNever : defenceFirst + 2 * turns;
                    reach[n] = std::max(fromQ, arrival[k][n]);
                }
                reach[ReachFrontIndex(pl.pos)] = 0;
                reach[ReachFrontIndex(q)] = std::min(reach[ReachFrontIndex(q)], defenceFirst);
                // A defence moving first can attack the holder where he stands.
                float tackle = 1;
                if (startFrame < 0 && defenceFirst == 1 && ChebyshevDistance(q, holderCell) <= 1)
                    tackle = static_cast<float>(std::max(0.0, 1 - TackleChance(holderCell, q, defending) - kKnockAwayChance));
                std::vector<float> f(count, 1), keeps(count, 1), lossValue(count, 0);
                for (std::size_t k = 0; k < count; ++k) {
                    const Route& rt = routes[k];
                    // Losing the ball: a tackle on the holder, a pass caught on its
                    // line, a pass ahead taken before its receiver. The loss is
                    // valued by the same measure for the other side: his goal chance
                    // from where he gets the ball.
                    float keep = tackle, worst = 0;
                    int lossCell = tackle < 1 ? ReachFrontIndex(holderCell) : -1, lossFrame = defenceFirst;
                    if (tackle < 1) worst = 1 - tackle;
                    if (rt.line >= 0) {
                        float line = 1;
                        int at = -1;
                        for (const ZoneCell& z : lines[rt.line])
                            if (reach[z.cell] < rt.passFrame && z.factor < line) { line = z.factor; at = z.cell; }
                        if (at >= 0 && 1 - line > worst) { worst = 1 - line; lossCell = at; lossFrame = rt.passFrame; }
                        keep *= line;
                    }
                    if (rt.waitCell >= 0 && reach[rt.waitCell] < rt.take) {
                        keep = 0;
                        lossCell = rt.waitCell;
                        lossFrame = rt.take;
                    }
                    float block = 1;
                    if (keep > 0)
                        for (const ZoneCell& z : ShotZone(team, rt.shotCell))
                            if (reach[z.cell] < rt.shotFrame) block = std::min(block, z.factor);
                    f[k] = keep * block;
                    keeps[k] = keep;
                    if (f[k] < 1) r.defenders |= 1u << d;   // he can do something: not cut off
                    if (loss && keep < 1 && lossCell >= 0) lossValue[k] = static_cast<float>(counter(lossCell, lossFrame));
                }
                choices[d].push_back(std::move(f));
                losses[d].push_back(std::move(lossValue));
                keepers[d].push_back(std::move(keeps));
        }
    }
    // The 1 v 0 test: the best route with every defender at his best for it.
    std::vector<float> each(count, 1);
    for (int d = 0; d < kPlayersPerTeam; ++d)
        for (std::size_t k = 0; k < count; ++k) {
            float m = 1;
            for (const auto& f : choices[d]) m = std::min(m, f[k]);
            each[k] *= m;
        }
    double open = 0;
    for (std::size_t k = 0; k < count; ++k) open = std::max(open, double(routes[k].v * each[k]));
    // The defence minimises over its commitments the attack's best route.
    double value = 2;
    long arg = -1;
    const std::size_t n0 = choices[0].size(), n1 = choices[1].size(), n2 = choices[2].size(), n3 = choices[3].size();
    for (std::size_t i0 = 0; i0 < n0; ++i0)
        for (std::size_t i1 = 0; i1 < n1; ++i1)
            for (std::size_t i2 = 0; i2 < n2; ++i2)
                for (std::size_t i3 = 0; i3 < n3; ++i3) {
                    const float *a = choices[0][i0].data(), *b = choices[1][i1].data(), *c = choices[2][i2].data(),
                                *e = choices[3][i3].data();
                    const float *la = losses[0][i0].data(), *lb = losses[1][i1].data(), *lc = losses[2][i2].data(),
                                *le = losses[3][i3].data();
                    const float *ka = keepers[0][i0].data(), *kb = keepers[1][i1].data(), *kc = keepers[2][i2].data(),
                                *ke = keepers[3][i3].data();
                    double best = -2;
                    long at = -1;
                    for (std::size_t k = 0; k < count && routes[k].v > best; ++k) {
                        // The ball is lost with 1 - the product of the keeps; the loss
                        // costs the other side's best chance among the defenders' cells.
                        const double lost = 1 - double(ka[k]) * kb[k] * kc[k] * ke[k];
                        const double x = routes[k].v * a[k] * b[k] * c[k] * e[k] -
                                         lost * std::max(std::max(la[k], lb[k]), std::max(lc[k], le[k]));
                        if (x > best) {
                            best = x;
                            at = static_cast<long>(k);
                            if (best >= value) break;   // this commitment is no better
                        }
                    }
                    if (best < value) { value = best; arg = at; }
                }
    r.valid = true;
    r.team = team;
    r.value = value > 1 ? 0 : std::max(-1.0, value);
    r.open = open;
    if (arg >= 0) {
        const Route& rt = routes[arg];
        r.shooter = rt.shooter;
        r.cell = CellOf(rt.shotCell);
        r.frame = rt.shotFrame;
    }
    if (!routes.empty()) r.best = routes.front().v;
    return r;
}

RouteStatic ComputeRouteStatic(const GameState& s, bool loss) {
    RouteStatic r;
    if (s.phase != Phase::Play || s.keeperRestartTeam >= 0) return r;
    const ReachFront front = ComputeReachFront(s);
    if (front.stop == ReachFrontStop::InvalidStart) return r;
    if (!s.ball.IsFree()) {
        r = Evaluate(s, front, s.ball.holder, ReachFrontIndex(s.players[s.ball.holder].pos), -1, loss);
        r.taker = s.ball.holder;
        return r;
    }
    // A free ball: on its untouched path, the first players the map puts on it
    // run for it and take it - by standing there at its step, or by moving onto
    // it. Nobody: the path ends with a restart.
    const int frames = static_cast<int>(front.frames.size());
    const auto nextOwn = [&](int team, int frame) {   // first frame >= frame of that team
        const int parity = team == s.teamToMove ? 1 : 0;
        return (frame & 1) == parity ? frame : frame + 1;
    };
    unsigned takers = 0;
    int cell = -1, start = -1;
    for (const BallWindow& w : BallPathWindows(s)) {
        const int c = ReachFrontIndex(w.cell);
        if (w.arrival >= 0) {
            const int f = (w.arrival + 1) / 2;
            if (f < frames && front.frames[f].players[c]) {
                takers = front.frames[f].players[c];
                cell = c;
                start = f + 1;   // he has it after the step; he acts in his next own frame
                break;
            }
        }
        const int firstSlot = w.arrival < 0 ? 0 : (w.arrival + 1) / 2;
        const int lastSlot = w.openEnd ? frames - 2 : w.lastMoveSlot;
        for (int slot = firstSlot; slot <= lastSlot && slot + 1 < frames; ++slot)
            if (front.frames[slot + 1].players[c]) {
                takers = front.frames[slot + 1].players[c];
                cell = c;
                start = slot + 1;
                break;
            }
        if (takers) break;
    }
    r.freeBall = true;
    if (!takers) {
        // A restart: the ball rolls on with everybody standing to its out or the
        // keeper, both teams take the standard formation and the restarting team
        // has the ball at the centre - that position is valued, by the same measure.
        GameState t = s;
        for (int guard = 0; guard < 64 && t.phase == Phase::Play && t.ball.IsFree(); ++guard) {
            const std::vector<Outcome> outcomes = OutcomesUnchecked(t, Action::EndTurn());
            Outcome through = outcomes.front();
            for (const Outcome& o : outcomes)
                if (o.kind == OutcomeKind::BallThrough) through = o;
            ApplyUnchecked(t, Action::EndTurn(), through);
        }
        for (int guard = 0; guard < 4 * kNumPlayers && t.phase == Phase::Lineup; ++guard) {
            const Action a = LineupAction(t);
            if (Validate(t, a) != ActionError::None) break;
            ApplyUnchecked(t, a, Outcome::Certain());
        }
        r.valid = true;
        r.restart = true;
        if (t.phase == Phase::Play && !t.ball.IsFree()) {
            RouteStatic e = ComputeRouteStatic(t, loss);
            if (e.valid) {
                e.freeBall = true;
                e.restart = true;
                return e;
            }
        }
        return r;
    }
    // The team picks the taker whose ball is worth most.
    bool have = false;
    for (int k = 0; k < kNumPlayers; ++k) {
        if (!(takers >> k & 1u)) continue;
        RouteStatic e = Evaluate(s, front, k, cell, std::min(nextOwn(TeamOf(k), start), frames - 1), loss);
        if (!e.valid) continue;
        if (!have || e.value > r.value) {
            const long routes = r.routes;
            r = e;
            r.routes += routes;
            r.taker = k;
            r.takeCell = CellOf(cell);
            r.takeFrame = start;
            r.freeBall = true;
            have = true;
        }
    }
    return r;
}

}  // namespace soccer::bot
