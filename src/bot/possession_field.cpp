#include "bot/possession_field.h"

#include "bot/ball_race.h"
#include "bot/bot.h"
#include "bot/reach_front.h"
#include "engine/notation.h"
#include "engine/rules.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace soccer::bot {
namespace {

constexpr int kNever = 1 << 20;
constexpr int kCells = kReachFrontCells;

Cell CellOf(int index) { return {index % kFieldWidth, kFirstRow + index / kFieldWidth}; }

struct ZoneCell { int16_t cell; float factor; };

// The blocking zone of a shooting cell per attacking team (ShotBlockFactor).
const std::vector<ZoneCell>& ShotZone(int team, int cell) {
    static const auto zones = [] {
        std::array<std::vector<std::vector<ZoneCell>>, kNumTeams> z;
        for (int t = 0; t < kNumTeams; ++t) {
            z[t].resize(kCells);
            for (int i = 0; i < kCells; ++i)
                for (int j = 0; j < kCells; ++j) {
                    if (j == i) continue;
                    const double f = ShotBlockFactor(CellOf(i), t, CellOf(j));
                    if (f < 1) z[t][i].push_back({static_cast<int16_t>(j), static_cast<float>(f)});
                }
        }
        return z;
    }();
    return zones[team][cell];
}

// Passes from each cell: the target and the cells of the line's bounding box
// with the chance a defender there touches the ball (InterceptionWeight).
struct PassLine { int16_t target; std::vector<ZoneCell> cells; };
const std::vector<PassLine>& PassLines(int cell) {
    static const auto lines = [] {
        std::vector<std::vector<PassLine>> all(kCells);
        for (int i = 0; i < kCells; ++i) {
            const Cell a = CellOf(i);
            for (int x = a.x - kMaxPassDistance; x <= a.x + kMaxPassDistance; ++x)
                for (int y = a.y - kMaxPassDistance; y <= a.y + kMaxPassDistance; ++y) {
                    const Cell b{x, y};
                    if (!InField(b) || b == a) continue;
                    PassLine line;
                    line.target = static_cast<int16_t>(ReachFrontIndex(b));
                    const double dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
                    for (int u = std::min(a.x, b.x); u <= std::max(a.x, b.x); ++u)
                        for (int v = std::min(a.y, b.y); v <= std::max(a.y, b.y); ++v) {
                            const Cell n{u, v};
                            if (n == a || n == b) continue;
                            const double dist = std::fabs(dx * (v - a.y) - dy * (u - a.x)) / len;
                            const double p = InterceptionWeight(dist);
                            if (p > 0.01) line.cells.push_back({static_cast<int16_t>(ReachFrontIndex(n)), static_cast<float>(p)});
                        }
                    all[i].push_back(std::move(line));
                }
        }
        return all;
    }();
    return lines[cell];
}

class Field {
public:
    Field(const GameState& s, const ReachFront& front, double w, int ballTeam, int passes, unsigned absent = 0,
          double endBonus = 0, int carrier = -1)
        : s_(s), front_(front), w_(w), ballTeam_(ballTeam), passes_(passes), absent_(absent), endBonus_(endBonus) {
        h_ = static_cast<int>(front.frames.size()) - 1;
        t_ = h_ + 4;
        stride_ = t_ + 4;
        for (auto& a : arr_) a.fill(kNever);
        for (int f = 0; f <= h_; ++f)
            for (int i = 0; i < kCells; ++i)
                for (int k = 0; k < kNumPlayers; ++k)
                    if ((front.frames[f].players[i] >> k & 1u) && arr_[k][i] == kNever) arr_[k][i] = f;
        firstOwn_[s.teamToMove] = 1;
        firstOwn_[Opponent(s.teamToMove)] = 2;
        // The defence's access around the ball carrier (07.11.2026): his own
        // claims do not hold it off - he is where the ball is, not on every
        // cell he could have reached (tree_13w: W2 holding on i19 left h18 to
        // B4). A second map without him gives the defenders' frames.
        if (carrier >= 0) {
            const ReachFront without = ComputeReachFront(s, kHalfTurnsPerMatch, 1u << carrier);
            if (without.stop != ReachFrontStop::InvalidStart) {
                // Everybody but the carrier: his partners' runs are not
                // shielded by his claims either (tree_13w: with B4 on h18, W1
                // cannot reach g19 in two moves; the map had kept B4 off h18,
                // W2's start cell).
                for (int k = 0; k < kNumPlayers; ++k)
                    if (k != carrier) arr_[k].fill(kNever);
                for (int f = 0; f < static_cast<int>(without.frames.size()) && f <= h_; ++f)
                    for (int i = 0; i < kCells; ++i)
                        for (int k = 0; k < kNumPlayers; ++k)
                            if (k != carrier && (without.frames[f].players[i] >> k & 1u) && arr_[k][i] == kNever) arr_[k][i] = f;
            }
        }
        claim_ = arr_;
        const std::size_t n = static_cast<std::size_t>(Layers()) * kNumPlayers * kCells * stride_;
        first_.assign(n, '-');
        b_.assign(n, 0);
        a_.assign(n, 0);
        a0_.assign(n, 0);
        keep_.assign(static_cast<std::size_t>(kNumTeams) * kCells * stride_, -1);
        tackler_.assign(keep_.size(), -1);
        won_.assign(keep_.size(), 0);
        shot_.assign(keep_.size(), -1);
        for (int t = t_; t >= 1; --t) Frame(t);
    }

    bool Own(int team, int t) const { return (t & 1) == (firstOwn_[team] & 1); }
    // His own frame on cell i by the map (0: where he stands -> his first frame).
    int OwnArrival(int k, int i) const {
        const int a = arr_[k][i];
        if (a == kNever) return kNever;
        if (a == 0) return firstOwn_[TeamOf(k)];
        return Own(TeamOf(k), a) ? a : a + 1;
    }
    double Bv(int k, int c, int t, int q = 0) const {
        if (t > t_) return End(k);
        return b_[Index(k, c, t, q)];
    }
    double Av(int k, int c, int t, int q = 0) const { return a_[Index(k, c, t, q)]; }
    // The ball still held at the horizon: W, plus the territory of the team
    // with the ball (a tie-break inside the possession: its players nearer the
    // goal), 07.11.2026.
    double End(int k) const { return TeamOf(k) == ballTeam_ ? w_ + endBonus_ : w_; }
    // Pass layers (07.11.2026): 0 - no pass yet; with two passes allowed, 1..4 -
    // one pass made by the team's player 0..3 (who may not get the ball back:
    // his place after passing is not tracked, tree_13w's one-two teleported
    // W2 to g19); 5 - both passes made.
    int Layers() const { return passes_ >= 2 ? 6 : passes_ + 1; }
    bool CanPass(int q) const { return passes_ >= 2 ? q <= 4 : q < passes_; }
    int NextLayer(int q, int k) const {
        if (passes_ < 2) return q + 1;
        return q == 0 ? 1 + (k - FirstPlayerOf(TeamOf(k))) : 5;
    }
    int Barred(int q, int team) const {   // the player who may not receive in layer q
        return passes_ >= 2 && q >= 1 && q <= 4 ? FirstPlayerOf(team) + q - 1 : -1;
    }
    // k has won or kept the ball on c and ends his frame f with it: the other
    // side may tackle him back in f + 1.
    double HoldEnd(int k, int c, int f) {
        if (f + 2 > t_) return End(k);
        const int team = TeamOf(k);
        return Fight(team, c, f + 1, Bv(k, c, f + 2));
    }

    // The holder at the root: his map area is exact (with his velocity).
    double Start(int k, int c, int t, bool exact, char* first) {
        double best = -2;
        for (int d = 0; d < kCells; ++d) {
            int when;
            if (exact) {
                when = d == c ? t : OwnArrival(k, d);
                if (when < t) when = t;
            } else {
                when = DribbleFrame(k, c, t, d);
            }
            if (when > t_) continue;
            const double v = Dribble(k, c, t, d, when);
            if (v > best) {
                best = v;
                if (first) *first = d != c ? 'd' : first_[Index(k, d, when)];
            }
        }
        return best;
    }
    // The holder's team not to move: the defence first may tackle him, by the
    // rules from where it stands (adjacent after a move, the attack direction).
    double Wait(int k, int c, int t) {
        const Cell ball = s_.players[k].pos;
        double keep = 1, won = 0, strongest = 0;
        int who = -1;
        for (int d = FirstPlayerOf(s_.teamToMove); d < FirstPlayerOf(s_.teamToMove) + kPlayersPerTeam; ++d) {
            const PlayerState& pl = s_.players[d];
            if (pl.attacked) continue;
            std::vector<Cell> from;
            if (pl.moved) from.push_back(pl.pos);
            else from = MoveDestinations(s_, d);
            double best = 0;
            for (const Cell q : from) {
                if (ChebyshevDistance(q, ball) != 1) continue;
                if (!AttackDirectionAllowed(q, ball, pl.Velocity(), {q.x - pl.turnStart.x, q.y - pl.turnStart.y})) continue;
                best = std::max(best, TackleChance(ball, q, s_.teamToMove));
            }
            if (best <= 0) continue;
            const double fight = std::min(1.0, best + kKnockAwayChance);
            won += keep * best;
            keep *= 1 - fight;
            if (best > strongest) { strongest = best; who = d; }
        }
        return keep * Start(k, c, t, true, nullptr) + won * (who >= 0 ? -HoldEnd(who, c, t - 1) : 0);
    }
    long transitions = 0;
    char RootFirst = '-';


