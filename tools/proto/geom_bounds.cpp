// Build: g++ -std=c++17 -O2 -Isrc src/engine/*.cpp src/bot/*.cpp tools/proto/geom_bounds.cpp -o geom_bounds
// Run:   geom_bounds games/game2.ssn 32 33 148 [--alt]   (half-turns of the record)
// Prototype (not part of the bot): bounds of the threat of a position from two
// relaxed games built on the reach tables (docs/bot_design.md, 7.14).
//
// Timeline: the team to move makes its m-th move at half-turn 2(m-1), the other
// team at 2(m-1)+1. A defender "can be at" a cell after n of his moves if his
// reach T(c) <= n (other players ignored).
//
// V_a(D): the best plan of ONE attacker a (dribbles, then a shot) against the
// defenders D, each defender wherever he is most dangerous at every stop (the
// planner's pessimism, but only these defenders):
//   - at the end of every attacker move but the last, each defender of D that can
//     get next to him in his next move tackles from the best cell: keep *= 1 - loss;
//   - the shot from s at move m: open shot x block factors of the defenders of D
//     that can be on the shot line by then; a discount 0.95 per move.
// The holder starts from his state; another attacker gets the ball by a pass at
// his move m to a cell x he can reach, within the pass range of a cell the holder
// can reach (interception ignored); a free ball: the race - the first player
// (either team) to stand on the ball's cell when it is there.
//
//   P = V_holder(all defenders)                    (holder only, all defenders)
//   O = min over assignments f: defenders -> attackers of max_a V_a(f^-1(a))
// (each defender may stop only the attacker he is assigned to, and is assigned
// before the attackers choose: the defence is weaker than in the game).
#include "bot/macro.h"
#include "bot/reach.h"
#include "engine/notation.h"
#include "engine/rules.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

using namespace soccer;
using namespace soccer::bot;

namespace {

constexpr int H = 6;   // attacker moves looked ahead
constexpr double kDisc = 0.95;
constexpr int kV = 5;

int Si(Cell c, Cell v) { return ((c.x * kGridHeight + c.y) * kV + (v.x + 2)) * kV + (v.y + 2); }
constexpr int kStates = kFieldWidth * kGridHeight * kV * kV;

struct Model {
    const GameState* s;
    int team;              // attacking team
    int off;               // 0: the attackers move first, 1: the defenders do
    ReachMap reach[kNumPlayers];
    std::vector<int> defenders;   // indices

    // Loss factor of defender d against the holder standing at x after attacker move m.
    double Keep(int d, Cell x, int m) const {
        const int n = m + off;   // defender moves made by his next move
        double best = -1;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                Cell c{x.x + dx, x.y + dy};
                if ((!dx && !dy) || !InField(c) || reach[d].Turns(c) > n) continue;
                best = std::max(best, TackleChance(x, c, TeamOf(d)));
            }
        return best < 0 ? 1 : 1 - (best + 0.5 * kKnockAwayChance);
    }
    double Block(int d, Cell from, int m) const {
        const int n = m - 1 + off;   // defender moves made before the attacker's m-th
        const int goalRow = AttackedGoalRow(team);
        const Cell goal{kGoalCenterX, goalRow};
        const int left = std::min(from.x, kGoalLeft), right = std::max(from.x, kGoalRight);
        const int lx = goal.x - from.x, ly = goal.y - from.y;
        const double len = std::sqrt(double(lx * lx + ly * ly));
        const int y0 = std::min(from.y, goalRow), y1 = std::max(from.y, goalRow);
        int best = -1;
        for (int x = left; x <= right; ++x)
            for (int y = y0; y <= y1; ++y) {
                if ((x == from.x && y == from.y) || !InField(Cell{x, y}) || reach[d].turns[x][y] > n) continue;
                const int cross = std::abs(lx * (y - from.y) - ly * (x - from.x));
                if (best < 0 || cross < best) best = cross;
            }
        return best >= 0 && len > 0 ? BlockFactor(best / len) : 1.0;
    }
};