    // ---- Analysis (07.11.2026): the best line of play and who acts on it.
    struct Step { char kind; int player; int from, to, frame; double keep; unsigned defenders; double value; };
    unsigned TackleMask(int team, int c, int f) const {
        unsigned m = 0;
        const int def = FirstPlayerOf(Opponent(team));
        const Cell cc = CellOf(c);
        for (int d = def; d < def + kPlayersPerTeam; ++d)
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    const Cell n{cc.x + dx, cc.y + dy};
                    if ((dx || dy) && InField(n) && Near(d, ReachFrontIndex(n), f)) m |= 1u << d;
                }
        return m;
    }
    unsigned ZoneMask(int team, int c, int t) const {
        unsigned m = 0;
        const int def = FirstPlayerOf(Opponent(team));
        for (int d = def; d < def + kPlayersPerTeam; ++d)
            for (const ZoneCell& z : ShotZone(team, c))
                if (By(d, z.cell, t - 1)) m |= 1u << d;
        return m;
    }

    // A pass ahead (07.11.2026): the ball lands on the target with its
    // velocity reduced by one per axis and rolls on - one step at the end of
    // every other turn, slowing by one (game_model.md). It rests on a cell for
    // our frame and the next of theirs; whoever can be on it first takes it
    // (a player standing there when it arrives too). Out of the field or into an
    // empty goal mouth: the other side's restart. Each step can be touched by a
    // defender by its line, as a pass.
    struct RollPick { double v = -2; int j = -1, cell = -1, frame = -1; };
    RollPick Roll(int k, int c, int t, int q, int target) {
        RollPick out;
        const int team = TeamOf(k), first = FirstPlayerOf(team), def = FirstPlayerOf(Opponent(team));
        const int nq = NextLayer(q, k), barred = Barred(q, team);
        const auto reduce = [](Cell v) { return Cell{v.x - Sign(v.x), v.y - Sign(v.y)}; };
        Cell at = CellOf(target);
        Cell vel = reduce({at.x - CellOf(c).x, at.y - CellOf(c).y});
        double carry = 1;      // the chance the ball is still untouched
        double value = 0;      // accumulated value of the touches on the way
        for (int m = 0; m <= 8; ++m) {
            if (m > 0) {
                if (vel.x == 0 && vel.y == 0) {
                    // It rests here: the next windows on the same cell.
                } else {
                    const Cell from = at;
                    at = {at.x + vel.x, at.y + vel.y};
                    const int stepFrame = t + 2 * m - 1;   // their End Turn
                    // Touched on the way, as a pass from `from` to `at`.
                    if (InField(at)) {
                        const int a = ReachFrontIndex(from);
                        for (const PassLine& line : PassLines(a)) {
                            if (line.target != ReachFrontIndex(at)) continue;
                            double through = 1, strongest = 0;
                            int who = -1, where = -1;
                            for (int d = def; d < def + kPlayersPerTeam; ++d) {
                                double p = 0;
                                int w = -1;
                                for (const ZoneCell& z : line.cells)
                                    if (z.factor > p && By(d, z.cell, stepFrame)) { p = z.factor; w = z.cell; }
                                through *= 1 - p;
                                if (p > strongest) { strongest = p; who = d; where = w; }
                            }
                            if (who >= 0) value += carry * (1 - through) * -Bv(who, where, stepFrame + 2);
                            carry *= through;
                            break;
                        }
                    }
                    vel = reduce(vel);
                    if (!InField(at)) { value += carry * -w_; out.v = value; return out; }
                }
            }
            const int cell = ReachFrontIndex(at);
            const int ours = m == 0 ? kNever : t + 2 * m;            // our frame with the ball here
            const int theirs = m == 0 ? t + 1 : t + 2 * m + 1;       // theirs
            if (ours > t_ && theirs > t_) break;
            if (InGoalMouth(at.x, at.y)) {
                // Taken by whoever stands there when it arrives, else the keeper.
                value += carry * -w_;
                out.v = value;
                return out;
            }
            int thief = -1, thiefFrame = kNever;
            for (int d = def; d < def + kPlayersPerTeam; ++d)
                if (!(absent_ >> d & 1u) && arr_[d][cell] < thiefFrame) { thief = d; thiefFrame = arr_[d][cell]; }
            // A defender there before our frame (standing when it arrives, or
            // stepping on it in his frame before ours) takes it.
            const int ourChance = ours;
            if (thief >= 0 && thiefFrame < ourChance && thiefFrame <= theirs) {
                value += carry * -HoldEnd(thief, cell, std::max(thiefFrame, m == 0 ? t + 1 : t + 2 * m - 1));
                out.v = value;
                return out;
            }
            if (ours <= t_) {
                double best = -3;
                int bj = -1;
                for (int j = first; j < first + kPlayersPerTeam; ++j) {
                    if (j == k || j == barred) continue;
                    const int a = OwnArrival(j, cell);
                    if (a == kNever || a > ours) continue;
                    const double v = a_[Index(j, cell, ours, nq)];
                    if (v > best) { best = v; bj = j; }
                }
                if (bj >= 0) {
                    value += carry * best;
                    out = {value, bj, cell, ours};
                    return out;
                }
            }
            if (thief >= 0 && thiefFrame <= theirs) {
                value += carry * -HoldEnd(thief, cell, theirs);
                out.v = value;
                return out;
            }
        }
        out.v = value;   // nobody in the horizon: a loose ball, worth nothing to either
        return out;
    }

    // The best pass of holder k from (c, t) to receiver j (any j != k if j < 0).
    struct PassPick { double v = -2; int j = -1, target = -1, take = -1; double through = 1; unsigned line = 0; int thief = -1; };
    PassPick BestPass(int k, int c, int t, int q, int only) {
        PassPick best;
        if (!CanPass(q)) return best;
        const int team = TeamOf(k), first = FirstPlayerOf(team), def = FirstPlayerOf(Opponent(team));
        for (const PassLine& line : PassLines(c)) {
            const int target = line.target;
            double through = 1, strongest = 0;
            int who = -1, where = -1;
            unsigned mask = 0;
            for (int d = def; d < def + kPlayersPerTeam; ++d) {
                double p = 0;
                int at = -1;
                for (const ZoneCell& z : line.cells)
                    if (z.factor > p && By(d, z.cell, t - 1)) { p = z.factor; at = z.cell; }
                through *= 1 - p;
                if (p > 0) mask |= 1u << d;
                if (p > strongest) { strongest = p; who = d; where = at; }
            }
            const double lost = who >= 0 ? -Bv(who, where, t + 1) : 0;
            const int nq = NextLayer(q, k), barred = Barred(q, team);
            for (int j = first; j < first + kPlayersPerTeam; ++j) {
                if (j == k || j == barred || (only >= 0 && j != only)) continue;
                const int a = OwnArrival(j, target);
                if (a == kNever) continue;
                if (a > t) continue;   // to his feet; a pass ahead rolls (below)
                const double v = through * a0_[Index(j, target, t, nq)] + (1 - through) * lost;
                if (v > best.v) best = {v, j, target, t, through, mask, -1};
            }
            // Ahead: the ball rolls on to whoever takes it first.
            bool someone = false;
            for (int j = first; j < first + kPlayersPerTeam; ++j)
                someone = someone || (j != k && OwnArrival(j, target) <= t);
            if (!someone) {
                const RollPick r = Roll(k, c, t, q, target);
                if (r.j >= 0 && (only < 0 || r.j == only)) {
                    const double v = through * r.v + (1 - through) * lost;
                    if (v > best.v) best = {v, r.j, r.cell, r.frame, through, mask, -1};
                }
            }
        }
        return best;
    }
    // Follow the best choices from k acting on c in frame t.
    void TraceAct(int k, int c, int t, int q, bool noPass, std::vector<Step>& out, int depth = 0) {
        if (depth > 12 || t > t_) return;
        const int team = TeamOf(k);
        const double shot = Shot(team, c, t) * (1 + w_) - w_;
        const double hold = Fight(team, c, t + 1, Bv(k, c, t + 2, q));
        PassPick pass;
        if (!noPass) pass = BestPass(k, c, t, q, -1);
        if (shot >= hold && shot >= pass.v) {
            out.push_back({'s', k, c, c, t, Shot(team, c, t), ZoneMask(team, c, t), shot});
            return;
        }
        if (pass.v > hold) {
            out.push_back({'p', pass.j, c, pass.target, pass.take, pass.through, pass.line, pass.v});
            if (pass.thief >= 0) { out.push_back({'x', pass.thief, pass.target, pass.target, pass.take - 1, 0, 0, 0}); return; }
            TraceAct(pass.j, pass.target, pass.take, NextLayer(q, k), pass.take == t, out, depth + 1);
            return;
        }
        out.push_back({'h', k, c, c, t, Keep(team, c, t + 1), TackleMask(team, c, t + 1), hold});
        if (t + 2 <= t_) TraceStart(k, c, t + 2, q, false, out, depth + 1);
    }
    void TraceStart(int k, int c, int t, int q, bool exact, std::vector<Step>& out, int depth = 0) {
        double best = -3;
        int bd = c, bw = t;
        for (int d = 0; d < kCells; ++d) {
            int when = exact ? (d == c ? t : std::max(t, OwnArrival(k, d))) : DribbleFrame(k, c, t, d);
            if (when > t_) continue;
            const double v = Dribble(k, c, t, d, when, q);
            if (v > best + 1e-12) { best = v; bd = d; bw = when; }
        }
        if (bd != c) out.push_back({'d', k, c, bd, bw, Keep(TeamOf(k), bd, bw - 1), bw > t ? TackleMask(TeamOf(k), bd, bw - 1) : 0u, best});
        TraceAct(k, bd, bw, q, false, out, depth + 1);
    }
    // The holder's first ball action by category: s(hot), h(old), or a pass
    // to each partner, each after his best dribble for it.
    struct Category { char kind; int receiver; double value; std::vector<Step> steps; };
    std::vector<Category> Categories(int k, int c, int t, bool exact) {
        std::vector<Category> cats;
        const int team = TeamOf(k), first = FirstPlayerOf(team);
        std::vector<std::pair<char, int>> kinds = {{'s', -1}, {'h', -1}};
        for (int j = first; j < first + kPlayersPerTeam; ++j)
            if (j != k) kinds.push_back({'p', j});
        for (auto [kind, j] : kinds) {
            Category cat{kind, j, -3, {}};
            int bd = -1, bw = -1;
            PassPick bp;
            for (int d = 0; d < kCells; ++d) {
                int when = exact ? (d == c ? t : std::max(t, OwnArrival(k, d))) : DribbleFrame(k, c, t, d);
                if (when > t_) continue;
                double act;
                PassPick pp;
                if (kind == 's') act = Shot(team, d, when) * (1 + w_) - w_;
                else if (kind == 'h') act = Fight(team, d, when + 1, Bv(k, d, when + 2, 0));
                else { pp = BestPass(k, d, when, 0, j); act = pp.v; if (act < -1.5) continue; }
                const double v = when == t ? act : Fight(team, d, when - 1, act);
                if (v > cat.value) { cat.value = v; bd = d; bw = when; bp = pp; }
            }
            if (bd < 0) continue;
            if (bd != c) cat.steps.push_back({'d', k, c, bd, bw, Keep(team, bd, bw - 1), bw > t ? TackleMask(team, bd, bw - 1) : 0u, cat.value});
            if (kind == 's') cat.steps.push_back({'s', k, bd, bd, bw, Shot(team, bd, bw), ZoneMask(team, bd, bw), cat.value});
            else if (kind == 'h') {
                cat.steps.push_back({'h', k, bd, bd, bw, Keep(team, bd, bw + 1), TackleMask(team, bd, bw + 1), cat.value});
                if (bw + 2 <= t_) TraceStart(k, bd, bw + 2, 0, false, cat.steps);
            } else {
                cat.steps.push_back({'p', bp.j, bd, bp.target, bp.take, bp.through, bp.line, bp.v});
                if (bp.thief >= 0) cat.steps.push_back({'x', bp.thief, bp.target, bp.target, bp.take - 1, 0, 0, 0});
                else TraceAct(bp.j, bp.target, bp.take, NextLayer(0, k), bp.take == bw, cat.steps);
            }
            cats.push_back(std::move(cat));
        }
        return cats;
    }
    int ArrivalOf(int k, int c) const { return arr_[k][c]; }
    int Horizon() const { return t_; }

private:
    std::size_t Index(int k, int c, int t, int q = 0) const {
        return ((static_cast<std::size_t>(q) * kNumPlayers + k) * kCells + c) * stride_ + t;
    }
    std::size_t TIndex(int team, int c, int f) const {
        return (static_cast<std::size_t>(team) * kCells + c) * stride_ + std::min(f, stride_ - 1);
    }
    bool By(int k, int cell, int frame) const { return !(absent_ >> k & 1u) && arr_[k][cell] <= frame; }
    // Next to the holder only through the map's contact: his own area blocks.
    bool Near(int k, int cell, int frame) const {
        return !(absent_ >> k & 1u) && claim_[k][cell] <= frame;
    }

    // Ending a turn with the ball on c: the defenders the map puts next to it
    // by `frame` each try their best tackle.
    double Keep(int team, int c, int f) {
        double& kp = keep_[TIndex(team, c, f)];
        if (kp >= 0) return kp;
        const int def = Opponent(team), first = FirstPlayerOf(def);
        const Cell cc = CellOf(c);
        kp = 1;
        double strongest = 0, won = 0, knocked = 0;
        int who = -1;
        for (int d = first; d < first + kPlayersPerTeam; ++d) {
            double best = 0;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    const Cell n{cc.x + dx, cc.y + dy};
                    if ((dx == 0 && dy == 0) || !InField(n) || !Near(d, ReachFrontIndex(n), f)) continue;
                    best = std::max(best, TackleChance(cc, n, def));
                }
            if (best <= 0) continue;
            const double fight = std::min(1.0, best + kKnockAwayChance);
            won += kp * best;
            knocked += kp * (fight - best);
            kp *= 1 - fight;
            if (best > strongest) { strongest = best; who = d; }
        }
        tackler_[TIndex(team, c, f)] = who;
        won_[TIndex(team, c, f)] = won;
        return kp;
    }
    int Tackler(int team, int c, int f) { Keep(team, c, f); return tackler_[TIndex(team, c, f)]; }
    // A fight on c: kept, won by the tackler (the other side's ball) or knocked
    // away (a free ball, valued 0).
    double Fight(int team, int c, int f, double kept) {
        const double keep = Keep(team, c, f);
        if (keep >= 1) return kept;
        const int x = Tackler(team, c, f);
        const double won = won_[TIndex(team, c, f)];
        return keep * kept + won * (x >= 0 ? -HoldEnd(x, c, f) : 0);
    }

    double Shot(int team, int c, int t) {
        double& v = shot_[TIndex(team, c, t)];
        if (v >= 0) return v;
        v = OpenShotProbability(CellOf(c), team);
        if (v <= 0) { v = 0; return v; }
        const int first = FirstPlayerOf(Opponent(team));
        for (int d = first; d < first + kPlayersPerTeam; ++d) {
            float block = 1;
            for (const ZoneCell& z : ShotZone(team, c))
                if (By(d, z.cell, t - 1)) block = std::min(block, z.factor);
            v *= block;
        }
        return v;
    }

    int DribbleFrame(int k, int c, int t, int d) const {
        if (d == c) return t;
        // With his run: a holder who keeps the ball goes on in the direction he
        // was going (07.11.2026; a standing start at every stop made long
        // dribbles too slow and holding too risky).
        const Cell a0 = CellOf(c), b0 = CellOf(d);
        const int turns = RaceTurns(a0, {Sign(b0.x - a0.x), Sign(b0.y - a0.y)}, b0);
        if (turns >= 1000) return kNever;
        int when = t + 2 * turns;
        const int a = OwnArrival(k, d);
        if (a == kNever) {
            if (TeamOf(k) == ballTeam_) return kNever;   // the map's areas hold for the team with the ball
        } else {
            when = std::max(when, a);
        }
        return when;
    }
    // Dribble from c (frame t) to d, acting there in frame `when`. Turns ended
    // on the way: the risk is taken where he arrives, one frame before.
    double Dribble(int k, int /*c*/, int t, int d, int when, int q = 0) {
        ++transitions;
        const double act = Av(k, d, when, q);
        if (when == t) return act;
        const int team = TeamOf(k);
        return Fight(team, d, when - 1, act);
    }

    void Frame(int t) {
        for (int team = 0; team < kNumTeams; ++team) {
            if (!Own(team, t)) continue;
            const int first = FirstPlayerOf(team);
            const int def = FirstPlayerOf(Opponent(team));
            for (int q = Layers() - 1; q >= 0; --q) {
                // Acting without a pass: a shot or the end of the turn with the ball.
                for (int k = first; k < first + kPlayersPerTeam; ++k)
                    for (int c = 0; c < kCells; ++c) {
                        const double shot = Shot(team, c, t) * (1 + w_) - w_;
                        const double hold = Fight(team, c, t + 1, Bv(k, c, t + 2, q));
                        const std::size_t i = Index(k, c, t, q);
                        a0_[i] = std::max(shot, hold);
                        first_[i] = shot >= hold ? 's' : 'h';
                        a_[i] = a0_[i];
                    }
                // Passes (the q + 1-th of the possession): per line the chance each
                // defender the map puts by it (before the pass) touches the ball;
                // the strongest one wins it.
                if (CanPass(q))
                    for (int c = 0; c < kCells; ++c) {
                        std::array<double, kPlayersPerTeam> pass;
                        pass.fill(-2);
                        for (const PassLine& line : PassLines(c)) {
                            const int target = line.target;
                            double through = 1, strongest = 0;
                            int who = -1, where = -1;
                            for (int d = def; d < def + kPlayersPerTeam; ++d) {
                                double p = 0;
                                int at = -1;
                                for (const ZoneCell& z : line.cells)
                                    if (z.factor > p && By(d, z.cell, t - 1)) { p = z.factor; at = z.cell; }
                                through *= 1 - p;
                                if (p > strongest) { strongest = p; who = d; where = at; }
                            }
                            const double lost = who >= 0 ? -Bv(who, where, t + 1) : 0;
                            bool someone = false;
                            for (int j = first; j < first + kPlayersPerTeam; ++j) {
                                const int a = OwnArrival(j, target);
                                if (a == kNever || a > t) continue;   // to his feet; ahead below
                                someone = true;
                                if (j == Barred(q, team)) continue;
                                for (int k = first; k < first + kPlayersPerTeam; ++k) {
                                    if (k == j) continue;
                                    const double v = through * a0_[Index(j, target, t, NextLayer(q, k))] + (1 - through) * lost;
                                    ++transitions;
                                    pass[k - first] = std::max(pass[k - first], v);
                                }
                            }
                            if (!someone)
                                for (int k = first; k < first + kPlayersPerTeam; ++k) {
                                    const RollPick r = Roll(k, c, t, q, target);
                                    if (r.j < 0) continue;
                                    ++transitions;
                                    pass[k - first] = std::max(pass[k - first], through * r.v + (1 - through) * lost);
                                }
                        }
                        for (int k = first; k < first + kPlayersPerTeam; ++k) {
                            const std::size_t i = Index(k, c, t, q);
                            if (pass[k - first] > a_[i]) { a_[i] = pass[k - first]; first_[i] = 'p'; }
                        }
                    }
                // Holding at the start of the frame: dribble and act.
                for (int k = first; k < first + kPlayersPerTeam; ++k)
                    for (int c = 0; c < kCells; ++c) {
                        double best = -2;
                        for (int d = 0; d < kCells; ++d) {
                            const int when = DribbleFrame(k, c, t, d);
                            if (when > t_) continue;
                            best = std::max(best, Dribble(k, c, t, d, when, q));
                        }
                        b_[Index(k, c, t, q)] = best < -1.5 ? Bv(k, c, t + 2, q) : best;
                    }
            }
        }
    }

    const GameState& s_;
    const ReachFront& front_;
    double w_;
    int ballTeam_;
    int passes_;   // passes allowed in one possession
    unsigned absent_;   // players taken off (analysis: who matters)
    double endBonus_ = 0;
    int h_ = 0, t_ = 0, stride_ = 0;
    std::array<std::array<int, kCells>, kNumPlayers> arr_;
    std::array<std::array<int, kCells>, kNumPlayers> claim_;   // the map's own, with contact
    std::array<int, kNumTeams> firstOwn_{};
    std::vector<double> b_, a_, a0_, keep_, shot_, won_;
    std::vector<int> tackler_;
    std::vector<char> first_;
};