// Best value of one attacker from (x, v) at the end of his move m0 (holding the
// ball, not yet tackled at x), against the defenders in `mask` (bits over m.defenders).
// mustSurvive: the defenders' next turn at x is still to come.
struct Dp {
    const Model& md;
    unsigned mask;
    std::vector<double> keepCache, shotCache;   // [m][cell]
    Dp(const Model& m, unsigned k) : md(m), mask(k) {
        keepCache.assign((H + 2) * kFieldWidth * kGridHeight, -1);
        shotCache.assign((H + 2) * kFieldWidth * kGridHeight, -1);
    }
    double Keep(Cell x, int m) {
        double& c = keepCache[(m * kFieldWidth + x.x) * kGridHeight + x.y];
        if (c >= 0) return c;
        double k = 1;
        for (std::size_t i = 0; i < md.defenders.size(); ++i)
            if (mask >> i & 1) k *= md.Keep(md.defenders[i], x, m);
        return c = k;
    }
    double Shot(Cell x, int m) {
        double& c = shotCache[(m * kFieldWidth + x.x) * kGridHeight + x.y];
        if (c >= 0) return c;
        double p = OpenShotProbability(x, md.team);
        for (std::size_t i = 0; i < md.defenders.size(); ++i)
            if (mask >> i & 1) p *= md.Block(md.defenders[i], x, m);
        return c = p;
    }
    // V[m][state]: value at the end of move m, having survived at the cell.
    std::vector<double> V;
    void Solve() {
        V.assign((H + 1) * kStates, 0);
        for (int m = H - 1; m >= 0; --m)
            for (int cx = 0; cx < kFieldWidth; ++cx)
                for (int cy = kFirstRow; cy <= kLastRow; ++cy)
                    for (int vx = -2; vx <= 2; ++vx)
                        for (int vy = -2; vy <= 2; ++vy) {
                            Cell c{cx, cy}, v{vx, vy};
                            Cell center{c.x + Sign(v.x), c.y + Sign(v.y)};
                            double best = 0;
                            for (int dx = -1; dx <= 1; ++dx)
                                for (int dy = -1; dy <= 1; ++dy) {
                                    Cell n{center.x + dx, center.y + dy};
                                    if (!InField(n)) continue;
                                    Cell nv{n.x - c.x, n.y - c.y};
                                    double shoot = kDisc * Shot(n, m + 1);
                                    double go = m + 1 < H ? kDisc * Keep(n, m + 1) * V[(m + 1) * kStates + Si(n, nv)] : 0;
                                    best = std::max(best, std::max(shoot, go));
                                }
                            V[m * kStates + Si(c, v)] = best;
                        }
    }
    // A start: the attacker is at x with the ball at the end of his move m (the
    // defenders' reply still to come), velocity v; he may also shoot right away.
    double Start(Cell x, Cell v, int m, bool canShootNow) {
        double best = canShootNow ? std::pow(kDisc, m) * Shot(x, m == 0 ? 1 : m) : 0;
        if (m < H) best = std::max(best, std::pow(kDisc, m) * Keep(x, m) * V[m * kStates + Si(x, v)]);
        return best;
    }
};

// Value of attacker a (bits `mask` of defenders) as the holder or receiver.
double AttackerValue(const Model& md, int a, unsigned mask) {
    const GameState& s = *md.s;
    Dp dp(md, mask);
    dp.Solve();
    const PlayerState& p = s.players[a];
    if (s.ball.holder == a) {
        // The holder: his first move from his state (or a shot from where he is).
        double best = dp.Shot(p.pos, 1);
        best = std::max(best, dp.V[0 * kStates + Si(p.turnStart, p.Velocity())]);
        // If the defenders move first, the holder has to survive their turn where he stands.
        if (md.off == 1) best *= dp.Keep(p.pos, 0);
        return best;
    }
    if (s.ball.IsFree() || TeamOf(s.ball.holder) != md.team) return 0;
    // A receiver: at move m in a cell x he can reach, within pass range of a cell
    // the holder can reach by then (the pass in the same move).
    const int h = s.ball.holder;
    double best = 0;
    for (int m = 1; m < H; ++m)
        for (int x = 0; x < kFieldWidth; ++x)
            for (int y = kFirstRow; y <= kLastRow; ++y) {
                Cell c{x, y};
                if (md.reach[a].Turns(c) > m) continue;
                bool inRange = false;
                for (int ox = -kMaxPassDistance; ox <= kMaxPassDistance && !inRange; ++ox)
                    for (int oy = -kMaxPassDistance; oy <= kMaxPassDistance && !inRange; ++oy) {
                        Cell yh{x + ox, y + oy};
                        if ((ox || oy) && InField(yh) && md.reach[h].Turns(yh) <= m) inRange = true;
                    }
                if (!inRange) continue;
                best = std::max(best, dp.Start(c, Cell{0, 0}, m, true));
            }
    return best;
}