// The players the map puts first on a free ball's untouched path, the cell
// and the frame from which they have it.
unsigned FreeBallTakers(const GameState& s, const ReachFront& front, int* cellOut, int* startOut) {
    const int frames = static_cast<int>(front.frames.size());
    unsigned takers = 0;
    int cell = -1, start = -1;
    for (const BallWindow& win : BallPathWindows(s)) {
        const int c = ReachFrontIndex(win.cell);
        if (win.arrival >= 0) {
            const int f = (win.arrival + 1) / 2;
            if (f < frames && front.frames[f].players[c]) { takers = front.frames[f].players[c]; cell = c; start = f + 1; break; }
        }
        const int firstSlot = win.arrival < 0 ? 0 : (win.arrival + 1) / 2;
        const int lastSlot = win.openEnd ? frames - 2 : win.lastMoveSlot;
        for (int slot = firstSlot; slot <= lastSlot && slot + 1 < frames; ++slot)
            if (front.frames[slot + 1].players[c]) { takers = front.frames[slot + 1].players[c]; cell = c; start = slot + 1; break; }
        if (takers) break;
    }
    *cellOut = cell;
    *startOut = start;
    return takers;
}

// Advance of a team (07.11.2026, user): its players nearer the other goal -
// the sum over its players of 1 - distance to the goal's centre / field length.
// (Territory on the map, tried first, preferred pressing the defenders to
// players in the gaps of their wall: rejected.)
double Territory(const GameState& s, const ReachFront&, int team) {
    const double gy = team == kWhite ? kFirstRow - 0.5 : kLastRow + 0.5;   // White attacks row 1 inside
    const double gx = (kFieldWidth - 1) / 2.0;
    double t = 0;
    for (int k = FirstPlayerOf(team); k < FirstPlayerOf(team) + kPlayersPerTeam; ++k) {
        const Cell c = s.players[k].pos;
        t += 1 - std::hypot(c.x - gx, c.y - gy) / (kLastRow - kFirstRow + 1);
    }
    return t;
}

// The episode after a free ball is taken (07.11.2026, user, g9_17w): the taker
// stands on the ball's cell without velocity, the others as they are. His run
// before the pickup no longer claims cells - to take the ball he had to go to
// it - so the map from here is the map of the possession.
GameState TakerState(const GameState& s, int k, int cell) {
    GameState t = s;
    PlayerState& p = t.players[k];
    p.pos = p.turnStart = p.prevTurnStart = CellOf(cell);
    p.moved = false;
    return t;
}

}  // namespace

PossessionField ComputePossessionField(const GameState& s, double w, int passes, double territory) {
    PossessionField r;
    if (s.phase != Phase::Play || s.keeperRestartTeam >= 0) return r;
    const ReachFront front = ComputeReachFront(s);
    if (front.stop == ReachFrontStop::InvalidStart) return r;
    r.frames = static_cast<int>(front.frames.size());
    const auto finish = [&](PossessionField& x) {
        x.valid = true;
        x.white = x.team == kWhite ? x.value : -x.value;
    };
    if (!s.ball.IsFree()) {
        const int k = s.ball.holder, team = TeamOf(k);
        Field f(s, front, w, team, passes, 0, territory * Territory(s, front, team), k);
        const int c = ReachFrontIndex(s.players[k].pos);
        r.team = team;
        r.holder = k;
        if (team == s.teamToMove) {
            char first = '-';
            r.value = s.players[k].moved ? f.Av(k, c, 1) : f.Start(k, c, 1, true, &first);
            r.first = first;
        } else {
            r.value = f.Wait(k, c, 2);
        }
        r.transitions = f.transitions;
        finish(r);
        return r;
    }
    // A free ball: its taker by the map (as in route_static.cpp).
    const int frames = r.frames;
    int cell = -1, start = -1;
    const unsigned takers = FreeBallTakers(s, front, &cell, &start);
    r.freeBall = true;
    if (!takers) {
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
        if (t.phase == Phase::Play && !t.ball.IsFree()) {
            PossessionField e = ComputePossessionField(t, w, passes, territory);
            if (e.valid) { e.freeBall = e.restart = true; return e; }
        }
        r.restart = true;
        finish(r);
        return r;
    }
    int team = -1;
    for (int k = 0; k < kNumPlayers; ++k)
        if (takers >> k & 1u) team = TeamOf(k);
    bool have = false;
    for (int k = 0; k < kNumPlayers; ++k) {
        if (!(takers >> k & 1u)) continue;
        const GameState taken = TakerState(s, k, cell);
        const ReachFront from = ComputeReachFront(taken);
        if (from.stop == ReachFrontStop::InvalidStart) continue;
        Field f(taken, from, w, team, passes, 0, territory * Territory(taken, from, team), k);
        int t = std::min(start, frames + 2);
        if (!f.Own(team, t)) ++t;
        // Not his own frame when he has it: the other side may tackle him first.
        const double v = t == start ? f.Bv(k, cell, t) : f.HoldEnd(k, cell, t - 2);
        if (!have || v > r.value) { r.value = v; r.holder = k; have = true; r.transitions = f.transitions; }
    }
    r.team = team;
    finish(r);
    return r;
}