// Free ball: the race. The ball's cells at each half-turn without anybody
// touching it; the first player to stand on its cell gets it.
double FreeBallValue(const Model& md, int* winner) {
    GameState t = *md.s;
    std::vector<Cell> ball;
    for (int h = 0; h < 2 * H + 2; ++h) {
        ball.push_back(t.ball.pos);
        if (t.phase != Phase::Play) break;
        std::vector<Outcome> outs = OutcomesUnchecked(t, Action::EndTurn());
        Outcome o = outs.front();
        for (const Outcome& x : outs)
            if (x.kind == OutcomeKind::BallThrough) o = x;
        ApplyUnchecked(t, Action::EndTurn(), o);
        if (!t.ball.IsFree()) break;
    }
    const GameState& s = *md.s;
    int bestH = 1 << 20, bestK = -1;
    for (int k = 0; k < kNumPlayers; ++k) {
        const int first = TeamOf(k) == s.teamToMove ? 0 : 1;
        for (int n = 1;; ++n) {
            const int h = 2 * (n - 1) + first;
            if (h >= static_cast<int>(ball.size()) || h >= bestH) break;
            if (md.reach[k].Turns(ball[h]) <= n) {
                bestH = h;
                bestK = k;
                break;
            }
        }
    }
    *winner = bestK;
    if (bestK < 0 || TeamOf(bestK) != md.team) return 0;
    Dp dp(md, (1u << md.defenders.size()) - 1);
    dp.Solve();
    const int m = bestH / 2 + 1;   // the attacker's move of the pick-up
    return dp.Start(ball[bestH], Cell{0, 0}, m, true);
}

struct Bounds {
    double P = 0, O = 0;
    double seconds = 0;
    std::string detail;
};