ReachOverload ComputeReachOverload(const GameState& s, const ReachFront& front, int team, double w,
                                   int late, int delay) {
    // An attacker is free on a map if he reaches a shooting cell (open shot at
    // least 2W/(1+W)) before every opponent, and no opponent can be in its
    // blocking zone before him: a 1 v 0. A defender is overloaded if taking
    // him off the map frees two attackers or more (07.11.2026, user, g8_47b:
    // the map gives a cell to whoever can be there first, so W3's area covers
    // both B2's and B1's lanes, though he can run only one of them).
    ReachOverload r;
    const double s0 = 2 * w / (1 + w);
    const int first = FirstPlayerOf(team), def = FirstPlayerOf(Opponent(team));
    struct Best { double shot = 0; int cell = -1, frame = 1 << 20; };
    const auto freeOn = [&](const ReachFront& m, std::array<Best, kPlayersPerTeam>* best) {
        std::array<std::array<int, kCells>, kNumPlayers> arr;
        for (auto& a : arr) a.fill(kNever);
        for (int f = 0; f < static_cast<int>(m.frames.size()); ++f)
            for (int i = 0; i < kCells; ++i)
                for (int k = 0; k < kNumPlayers; ++k)
                    if ((m.frames[f].players[i] >> k & 1u) && arr[k][i] == kNever) arr[k][i] = f;
        if (late >= 0)
            for (int i = 0; i < kCells; ++i)
                if (arr[late][i] != kNever) arr[late][i] += delay;
        unsigned freed = 0;
        for (int k = first; k < first + kPlayersPerTeam; ++k) {
            Best b;
            for (int i = 0; i < kCells; ++i) {
                if (arr[k][i] == kNever) continue;
                const double p = OpenShotProbability(CellOf(i), team);
                if (p < s0 || p < b.shot - 1e-9) continue;
                bool open = true;
                for (int d = def; d < def + kPlayersPerTeam && open; ++d) {
                    open = arr[d][i] > arr[k][i];
                    for (const ZoneCell& z : ShotZone(team, i)) open = open && arr[d][z.cell] >= arr[k][i];
                }
                if (!open) continue;
                if (p > b.shot + 1e-9 || arr[k][i] < b.frame) b = {p, i, arr[k][i]};
            }
            if (b.cell >= 0) freed |= 1u << k;
            if (best) (*best)[k - first] = b;
        }
        return freed;
    };
    std::array<Best, kPlayersPerTeam> best0, bestD;
    const unsigned free0 = freeOn(front, &best0);
    std::array<unsigned, kPlayersPerTeam> freedBy{};
    std::array<std::array<Best, kPlayersPerTeam>, kPlayersPerTeam> bests;
    for (int d = def; d < def + kPlayersPerTeam; ++d) {
        const ReachFront m = ComputeReachFront(s, kHalfTurnsPerMatch, 1u << d);
        if (m.stop == ReachFrontStop::InvalidStart) continue;
        freedBy[d - def] = freeOn(m, &bestD) & ~free0;
        bests[d - def] = bestD;
    }
    for (int k = first; k < first + kPlayersPerTeam; ++k) {
        OverloadAttacker a;
        a.player = k;
        if (free0 >> k & 1u) {
            const Best& b = best0[k - first];
            a.cell = b.cell; a.frame = b.frame; a.shot = b.shot;
        } else {
            for (int d = 0; d < kPlayersPerTeam; ++d)
                if (freedBy[d] >> k & 1u) {
                    a.cover |= 1u << (def + d);
                    const Best& b = bests[d][k - first];
                    if (b.shot > a.shot) { a.cell = b.cell; a.frame = b.frame; a.shot = b.shot; }
                }
            if (!a.cover) continue;
        }
        r.attackers.push_back(a);
    }
    // The attackers each held by a single defender: one defender holds one
    // of them (the largest matching); the rest are free - an overload of
    // their defenders (g9_17w: W1 and W3 each hold both B2 and B3, but the two
    // together hold them; g8_47b: B1 and B2 both hold only by W3).
    std::vector<OverloadAttacker*> held;
    for (auto& a : r.attackers)
        if (!(free0 >> a.player & 1u)) held.push_back(&a);
    const int n = static_cast<int>(held.size());
    int bestSize = 0;
    unsigned bestSet = 0;
    for (unsigned set = 0; set < (1u << n); ++set) {
        bool ok = true;
        for (unsigned sub = set; sub && ok; sub = (sub - 1) & set) {
            unsigned u = 0;
            int cnt = 0;
            for (int i = 0; i < n; ++i)
                if (sub >> i & 1u) { u |= held[i]->cover; ++cnt; }
            ok = static_cast<int>(std::bitset<32>(u).count()) >= cnt;
        }
        const int size = static_cast<int>(std::bitset<32>(set).count());
        if (ok && size > bestSize) { bestSize = size; bestSet = set; }
    }
    r.free = free0;
    for (int i = 0; i < n; ++i)
        if (!(bestSet >> i & 1u)) { r.free |= 1u << held[i]->player; r.overloaded |= held[i]->cover; }
    r.deficit = static_cast<int>(std::bitset<8>(r.free).count());
    r.matched = static_cast<int>(r.attackers.size()) - r.deficit;
    return r;
}

std::string AnalyzePossession(const GameState& s, double w, int passes) {
    std::string out;
    char buf[256];
    if (s.phase != Phase::Play || s.keeperRestartTeam >= 0) return "{\"error\":\"not in play\"}";
    ReachFront front = ComputeReachFront(s);
    if (front.stop == ReachFrontStop::InvalidStart) return "{\"error\":\"invalid\"}";
    GameState base = s;
    int k, c, t;
    bool exact, toMove, free = s.ball.IsFree();
    if (!free) {
        k = s.ball.holder;
        c = ReachFrontIndex(s.players[k].pos);
        toMove = TeamOf(k) == s.teamToMove;
        t = toMove ? 1 : 2;
        exact = true;
    } else {
        // The free ball's taker by the map, from where and when he has it.
        int start = -1;
        const unsigned takers = FreeBallTakers(s, front, &c, &start);
        if (!takers) return "{\"error\":\"restart\"}";
        k = -1;
        for (int p = kNumPlayers - 1; p >= 0; --p)
            if (takers >> p & 1u) k = p;
        t = start;
        if ((t & 1) != (TeamOf(k) == s.teamToMove ? 1 : 0)) ++t;
        toMove = true;
        exact = false;
        base = TakerState(s, k, c);
        front = ComputeReachFront(base);
        if (front.stop == ReachFrontStop::InvalidStart) return "{\"error\":\"invalid taker\"}";
    }
    const int team = TeamOf(k);
    const auto value = [&](Field& f) {
        if (free) return f.Start(k, c, t, false, nullptr);
        return toMove ? f.Start(k, c, 1, true, nullptr) : f.Wait(k, c, 2);
    };
    Field f(base, front, w, team, passes, 0, 0, k);
    const double v = value(f);
    const auto cats = f.Categories(k, c, t, exact);
    // Each player taken off in turn: how much the value moves, per category.
    std::snprintf(buf, sizeof buf, "{\"free\":%d,\"cell\":%d,\"start\":%d,\"team\":%d,\"holder\":%d,\"value\":%.4f,\"frames\":%d,\"stop\":%d,\"horizon\":%d,",
                  free ? 1 : 0, c, t, team, k, v, static_cast<int>(front.frames.size()), static_cast<int>(front.stop), f.Horizon());
    out += buf;
    out += "\"arrival\":[";
    for (int p = 0; p < kNumPlayers; ++p) {
        out += p ? ",[" : "[";
        for (int i = 0; i < kCells; ++i) {
            std::snprintf(buf, sizeof buf, "%s%d", i ? "," : "", f.ArrivalOf(p, i) >= (1 << 20) ? -1 : f.ArrivalOf(p, i));
            out += buf;
        }
        out += "]";
    }
    out += "],\"without\":[";
    std::vector<std::vector<double>> without(kNumPlayers);
    for (int p = 0; p < kNumPlayers; ++p) {
        if (p == k) { out += p ? ",null" : "null"; continue; }
        Field g(base, front, w, team, passes, 1u << p, 0, k);
        const double gv = value(g);
        const auto gc = g.Categories(k, c, t, exact);
        std::snprintf(buf, sizeof buf, "%s{\"value\":%.4f,\"cats\":[", p ? "," : "", gv);
        out += buf;
        for (std::size_t i = 0; i < gc.size(); ++i) {
            std::snprintf(buf, sizeof buf, "%s[\"%c\",%d,%.4f]", i ? "," : "", gc[i].kind, gc[i].receiver, gc[i].value);
            out += buf;
        }
        out += "]}";
    }
    out += "],\"cats\":[";
    for (std::size_t i = 0; i < cats.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%s{\"kind\":\"%c\",\"receiver\":%d,\"value\":%.4f,\"steps\":[", i ? "," : "",
                      cats[i].kind, cats[i].receiver, cats[i].value);
        out += buf;
        for (std::size_t j = 0; j < cats[i].steps.size(); ++j) {
            const auto& st = cats[i].steps[j];
            std::snprintf(buf, sizeof buf, "%s[\"%c\",%d,%d,%d,%d,%.4f,%u,%.4f]", j ? "," : "", st.kind, st.player, st.from, st.to,
                          st.frame, st.keep, st.defenders, st.value);
            out += buf;
        }
        out += "]}";
    }
    out += "]";
    const ReachOverload ov = ComputeReachOverload(base, front, team, w, free ? k : -1, free ? t : 0);
    std::snprintf(buf, sizeof buf, ",\"overload\":{\"matched\":%d,\"deficit\":%d,\"free\":%u,\"overloaded\":%u,\"attackers\":[",
                  ov.matched, ov.deficit, ov.free, ov.overloaded);
    out += buf;
    for (std::size_t i = 0; i < ov.attackers.size(); ++i) {
        const auto& a = ov.attackers[i];
        std::snprintf(buf, sizeof buf, "%s[%d,%d,%d,%.3f,%u]", i ? "," : "", a.player, a.cell, a.frame, a.shot, a.cover);
        out += buf;
    }
    out += "]}}";
    return out;
}