Bounds Evaluate(const GameState& s, int team) {
    auto t0 = std::chrono::steady_clock::now();
    Model md;
    md.s = &s;
    md.team = team;
    md.off = s.teamToMove == team ? 0 : 1;
    for (int k = 0; k < kNumPlayers; ++k) md.reach[k] = PlayerReach(s, k);
    for (int k = FirstPlayerOf(Opponent(team)); k < FirstPlayerOf(Opponent(team)) + kPlayersPerTeam; ++k)
        md.defenders.push_back(k);
    Bounds b;
    char buf[200];
    if (s.ball.IsFree()) {
        int w;
        b.P = b.O = FreeBallValue(md, &w);
        std::snprintf(buf, sizeof buf, "free ball: %s first", w >= 0 ? PlayerName(w).c_str() : "nobody");
        b.detail = buf;
    } else if (TeamOf(s.ball.holder) == team) {
        const unsigned all = (1u << md.defenders.size()) - 1;
        std::vector<int> attackers;
        for (int k = FirstPlayerOf(team); k < FirstPlayerOf(team) + kPlayersPerTeam; ++k) attackers.push_back(k);
        // V[a][subset]
        std::vector<std::vector<double>> val(attackers.size(), std::vector<double>(all + 1));
        for (std::size_t i = 0; i < attackers.size(); ++i)
            for (unsigned sub = 0; sub <= all; ++sub) val[i][sub] = AttackerValue(md, attackers[i], sub);
        b.P = val[std::find(attackers.begin(), attackers.end(), s.ball.holder) - attackers.begin()][all];
        // O: min over assignments of the defenders to the attackers of the max.
        const int na = static_cast<int>(attackers.size()), nd = static_cast<int>(md.defenders.size());
        int combos = 1;
        for (int i = 0; i < nd; ++i) combos *= na;
        b.O = 1e9;
        int bestF = 0;
        for (int f = 0; f < combos; ++f) {
            std::vector<unsigned> sub(na, 0);
            int x = f;
            for (int d = 0; d < nd; ++d, x /= na) sub[x % na] |= 1u << d;
            double mx = 0;
            for (int a = 0; a < na; ++a) mx = std::max(mx, val[a][sub[a]]);
            if (mx < b.O) {
                b.O = mx;
                bestF = f;
            }
        }
        std::string d = "alone:";
        for (int a = 0; a < na; ++a) {
            std::snprintf(buf, sizeof buf, " %s %.2f", PlayerName(attackers[a]).c_str(), val[a][0]);
            d += buf;
        }
        d += " | best assignment:";
        int x = bestF;
        for (int dd = 0; dd < nd; ++dd, x /= na)
            d += " " + PlayerName(md.defenders[dd]) + ">" + PlayerName(attackers[x % na]);
        b.detail = d;
    }
    b.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    std::ifstream f(argv[1]);
    std::stringstream buf;
    buf << f.rdbuf();
    GameRecord r;
    ReadRecord(buf.str(), &r, nullptr);
    std::vector<int> wanted;
    bool alternatives = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--alt") alternatives = true;
        else wanted.push_back(std::atoi(argv[i]));
    }
    GameState s = RecordStart(r);
    int last = -1;
    for (const RecordedAction& ra : r.actions) {
        if (s.phase == Phase::Play && s.turnId != last) {
            last = s.turnId;
            if (std::find(wanted.begin(), wanted.end(), s.halfTurns) != wanted.end()) {
                std::printf("%3d %s\n", s.halfTurns, FormatPosition(s).c_str());
                if (alternatives && s.phase == Phase::Play &&
                    (s.ball.IsFree() || TeamOf(s.ball.holder) != s.teamToMove)) {
                    // Every defence candidate of the team to move: the opponent's bounds after it.
                    MacroOptions mo;
                    mo.table = &ValueTable::Zero();
                    MacroGenerator gen(mo);
                    MacroGenerator::Result res = gen.Generate(s);
                    std::vector<Macro> all = res.macros;
                    std::vector<Macro> extra = gen.ToTheBall(s, all);
                    all.insert(all.end(), extra.begin(), extra.end());
                    for (const Macro& m : all) {
                        MacroPlay play = MacroGenerator::Play(s, false, m.actions);
                        GameState t = play.outcomes.front().state;   // the most likely outcome
                        std::string acts;
                        GameState u = s;
                        for (const Action& a : play.played) {
                            acts += " " + (a.type == ActionType::EndTurn ? std::string("end") : FormatAction(u, a));
                            std::vector<Outcome> outs = OutcomesUnchecked(u, a);
                            ApplyUnchecked(u, a, outs.front());
                        }
                        Bounds b = Evaluate(t, Opponent(s.teamToMove));
                        std::printf("     %-34s static %7.3f | opponent P %.3f O %.3f  %s\n", acts.c_str(), m.estimate, b.P,
                                    b.O, b.detail.substr(0, 60).c_str());
                    }
                }
                for (int team : {kWhite, kBlack}) {
                    Bounds b = Evaluate(s, team);
                    if (b.detail.empty()) continue;
                    std::printf("     threat of %s: P %.3f  O %.3f  (%.0f ms)  %s\n", team == kWhite ? "White" : "Black", b.P,
                                b.O, 1e3 * b.seconds, b.detail.c_str());
                }
            }
        }
        ApplyOutcome(s, ra.action, ra.outcome, nullptr);
    }
}