namespace {

// Delivery (08.11.2026): see possession_field.h.
class DeliverySolver {
public:
    DeliverySolver(const GameState& s, const ReachFront& front, int holder, double w, const ReachOverload& ov)
        : s_(s), holder_(holder), team_(TeamOf(holder)), def_(FirstPlayerOf(Opponent(TeamOf(holder)))) {
        s0_ = 2 * w / (1 + w);
        overloaded_ = ov.overloaded;
        for (auto& a : arr_) a.fill(kNever);
        const auto fill = [&](const ReachFront& m, unsigned players) {
            for (int f = 0; f < static_cast<int>(m.frames.size()); ++f)
                for (int i = 0; i < kCells; ++i)
                    for (int k = 0; k < kNumPlayers; ++k)
                        if ((players >> k & 1u) && (m.frames[f].players[i] >> k & 1u) && arr_[k][i] == kNever) arr_[k][i] = f;
        };
        const unsigned attack = 0x0Fu << FirstPlayerOf(team_), defence = 0xFFu & ~attack;
        // The defenders' frames from the map without the holder: his claims do
        // not hold anybody off - he is where the ball is (as in the field).
        const ReachFront without = ComputeReachFront(s, kHalfTurnsPerMatch, 1u << holder);
        fill(without.stop != ReachFrontStop::InvalidStart ? without : front, defence);
        // The attackers' frames without the overloaded defenders too: one
        // defender can not hold off two runs, the map without him is where the
        // danger was found. He still acts on the ball (his frames above).
        const ReachFront holderMap = overloaded_ ? ComputeReachFront(s, kHalfTurnsPerMatch, overloaded_) : front;
        fill(holderMap.stop != ReachFrontStop::InvalidStart ? holderMap : front, 1u << holder);
        const ReachFront others = ComputeReachFront(s, kHalfTurnsPerMatch, (1u << holder) | overloaded_);
        fill(others.stop != ReachFrontStop::InvalidStart ? others : front, attack & ~(1u << holder));
        h_ = 0;
        for (const auto& a : arr_)
            for (int f : a)
                if (f != kNever) h_ = std::max(h_, f);
        firstOwn_ = team_ == s.teamToMove ? 1 : 2;
        for (const OverloadAttacker& a : ov.attackers) {
            if (!a.cover) target1_ |= 1u << a.player;
            else if ((a.cover & ~ov.overloaded) == 0) target2_ |= 1u << a.player;
        }
        stride_ = h_ + 4;
        // Run states of the attackers (cell, sign of the last move) after each
        // own frame: the rules' inertia (the 3x3 square one cell ahead), the
        // cells where the map lets him by then.
        for (int k = FirstPlayerOf(team_); k < FirstPlayerOf(team_) + kPlayersPerTeam; ++k) {
            auto& runs = runs_[k - FirstPlayerOf(team_)];
            runs.assign(static_cast<std::size_t>(stride_) * kCells, 0);
            const PlayerState& p = s.players[k];
            int f0;
            Cell c0, v0;
            if (team_ == s.teamToMove && p.moved) { f0 = 1; c0 = p.pos; v0 = p.Displacement(); }
            else { f0 = firstOwn_ - 2; c0 = p.turnStart; v0 = p.Velocity(); }
            std::vector<std::pair<int, int>> cur{{ReachFrontIndex(c0), SignCode({Sign(v0.x), Sign(v0.y)})}};
            if (f0 >= 0) Mark(k, f0, cur[0].first, cur[0].second);
            for (int f = f0 + 2; f <= h_ + 2 && !cur.empty(); f += 2) {
                std::vector<std::pair<int, int>> next;
                for (const auto& [c, sc] : cur) {
                    const Cell cc = CellOf(c), sg = SignOf(sc);
                    for (int dx = -1; dx <= 1; ++dx)
                        for (int dy = -1; dy <= 1; ++dy) {
                            const Cell x{cc.x + sg.x + dx, cc.y + sg.y + dy};
                            if (!InField(x)) continue;
                            const int xi = ReachFrontIndex(x);
                            if (x != cc && arr_[k][xi] > f) continue;
                            const int ns = SignCode({Sign(x.x - cc.x), Sign(x.y - cc.y)});
                            if (Mark(k, f, xi, ns)) next.push_back({xi, ns});
                        }
                }
                cur.swap(next);
            }
        }
    }

    unsigned target1_ = 0, target2_ = 0, overloaded_ = 0;
    long states = 0;

    bool Solve(int target, int maxPasses, int cell, int frame, DeliveryRoute* out) {
        target_ = target;
        maxPasses_ = maxPasses;
        memo_.assign(static_cast<std::size_t>(maxPasses + 1) * kPlayersPerTeam * kCells * 9 * stride_ * 2, Entry());
        const PlayerState& hp = s_.players[holder_];
        const bool moved = team_ == s_.teamToMove && hp.moved;
        const Cell v = moved ? hp.Displacement() : (s_.ball.IsFree() ? Cell{0, 0} : hp.Velocity());
        const int sc = SignCode({Sign(v.x), Sign(v.y)});
        const float val = V(0, holder_, cell, sc, frame, moved ? 1 : 0);
        if (val <= 0) return false;
        DeliveryRoute r;
        r.target = target;
        r.kind = (target1_ >> target & 1u) ? 1 : 2;
        r.value = val;
        int n = 0, j = holder_, c = cell, s = sc, t = frame, m = moved ? 1 : 0;
        double keep = 1;
        for (int guard = 0; guard < 64; ++guard) {
            const Entry& e = memo_[Key(n, j, c, s, t, m)];
            if (e.kind == 'h') {
                r.legs.push_back({'h', j, c, e.a, e.b, 1});
                r.shotCell = e.a;
                r.shotFrame = e.b;
                r.shot = OpenShotProbability(CellOf(e.a), team_);
                break;
            }
            if (e.kind == 'd') {   // the carrier's move in frame t (or none), then the pass or the end of the turn
                r.legs.push_back({'d', j, c, e.a, t, 1});
                s = e.b;
                c = e.a;
                m = 1;
                continue;
            }
            if (e.kind == 'k') {
                const double k2 = 1 - Risk(c, t + 1);
                keep *= k2;
                r.legs.push_back({'k', j, c, c, t, k2});
                t += 2;
                m = 0;
                continue;
            }
            if (e.kind == 'p') {
                keep *= e.keep;
                r.legs.push_back({'p', j, c, e.a, t, e.keep});
                r.legs.push_back({e.how, e.r, e.a, e.c, e.tr, 1});
                ++r.passes;
                ++n;
                j = e.r; c = e.c; s = e.rs; t = e.tr; m = e.how == 'c' ? 1 : 0;
                continue;
            }
            break;
        }
        r.keep = keep;
        r.participants = Participants(r);
        *out = r;
        return true;
    }

private:
    // The episode's participants: the attackers on the route and the
    // defenders who can act on it in time - be on a cell of a pass's line or
    // of the rolling ball's path while it is there, on the ball's cell while
    // it waits, next to the carrier when he holds it at the end of a turn, or
    // in the shot's blocking zone. A defender's frames here come from his
    // inertia alone, not from the map: the runners' claims would cut him off
    // from cells they hold only one at a time (Longpass: W1's area shut B2 in).
    unsigned Participants(const DeliveryRoute& r) const {
        unsigned m = overloaded_ | (1u << holder_) | (1u << r.target);
        struct Spot { int cell, frame; bool adjacent; };
        std::vector<Spot> spots;
        const auto line = [&](int a, int b, int frame) {
            for (const PassLine& l : PassLines(a))
                if (l.target == b)
                    for (const ZoneCell& z : l.cells) spots.push_back({z.cell, frame, false});
        };
        int ballCell = -1, passFrame = -1;
        for (const DeliveryLeg& l : r.legs) {
            m |= 1u << l.player;
            switch (l.kind) {
                case 'd': spots.push_back({l.to, l.frame + 1, true}); break;
                case 'k': spots.push_back({l.from, l.frame + 1, true}); break;
                case 'p': line(l.from, l.to, l.frame - 1); ballCell = l.to; passFrame = l.frame; break;
                case 'f': case 'b': case 'c':
                    if (ballCell >= 0 && l.kind != 'f') {
                        spots.push_back({ballCell, passFrame + 1, false});   // it lies there through their turn
                        line(ballCell, l.to, l.frame - 1);
                    }
                    spots.push_back({l.to, l.frame + 1, true});
                    break;
                default: break;
            }
        }
        if (r.shotCell >= 0)
            for (const ZoneCell& z : ShotZone(team_, r.shotCell)) spots.push_back({z.cell, r.shotFrame - 1, false});
        const int defFirst = Opponent(team_) == s_.teamToMove ? 1 : 2;
        for (int d = def_; d < def_ + kPlayersPerTeam; ++d) {
            const PlayerState& p = s_.players[d];
            const Cell v = p.Velocity();
            const auto frameAt = [&](Cell n) {
                if (n == p.turnStart) return 0;
                const int turns = RaceTurns(p.turnStart, {Sign(v.x), Sign(v.y)}, n);
                return turns >= 1000 ? kNever : defFirst + 2 * (turns - 1);
            };
            bool acts = false;
            for (const Spot& sp : spots) {
                const Cell cc = CellOf(sp.cell);
                const int reach = sp.adjacent ? 1 : 0;
                for (int dx = -reach; dx <= reach && !acts; ++dx)
                    for (int dy = -reach; dy <= reach && !acts; ++dy) {
                        const Cell n{cc.x + dx, cc.y + dy};
                        if ((sp.adjacent && !dx && !dy) || !InField(n)) continue;
                        acts = frameAt(n) <= sp.frame;
                    }
                if (acts) break;
            }
            if (acts) m |= 1u << d;
        }
        return m;
    }

    struct Entry {
        float v = -2; char kind = 0, how = 0;
        int16_t a = 0, b = 0, c = 0, tr = 0, r = 0, rs = 0;
        float keep = 1;
    };
    const GameState& s_;
    int holder_, team_, def_;
    double s0_;
    int h_ = 0, firstOwn_ = 1, stride_ = 4;
    int target_ = -1, maxPasses_ = 0;
    std::array<std::array<int, kCells>, kNumPlayers> arr_;
    std::array<std::vector<uint16_t>, kPlayersPerTeam> runs_;
    std::vector<Entry> memo_;

    static int SignCode(Cell s) { return (s.x + 1) * 3 + (s.y + 1); }
    static Cell SignOf(int code) { return {code / 3 - 1, code % 3 - 1}; }
    bool Mark(int k, int f, int c, int sc) {
        if (f < 0 || f >= stride_) return false;
        uint16_t& m = runs_[k - FirstPlayerOf(team_)][static_cast<std::size_t>(f) * kCells + c];
        if (m >> sc & 1u) return false;
        m |= static_cast<uint16_t>(1u << sc);
        return true;
    }
    // Signs of k's run states on cell c after his own frame f (0: none).
    uint16_t Runs(int k, int f, int c) const {
        if (f < 0 || f >= stride_) return 0;
        return runs_[k - FirstPlayerOf(team_)][static_cast<std::size_t>(f) * kCells + c];
    }
    std::size_t Key(int n, int j, int c, int s, int t, int m) const {
        return (((((static_cast<std::size_t>(n) * kPlayersPerTeam + (j - FirstPlayerOf(team_))) * kCells + c) * 9 + s) * stride_ + t) * 2) + m;
    }
    // Losing the ball to a tackle when holding on c at the end of a turn: the
    // defenders the map puts next to it by frame f; the best single tackle.
    double Risk(int c, int f) const {
        double worst = 0;
        const Cell cc = CellOf(c);
        for (int d = def_; d < def_ + kPlayersPerTeam; ++d)
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    const Cell n{cc.x + dx, cc.y + dy};
                    if ((!dx && !dy) || !InField(n)) continue;
                    if (arr_[d][ReachFrontIndex(n)] <= f)
                        worst = std::max(worst, TackleChance(cc, n, Opponent(team_)) + kKnockAwayChance);
                }
        return std::min(1.0, worst);
    }
    // The ball from a to b goes through the defenders the map puts by the
    // line by frame f (each at his worst cell for it).
    double Line(int a, int b, int f) const {
        for (const PassLine& line : PassLines(a)) {
            if (line.target != b) continue;
            double through = 1;
            for (int d = def_; d < def_ + kPlayersPerTeam; ++d) {
                double worst = 0;
                for (const ZoneCell& z : line.cells)
                    if (arr_[d][z.cell] <= f) worst = std::max(worst, double(z.factor));
                through *= 1 - worst;
            }
            return through;
        }
        return 1;   // adjacent cells: no line
    }
    bool Theirs(int c, int f) const {   // a defender can be on c by frame f
        for (int d = def_; d < def_ + kPlayersPerTeam; ++d)
            if (arr_[d][c] <= f) return true;
        return false;
    }
    // From c with the sign s, in his own frame t (moving in t unless `moved`),
    // the target reaches a shooting cell before every defender but the
    // overloaded ones can be on it or in its blocking zone.
    double Danger(int j, int c, int s, int t, bool moved, int* cellOut, int* frameOut) const {
        const unsigned allowed = (target1_ >> j & 1u) ? 0u : overloaded_;
        double best = 0;
        int bestFrame = kNever;
        for (int i = 0; i < kCells; ++i) {
            const double p = OpenShotProbability(CellOf(i), team_);
            if (p < s0_ || p < best - 1e-12) continue;
            int ts;
            if (i == c) ts = t;
            else {
                const int turns = RaceTurns(CellOf(c), SignOf(s), CellOf(i));
                if (turns >= 1000) continue;
                ts = t + 2 * (moved ? turns : turns - 1);
            }
            if (ts > h_ + 2 || (p <= best + 1e-12 && ts >= bestFrame)) continue;
            bool open = true;
            for (int d = def_; d < def_ + kPlayersPerTeam && open; ++d) {
                if (allowed >> d & 1u) continue;
                open = arr_[d][i] > ts;
                for (const ZoneCell& z : ShotZone(team_, i)) open = open && arr_[d][z.cell] >= ts;
            }
            if (!open) continue;
            best = p;
            bestFrame = ts;
            *cellOut = i;
            *frameOut = ts;
        }
        if (best > 0) best -= 1e-4 * bestFrame;   // equal shots: the earlier one
        return best;
    }

    // The passes from the carrier j on c in frame t (after his move there).
    // The ball runs to q at once (its line crossed by the defenders there by
    // now), then lies on q through their turn and steps at its end, with its
    // velocity less one per axis each step (rules.cpp). A partner takes it:
    //  - 'f' to the feet: he stands on q (his run state there in frame t);
    //  - 'b' the ideal lead pass (user, 08.11.2026): the step at the end of
    //    their turn brings it onto the cell where he stands after his own
    //    move - he goes on with his speed, nobody could tackle him meanwhile;
    //  - 'c' he runs onto it in his frame (his run state on its cell then).
    // A defender who can be on its cell while it lies there takes it; out of
    // the field or into the goal mouth: lost.
    void Passes(int n, int j, int c, int t, Entry& best) {
        const Cell pc = CellOf(c);
        const int first = FirstPlayerOf(team_);
        for (const PassLine& line : PassLines(c)) {
            const int q = line.target;
            double carry = Line(c, q, t - 1);
            if (carry <= best.v) continue;
            // To the feet.
            for (int r = first; r < first + kPlayersPerTeam; ++r) {
                if (r == j) continue;
                const uint16_t signs = Runs(r, t, q);
                for (int rs = 0; rs < 9; ++rs) {
                    if (!(signs >> rs & 1u)) continue;
                    const float v = static_cast<float>(carry * (1 - Risk(q, t + 1))) * V(n + 1, r, q, rs, t + 2, 0);
                    if (v > best.v) best = {v, 'p', 'f', static_cast<int16_t>(q), 0, static_cast<int16_t>(q), static_cast<int16_t>(t + 2), static_cast<int16_t>(r), static_cast<int16_t>(rs), static_cast<float>(carry * (1 - Risk(q, t + 1)))};
                }
            }
            if (InGoalMouth(CellOf(q).x, CellOf(q).y)) continue;
            // The roll: the ball lies on `at` through frames [ours, theirs].
            Cell at = CellOf(q);
            Cell vel{at.x - pc.x, at.y - pc.y};
            vel = {vel.x - Sign(vel.x), vel.y - Sign(vel.y)};
            bool lost = Theirs(q, t + 1);   // taken on q in their turn
            for (int m = 1; m <= 4 && !lost && carry > best.v; ++m) {
                const int stepEnd = t + 2 * m - 1;     // their End Turn: the step
                if (vel.x || vel.y) {
                    const Cell from = at;
                    at = {at.x + vel.x, at.y + vel.y};
                    vel = {vel.x - Sign(vel.x), vel.y - Sign(vel.y)};
                    if (!InField(at) || InGoalMouth(at.x, at.y)) break;
                    carry *= Line(ReachFrontIndex(from), ReachFrontIndex(at), stepEnd);
                }
                const int cell = ReachFrontIndex(at);
                // 'b': standing on it after his own frame stepEnd - 1.
                for (int r = first; r < first + kPlayersPerTeam; ++r) {
                    if (r == j) continue;
                    const uint16_t signs = Runs(r, stepEnd - 1, cell);
                    for (int rs = 0; rs < 9; ++rs) {
                        if (!(signs >> rs & 1u)) continue;
                        const float v = static_cast<float>(carry) * V(n + 1, r, cell, rs, stepEnd + 1, 0);
                        if (v > best.v) best = {v, 'p', 'b', static_cast<int16_t>(q), 0, static_cast<int16_t>(cell), static_cast<int16_t>(stepEnd + 1), static_cast<int16_t>(r), static_cast<int16_t>(rs), static_cast<float>(carry)};
                    }
                }
                // A defender standing there when it arrives, or stepping on it in his next frame.
                if (Theirs(cell, stepEnd)) break;
                // 'c': he runs onto it in his frame stepEnd + 1.
                for (int r = first; r < first + kPlayersPerTeam; ++r) {
                    if (r == j) continue;
                    const uint16_t signs = Runs(r, stepEnd + 1, cell);
                    for (int rs = 0; rs < 9; ++rs) {
                        if (!(signs >> rs & 1u)) continue;
                        const float v = static_cast<float>(carry) * V(n + 1, r, cell, rs, stepEnd + 1, 1);
                        if (v > best.v) best = {v, 'p', 'c', static_cast<int16_t>(q), 0, static_cast<int16_t>(cell), static_cast<int16_t>(stepEnd + 1), static_cast<int16_t>(r), static_cast<int16_t>(rs), static_cast<float>(carry)};
                    }
                }
                lost = Theirs(cell, stepEnd + 2);
            }
        }
    }

    // n: passes made; j holds the ball on c, the sign of his last move s, at
    // his own frame t; m: he has moved in t already.
    float V(int n, int j, int c, int s, int t, int m) {
        if (t > h_ + 2) return 0;
        const std::size_t key = Key(n, j, c, s, t, m);
        if (memo_[key].v > -2) return memo_[key].v;
        ++states;
        Entry best;
        best.v = 0;
        if (j == target_) {
            int sc = -1, sf = -1;
            const double d = Danger(j, c, s, t, m != 0, &sc, &sf);
            if (d > best.v) { best.v = static_cast<float>(d); best.kind = 'h'; best.a = static_cast<int16_t>(sc); best.b = static_cast<int16_t>(sf); }
        }
        if (n < maxPasses_) Passes(n, j, c, t, best);
        // Keep the ball to his next own frame (a tackle may come meanwhile).
        {
            const float v = static_cast<float>(1 - Risk(c, t + 1)) * V(n, j, c, s, t + 2, 0);
            if (v > best.v) { best = Entry(); best.v = v; best.kind = 'k'; }
        }
        // His move with the ball in frame t: the rules' inertia, the map's cells.
        if (!m) {
            const Cell cc = CellOf(c), sg = SignOf(s);
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    const Cell x{cc.x + sg.x + dx, cc.y + sg.y + dy};
                    if (!InField(x) || x == cc) continue;
                    const int xi = ReachFrontIndex(x);
                    if (arr_[j][xi] > t) continue;   // the map does not let him there yet
                    const int ns = SignCode({Sign(x.x - cc.x), Sign(x.y - cc.y)});
                    const float v = V(n, j, xi, ns, t, 1);
                    if (v > best.v) { best = Entry(); best.v = v; best.kind = 'd'; best.a = static_cast<int16_t>(xi); best.b = static_cast<int16_t>(ns); }
                }
        }
        memo_[key] = best;
        return best.v;
    }
};

}  // namespace

Delivery ComputeDelivery(const GameState& s, double w, int maxPasses) {
    Delivery d;
    if (s.phase != Phase::Play || s.keeperRestartTeam >= 0) return d;
    ReachFront front = ComputeReachFront(s);
    if (front.stop == ReachFrontStop::InvalidStart) return d;
    GameState base = s;
    int k, c, t;
    if (!s.ball.IsFree()) {
        k = s.ball.holder;
        c = ReachFrontIndex(s.players[k].pos);
        t = TeamOf(k) == s.teamToMove ? 1 : 2;
    } else {
        int start = -1;
        const unsigned takers = FreeBallTakers(s, front, &c, &start);
        if (!takers) return d;
        k = -1;
        for (int p = kNumPlayers - 1; p >= 0; --p)
            if (takers >> p & 1u) k = p;
        t = start;
        if ((t & 1) != (TeamOf(k) == s.teamToMove ? 1 : 0)) ++t;
        base = TakerState(s, k, c);
        front = ComputeReachFront(base);
        if (front.stop == ReachFrontStop::InvalidStart) return d;
        d.freeBall = true;
    }
    d.team = TeamOf(k);
    d.holder = k;
    d.startCell = c;
    d.startFrame = t;
    d.overload = ComputeReachOverload(base, front, d.team, w, d.freeBall ? k : -1, d.freeBall ? t : 0);
    DeliverySolver solver(base, front, k, w, d.overload);
    d.targets1v0 = solver.target1_;
    d.targets2v1 = solver.target2_;
    for (int target = FirstPlayerOf(d.team); target < FirstPlayerOf(d.team) + kPlayersPerTeam; ++target) {
        if (!((solver.target1_ | solver.target2_) >> target & 1u)) continue;
        double prev = 0;
        for (int passes = 0; passes <= maxPasses; ++passes) {
            DeliveryRoute r;
            if (!solver.Solve(target, passes, c, t, &r)) continue;
            if (r.value <= prev + 1e-6) continue;   // no better with one more pass
            prev = r.value;
            d.routes.push_back(r);
        }
    }
    std::sort(d.routes.begin(), d.routes.end(), [](const DeliveryRoute& a, const DeliveryRoute& b) { return a.value > b.value; });
    d.states = solver.states;
    d.valid = true;
    return d;
}

std::string FormatDelivery(const GameState& s, const Delivery& d) {
    (void)s;
    auto pn = [](int k) { return std::string(k < kPlayersPerTeam ? "W" : "B") + char('1' + k % kPlayersPerTeam); };
    auto cn = [](int i) { return CellName(CellOf(i)); };
    auto mask = [&](unsigned m) {
        std::string r;
        for (int k = 0; k < kNumPlayers; ++k)
            if (m >> k & 1u) r += (r.empty() ? "" : ",") + pn(k);
        return r.empty() ? std::string("-") : r;
    };
    std::string out;
    char buf[512];
    if (!d.valid) return "no delivery: not in play, a restart or no map\n";
    std::snprintf(buf, sizeof buf, "holder %s on %s, frame %d%s; 1 v 0 targets %s, 2 v 1 targets %s (overloaded %s); %ld states\n",
                  pn(d.holder).c_str(), cn(d.startCell).c_str(), d.startFrame, d.freeBall ? " (takes the free ball)" : "",
                  mask(d.targets1v0).c_str(), mask(d.targets2v1).c_str(), mask(d.overload.overloaded).c_str(), d.states);
    out += buf;
    if (!(d.targets1v0 | d.targets2v1)) out += "  no danger on the map: a calm position\n";
    for (const DeliveryRoute& r : d.routes) {
        std::snprintf(buf, sizeof buf, "  %.3f  %s %s, %d pass%s, keep %.3f, shot %.3f from %s at frame %d, participants %s:", r.value,
                      r.kind == 1 ? "1 v 0" : "2 v 1", pn(r.target).c_str(), r.passes, r.passes == 1 ? "" : "es", r.keep,
                      r.shot, cn(r.shotCell).c_str(), r.shotFrame, mask(r.participants).c_str());
        out += buf;
        for (const DeliveryLeg& l : r.legs) {
            switch (l.kind) {
                case 'd': std::snprintf(buf, sizeof buf, " %s %s-%s(f%d)", pn(l.player).c_str(), cn(l.from).c_str(), cn(l.to).c_str(), l.frame); break;
                case 'k': std::snprintf(buf, sizeof buf, " %s ends f%d with it on %s(keep %.2f)", pn(l.player).c_str(), l.frame, cn(l.from).c_str(), l.keep); break;
                case 'p': std::snprintf(buf, sizeof buf, " >%s(f%d, %.2f)", cn(l.to).c_str(), l.frame, l.keep); break;
                case 'f': std::snprintf(buf, sizeof buf, " %s at his feet(f%d)", pn(l.player).c_str(), l.frame); break;
                case 'b': std::snprintf(buf, sizeof buf, " -> %s on %s after their turn(f%d)", pn(l.player).c_str(), cn(l.to).c_str(), l.frame); break;
                case 'c': std::snprintf(buf, sizeof buf, " %s runs onto it on %s(f%d)", pn(l.player).c_str(), cn(l.to).c_str(), l.frame); break;
                case 'h': std::snprintf(buf, sizeof buf, " | %s shoots from %s by f%d", pn(l.player).c_str(), cn(l.to).c_str(), l.frame); break;
                default: buf[0] = 0;
            }
            out += buf;
        }
        out += "\n";
    }
    return out;
}

}  // namespace soccer::bot
