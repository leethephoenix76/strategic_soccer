#include "bot/ball_race.h"

#include "engine/rules.h"
#include "engine/notation.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

namespace soccer {
namespace bot {

namespace {

constexpr int kNoTurns = 255;

// ---------------------------------------------------------------- inertia per axis
//
// A move is a displacement d with each component in sign(previous) + {-1,0,1},
// and the next sign is sign(d) (docs/game_model.md, "Движение игроков"). The
// two axes are independent and a player may keep any axis still, so the
// earliest turn at which he can stand on (x, y) is the larger of the earliest
// turns along x and along y, and once there he can stay.
struct AxisTables {
    // [axis][start][sign + 1][target]: earliest number of turns, kNoTurns if never.
    uint8_t turns[2][kGridHeight][3][kGridHeight];

    AxisTables() {
        std::memset(turns, kNoTurns, sizeof turns);
        for (int axis = 0; axis < 2; ++axis) {
            const int lo = axis == 0 ? 0 : kFirstRow, hi = axis == 0 ? kFieldWidth - 1 : kLastRow;
            for (int x0 = lo; x0 <= hi; ++x0)
                for (int s0 = -1; s0 <= 1; ++s0) {
                    uint8_t* out = turns[axis][x0][s0 + 1];
                    bool seen[kGridHeight][3] = {};
                    std::vector<std::pair<int, int>> layer{{x0, s0}}, next;
                    seen[x0][s0 + 1] = true;
                    out[x0] = 0;
                    for (int t = 1; !layer.empty(); ++t) {
                        next.clear();
                        for (auto [x, s] : layer)
                            for (int d = s - 1; d <= s + 1; ++d) {
                                const int y = x + d;
                                if (y < lo || y > hi) continue;
                                const int ns = Sign(d);
                                if (seen[y][ns + 1]) continue;
                                seen[y][ns + 1] = true;
                                next.push_back({y, ns});
                                if (out[y] == kNoTurns) out[y] = static_cast<uint8_t>(t);
                            }
                        layer.swap(next);
                    }
                }
        }
    }
};

const AxisTables& Axis() {
    static const AxisTables tables;
    return tables;
}

Cell SignOf(Cell v) { return {Sign(v.x), Sign(v.y)}; }

// ---------------------------------------------------------------- blocking geometry
//
// Which other players can make a move impossible. Blocking is a property of
// the relative geometry of the start, the target and the other players in the
// 3x3 square around the centre, and more players never unblock a move, so it is
// described by single blocking cells and blocking pairs (the "corridor" of two
// players at the corners of a diagonal). The table is read off the engine's own
// MoveDestinations once.
struct MoveBlockers {
    std::vector<Cell> singles;                  // offsets from the start
    std::vector<std::pair<Cell, Cell>> pairs;   // offsets from the start
};

struct BlockingTable {
    MoveBlockers moves[3][3][3][3];   // [sign x][sign y][target x - centre x][target y - centre y]

    BlockingTable() {
        const Cell start{5, 10};
        for (int sx = -1; sx <= 1; ++sx)
            for (int sy = -1; sy <= 1; ++sy) {
                const Cell center{start.x + sx, start.y + sy};
                std::vector<Cell> around;
                for (int i = -1; i <= 1; ++i)
                    for (int j = -1; j <= 1; ++j) {
                        const Cell c{center.x + i, center.y + j};
                        if (c != start) around.push_back(c);
                    }
                const auto reachable = [&](const std::vector<Cell>& others) {
                    GameState s = NewMatch(1);
                    s.phase = Phase::Play;
                    s.teamToMove = kWhite;
                    const Cell parked[kNumPlayers] = {{0, 1}, {1, 1}, {2, 1}, {8, 1}, {9, 1}, {10, 1}, {0, 19}, {10, 19}};
                    for (int k = 0; k < kNumPlayers; ++k) {
                        PlayerState& p = s.players[k];
                        p = PlayerState();
                        p.pos = p.turnStart = p.prevTurnStart = parked[k];
                    }
                    PlayerState& me = s.players[0];
                    me.pos = me.turnStart = start;
                    me.prevTurnStart = {start.x - sx, start.y - sy};
                    for (std::size_t i = 0; i < others.size(); ++i) {
                        PlayerState& p = s.players[1 + i];
                        p.pos = p.turnStart = p.prevTurnStart = others[i];
                    }
                    s.ball.holder = kNoPlayer;
                    s.ball.pos = {10, 10};
                    bool ok[3][3] = {};
                    for (Cell d : MoveDestinations(s, 0)) ok[d.x - center.x + 1][d.y - center.y + 1] = true;
                    std::array<bool, 9> r{};
                    for (int i = 0; i < 3; ++i)
                        for (int j = 0; j < 3; ++j) r[i * 3 + j] = ok[i][j];
                    return r;
                };
                const std::array<bool, 9> free = reachable({});
                std::array<std::array<bool, 9>, 9> single{};
                for (std::size_t a = 0; a < around.size(); ++a) single[a] = reachable({around[a]});
                for (int t = 0; t < 9; ++t) {
                    if (!free[t]) continue;   // never happens away from the edges
                    MoveBlockers& m = moves[sx + 1][sy + 1][t / 3][t % 3];
                    for (std::size_t a = 0; a < around.size(); ++a)
                        if (!single[a][t]) m.singles.push_back({around[a].x - start.x, around[a].y - start.y});
                    for (std::size_t a = 0; a < around.size(); ++a)
                        for (std::size_t b = a + 1; b < around.size(); ++b) {
                            if (!single[a][t] || !single[b][t]) continue;
                            if (!reachable({around[a], around[b]})[t])
                                m.pairs.push_back({{around[a].x - start.x, around[a].y - start.y},
                                                   {around[b].x - start.x, around[b].y - start.y}});
                        }
                }
            }
    }
};

const BlockingTable& Blocking() {
    static const BlockingTable table;
    return table;
}

// Is the move a -> t (t in the destination square of a player at a whose
// previous displacement had signs `sign`) blocked when `mayStand(c)` gives the
// bits of the players that may stand on cell c?
template <class MayStand>
bool MayBeBlocked(Cell a, Cell sign, Cell t, MayStand mayStand) {
    const Cell center{a.x + sign.x, a.y + sign.y};
    const MoveBlockers& b = Blocking().moves[sign.x + 1][sign.y + 1][t.x - center.x + 1][t.y - center.y + 1];
    for (Cell o : b.singles)
        if (mayStand(Cell{a.x + o.x, a.y + o.y})) return true;
    for (const auto& [first, second] : b.pairs) {
        const unsigned m1 = mayStand(Cell{a.x + first.x, a.y + first.y});
        const unsigned m2 = mayStand(Cell{a.x + second.x, a.y + second.y});
        if (m1 && m2 && !(m1 == m2 && (m1 & (m1 - 1)) == 0)) return true;   // two different players
    }
    return false;
}

// ---------------------------------------------------------------- players' turns

struct Runner {
    Cell from;
    Cell sign;       // signs of the displacement that sets his next centre
    int firstSlot;   // slot of his first remaining turn
};

Runner RunnerOf(const GameState& s, int k) {
    const PlayerState& p = s.players[k];
    return {p.pos, SignOf(p.moved ? p.Displacement() : p.Velocity()), RaceFirstSlot(s, k)};
}

// Own turns a player has had by the end of `slot` (slot -1: none).
int TurnsBy(int firstSlot, int slot) { return slot < firstSlot ? 0 : (slot - firstSlot) / 2 + 1; }
int SlotOfTurn(int firstSlot, int turn) { return turn == 0 ? -1 : firstSlot + 2 * (turn - 1); }
// First own slot not earlier than `slot`.
int OwnSlotFrom(int firstSlot, int slot) {
    if (slot <= firstSlot) return firstSlot;
    return firstSlot + (slot - firstSlot + 1) / 2 * 2;
}

// The earliest moment the player takes the ball, given the earliest number of
// own turns to stand on each cell.
template <class TurnsTo>
int EarliestTake(const std::vector<BallWindow>& windows, int firstSlot, TurnsTo turnsTo,
                 bool guaranteed, Cell* where) {
    int best = kRaceNever;
    for (const BallWindow& w : windows) {
        const int t = turnsTo(w.cell);
        if (t >= kNoTurns) continue;
        if (w.arrival >= 0) {
            const int slot = (w.arrival - 1) / 2;
            if (t <= TurnsBy(firstSlot, slot) && w.arrival < best &&
                (!guaranteed || w.certainStand)) {
                best = w.arrival;
                *where = w.cell;
            }
        }
        if (w.lastMoveSlot < 0 && !w.openEnd) continue;
        if (t == 0) continue;   // standing there: taken at the arrival above
        const int from = w.arrival >= 0 ? (w.arrival - 1) / 2 + 1 : 0;
        const int slot = std::max(OwnSlotFrom(firstSlot, from), SlotOfTurn(firstSlot, t));
        if ((w.openEnd || slot <= w.lastMoveSlot) && 2 * slot < best) {
            best = 2 * slot;
            *where = w.cell;
        }
    }
    return best;
}

// Earliest own turns along routes that no opponent can block. Team-mates keep
// their cells; an opponent may be on any cell he can reach by then.
struct GuaranteedReach {
    uint8_t turns[kFieldWidth][kGridHeight];
};

// The cell where the ball lies during `slot` (untouched), or an off-field cell.
Cell BallCellIn(const std::vector<BallWindow>& windows, int slot) {
    for (const BallWindow& w : windows) {
        const int from = w.arrival >= 0 ? (w.arrival - 1) / 2 + 1 : 0;
        if (slot >= from && (w.openEnd || slot <= w.lastMoveSlot)) return w.cell;
    }
    return {-1, -1};
}

GuaranteedReach ComputeGuaranteedReach(const GameState& s, int k, const Runner runners[kNumPlayers],
                                       const std::vector<BallWindow>& windows, int horizon) {
    GuaranteedReach r;
    std::memset(r.turns, kNoTurns, sizeof r.turns);
    const Runner& me = runners[k];
    r.turns[me.from.x][me.from.y] = 0;
    if (s.players[k].attacked) return r;   // the direction of his attack restricts him: no guarantee

    // Bits of the players that may stand on each cell when he makes his m-th move.
    uint8_t occupied[kFieldWidth][kGridHeight];
    const auto mayStand = [&](Cell c) -> unsigned {
        return InField(c) ? occupied[c.x][c.y] : 0u;
    };

    bool layer[kFieldWidth][kGridHeight][3][3] = {};
    layer[me.from.x][me.from.y][me.sign.x + 1][me.sign.y + 1] = true;
    for (int m = 1; m <= horizon; ++m) {
        const int slot = SlotOfTurn(me.firstSlot, m);
        std::memset(occupied, 0, sizeof occupied);
        for (int q = 0; q < kNumPlayers; ++q) {
            if (q == k) continue;
            const unsigned bit = 1u << q;
            if (TeamOf(q) == TeamOf(k)) {
                if (InField(s.players[q].pos)) occupied[s.players[q].pos.x][s.players[q].pos.y] |= bit;
                continue;
            }
            const int n = TurnsBy(runners[q].firstSlot, slot - 1);
            const Runner& o = runners[q];
            if (!InField(o.from)) continue;
            for (int x = 0; x < kFieldWidth; ++x) {
                const int tx = Axis().turns[0][o.from.x][o.sign.x + 1][x];
                if (tx > n) continue;
                for (int y = kFirstRow; y <= kLastRow; ++y)
                    if (Axis().turns[1][o.from.y][o.sign.y + 1][y] <= n) occupied[x][y] |= bit;
            }
        }
        // An opponent on the ball's cell would have taken the ball already, before
        // this move: then his lower bound is earlier and no guarantee is claimed.
        const Cell ball = BallCellIn(windows, slot);
        if (InField(ball)) {
            unsigned mates = 0;
            for (int q = FirstPlayerOf(TeamOf(k)); q < FirstPlayerOf(TeamOf(k)) + kPlayersPerTeam; ++q) mates |= 1u << q;
            occupied[ball.x][ball.y] &= mates;
        }

        bool next[kFieldWidth][kGridHeight][3][3] = {};
        bool any = false;
        for (int x = 0; x < kFieldWidth; ++x)
            for (int y = kFirstRow; y <= kLastRow; ++y)
                for (int sx = 0; sx < 3; ++sx)
                    for (int sy = 0; sy < 3; ++sy) {
                        if (!layer[x][y][sx][sy]) continue;
                        any = true;
                        next[x][y][1][1] = true;   // a turn without moving
                        const Cell a{x, y};
                        const Cell center{x + sx - 1, y + sy - 1};
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j) {
                                const Cell t{center.x + i - 1, center.y + j - 1};
                                if (!InField(t) || t == a) continue;
                                if (MayBeBlocked(a, {sx - 1, sy - 1}, t, mayStand)) continue;
                                const int nsx = Sign(t.x - x), nsy = Sign(t.y - y);
                                next[t.x][t.y][nsx + 1][nsy + 1] = true;
                                if (r.turns[t.x][t.y] == kNoTurns) r.turns[t.x][t.y] = static_cast<uint8_t>(m);
                            }
                    }
        if (!any) break;
        std::memcpy(layer, next, sizeof layer);
    }
    return r;
}

// The ball's path, untouched, with the moments it can be taken on each cell.
void BuildWindows(const GameState& s, BallRaceResult* r) {
    const BallState& b = s.ball;
    const int firstStepSlot = b.stepPhase == 0 ? 0 : 1;
    Cell pos = b.pos, vel = b.vel;
    BallWindow now;
    now.cell = pos;
    now.openEnd = vel == Cell{0, 0};
    now.lastMoveSlot = now.openEnd ? -1 : firstStepSlot;
    r->windows.push_back(now);
    for (int j = 0; vel != Cell{0, 0}; ++j) {
        const int slot = firstStepSlot + 2 * j;
        const int moment = 2 * slot + 1;
        // Out of play: from the edge outwards, or a clipped step that nobody meets.
        if (!InField({pos.x + Sign(vel.x), pos.y + Sign(vel.y)})) {
            r->endMoment = moment;
            r->endTeam = Opponent(b.lastTouchTeam);
            r->endCertain = true;
            return;
        }
        Cell end{pos.x + vel.x, pos.y + vel.y};
        const bool outside = !InField(end);
        end.x = std::max(0, std::min(kFieldWidth - 1, end.x));
        end.y = std::max(kFirstRow, std::min(kLastRow, end.y));
        // Goal mouth cells on the step (as in the engine's FindMouthCell): an
        // empty one is the keeper's.
        std::vector<Cell> mouth;
        if (pos.y == end.y && pos.x != end.x && (end.y == kFirstRow || end.y == kLastRow)) {
            const int step = end.x > pos.x ? 1 : -1;
            for (int x = pos.x + step; x != end.x + step; x += step)
                if (InGoalMouth(x, end.y)) mouth.push_back({x, end.y});
        } else if (InGoalMouth(end.x, end.y)) {
            mouth.push_back(end);
        }
        if (!mouth.empty()) {
            const int keeperTeam = MouthOwner(mouth.front().y);
            const bool backPass = b.lastTouchWasPass && b.lastTouchTeam == keeperTeam;
            BallWindow w;
            w.cell = end;
            w.arrival = moment;
            // Standing on the end cell takes the ball only if every mouth cell
            // on the step is occupied too.
            w.certainStand = mouth.size() == 1 && mouth.front() == end;
            r->windows.push_back(w);
            r->endMoment = moment;
            r->endKeeper = true;
            r->endTeam = backPass ? Opponent(keeperTeam) : keeperTeam;
            r->endCertain = w.certainStand;
            return;
        }
        BallWindow w;
        w.cell = end;
        w.arrival = moment;
        if (outside) {   // only a player standing on the clipped cell keeps it in play
            r->windows.push_back(w);
            r->endMoment = moment;
            r->endTeam = Opponent(b.lastTouchTeam);
            r->endCertain = true;
            return;
        }
        pos = end;
        if (vel.x) vel.x -= Sign(vel.x);
        if (vel.y) vel.y -= Sign(vel.y);
        w.openEnd = vel == Cell{0, 0};
        w.lastMoveSlot = w.openEnd ? -1 : slot + 2;
        r->windows.push_back(w);
    }
}

}  // namespace

double BreakawayShotLowerBound(const GameState& s, int horizonTurns, double floor, BreakawayAhead ahead) {
    const int opponentHeadStart = ahead.opponentHeadStart;
    if (s.phase != Phase::Play || s.ball.IsFree() || s.keeperRestartTeam >= 0) return 0;
    const int k = s.ball.holder;
    const int team = TeamOf(k);
    if (s.players[k].attacked) return 0;
    Runner runners[kNumPlayers];
    for (int q = 0; q < kNumPlayers; ++q) {
        runners[q] = RunnerOf(s, q);
        if (ahead.origin && TeamOf(q) != team && !(ahead.exact >> q & 1u) && ahead.opponentHeadStart > 0)
            runners[q].sign = RunnerOf(*ahead.origin, q).sign;
    }
    const Runner& me = runners[k];
    // No cell the holder can reach at all gives more than `floor`: nothing to prove.
    {
        double open = 0;
        for (int x = 0; x < kFieldWidth; ++x)
            for (int y = kFirstRow; y <= kLastRow; ++y)
                if (RaceTurns(me.from, me.sign, {x, y}) <= horizonTurns)
                    open = std::max(open, OpenShotProbability({x, y}, team));
        if (open <= floor) return 0;
    }
    // Cells each opponent can stand on after n of his turns: rows of column bits.
    constexpr int kMaxN = 32;
    const int maxN = std::min(kMaxN - 1, horizonTurns + 2 + opponentHeadStart);
    uint16_t masks[kNumPlayers][kMaxN][kGridHeight] = {};
    for (int q = 0; q < kNumPlayers; ++q) {
        const Runner& o = runners[q];
        if (TeamOf(q) == team || !InField(o.from)) continue;
        for (int n = 0; n <= maxN; ++n)
            for (int x = 0; x < kFieldWidth; ++x) {
                if (Axis().turns[0][o.from.x][o.sign.x + 1][x] > n) continue;
                for (int y = kFirstRow; y <= kLastRow; ++y)
                    if (Axis().turns[1][o.from.y][o.sign.y + 1][y] <= n) masks[q][n][y] |= static_cast<uint16_t>(1u << x);
            }
    }
    // Opponent q can stand on c after his turns up to `slot`.
    const auto reach = [&](int q, Cell c, int slot) {
        if (!InField(c) || !InField(runners[q].from)) return false;
        const int n = std::min(maxN, TurnsBy(runners[q].firstSlot, slot) +
                                         ((ahead.exact >> q & 1u) ? 0 : opponentHeadStart));
        return (masks[q][n][c.y] >> c.x & 1u) != 0;
    };
    const int goalRow = AttackedGoalRow(team) == 0 ? kFirstRow : kLastRow;
    double best = 0;
    // The worst shot from c in `slot`, after the opponents' turns before it;
    // only computed when it could beat the best so far.
    const auto shot = [&](Cell c, int slot) {
        double p = OpenShotProbability(c, team);
        if (p <= best || p <= floor) return 0.0;
        const int left = std::min(c.x, kGoalLeft), right = std::max(c.x, kGoalRight);
        const int lowY = std::min(c.y, goalRow), highY = std::max(c.y, goalRow);
        for (int q = 0; q < kNumPlayers && p > best; ++q) {
            if (q == k) continue;
            if (TeamOf(q) == team) {
                p *= ShotBlockFactor(c, team, s.players[q].pos);
                continue;
            }
            double worst = 1;
            for (int x = left; x <= right; ++x)
                for (int y = lowY; y <= highY; ++y)
                    if (Cell{x, y} != c && reach(q, {x, y}, slot - 1))
                        worst = std::min(worst, ShotBlockFactor(c, team, {x, y}));
            p *= worst;
        }
        return p;
    };
    // Nobody of the other team can be next to c after his turns up to `slot`.
    const auto safe = [&](Cell c, int slot) {
        for (int q = FirstPlayerOf(Opponent(team)); q < FirstPlayerOf(Opponent(team)) + kPlayersPerTeam; ++q)
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    if ((dx || dy) && reach(q, {c.x + dx, c.y + dy}, slot)) return false;
        return true;
    };

    if (team == s.teamToMove && !s.ballPlayed) best = shot(me.from, 0);
    if (me.firstSlot > 0 && !safe(me.from, me.firstSlot - 1)) return best;

    uint8_t occupied[kFieldWidth][kGridHeight];
    const auto mayStand = [&](Cell c) -> unsigned { return InField(c) ? occupied[c.x][c.y] : 0u; };
    bool layer[kFieldWidth][kGridHeight][3][3] = {};
    layer[me.from.x][me.from.y][me.sign.x + 1][me.sign.y + 1] = true;
    for (int m = 1; m <= horizonTurns; ++m) {
        const int slot = SlotOfTurn(me.firstSlot, m);
        std::memset(occupied, 0, sizeof occupied);
        for (int q = 0; q < kNumPlayers; ++q) {
            if (q == k) continue;
            if (TeamOf(q) == team) {
                if (InField(s.players[q].pos)) occupied[s.players[q].pos.x][s.players[q].pos.y] |= 1u << q;
                continue;
            }
            for (int x = 0; x < kFieldWidth; ++x)
                for (int y = kFirstRow; y <= kLastRow; ++y)
                    if (reach(q, {x, y}, slot - 1)) occupied[x][y] |= 1u << q;
        }
        bool next[kFieldWidth][kGridHeight][3][3] = {};
        bool any = false;
        for (int x = 0; x < kFieldWidth; ++x)
            for (int y = kFirstRow; y <= kLastRow; ++y)
                for (int sx = 0; sx < 3; ++sx)
                    for (int sy = 0; sy < 3; ++sy) {
                        if (!layer[x][y][sx][sy]) continue;
                        const Cell a{x, y};
                        // Standing: he shoots from here now or stays for later.
                        if (slot > 0 || !s.ballPlayed) best = std::max(best, shot(a, slot));
                        if (safe(a, slot + 1)) { next[x][y][1][1] = true; any = true; }
                        if (m == 1 && ahead.holderMoved) continue;
                        const Cell center{x + sx - 1, y + sy - 1};
                        for (int i = 0; i < 3; ++i)
                            for (int j = 0; j < 3; ++j) {
                                const Cell t{center.x + i - 1, center.y + j - 1};
                                if (!InField(t) || t == a) continue;
                                if (MayBeBlocked(a, {sx - 1, sy - 1}, t, mayStand)) continue;
                                // Dribbled to t: he shoots there in this turn, or ends
                                // the turn there if nobody can tackle him next.
                                if (slot > 0 || !s.ballPlayed) best = std::max(best, shot(t, slot));
                                if (!safe(t, slot + 1)) continue;
                                next[t.x][t.y][Sign(t.x - x) + 1][Sign(t.y - y) + 1] = true;
                                any = true;
                            }
                    }
        if (!any) break;
        std::memcpy(layer, next, sizeof layer);
    }
    return best;
}

int RaceTurns(Cell from, Cell sign, Cell target) {
    if (!InField(from) || !InField(target)) return kNoTurns;
    return std::max(Axis().turns[0][from.x][Sign(sign.x) + 1][target.x],
                    Axis().turns[1][from.y][Sign(sign.y) + 1][target.y]);
}

int RaceFirstSlot(const GameState& s, int k) {
    if (TeamOf(k) != s.teamToMove) return 1;
    return s.players[k].moved ? 2 : 0;
}

std::string RaceMomentName(int moment) {
    if (moment >= kRaceNever) return "never";
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, moment % 2 ? "s%d+" : "s%d", moment / 2);
    return buffer;
}

std::vector<BallWindow> BallPathWindows(const GameState& s) {
    BallRaceResult r;
    if (s.phase != Phase::Play || !s.ball.IsFree()) return r.windows;
    BuildWindows(s, &r);
    return r.windows;
}

BallRaceResult SolveBallRace(const GameState& s, const BallRaceOptions& options) {
    BallRaceResult r;
    if (s.phase != Phase::Play || !s.ball.IsFree() || s.keeperRestartTeam >= 0) return r;
    BuildWindows(s, &r);

    Runner runners[kNumPlayers];
    for (int k = 0; k < kNumPlayers; ++k) runners[k] = RunnerOf(s, k);

    int bestLower[kNumTeams] = {kRaceNever, kRaceNever};
    for (int k = 0; k < kNumPlayers; ++k) {
        const Runner& me = runners[k];
        BallRacePlayer& p = r.players[k];
        // In slot 0 the opponents and the team-mates who have already moved stand
        // still: the first move of a runner in slot 0 must get past them.
        std::vector<std::pair<Cell, Cell>> starts;   // after the first turn: cell, signs
        if (me.firstSlot == 0) {
            const auto fixed = [&](Cell c) -> unsigned {
                const int q = PlayerAt(s, c);
                if (q == kNoPlayer || q == k) return 0u;
                return TeamOf(q) != TeamOf(k) || s.players[q].moved ? 1u << q : 0u;
            };
            starts.push_back({me.from, {0, 0}});
            for (int i = -1; i <= 1; ++i)
                for (int j = -1; j <= 1; ++j) {
                    const Cell t{me.from.x + me.sign.x + i, me.from.y + me.sign.y + j};
                    if (!InField(t) || t == me.from || MayBeBlocked(me.from, me.sign, t, fixed)) continue;
                    starts.push_back({t, SignOf({t.x - me.from.x, t.y - me.from.y})});
                }
        }
        const auto turnsTo = [&](Cell c) {
            if (me.firstSlot != 0 || c == me.from) return RaceTurns(me.from, me.sign, c);
            int best = kNoTurns;
            for (const auto& [cell, sign] : starts) best = std::min(best, 1 + RaceTurns(cell, sign, c));
            return best;
        };
        p.lower = EarliestTake(r.windows, me.firstSlot, turnsTo, false, &p.lowerCell);
        bestLower[TeamOf(k)] = std::min(bestLower[TeamOf(k)], p.lower);
    }

    // Guaranteed bounds: per team in the order of the lower bounds, until no
    // further player can improve the team's best (upper >= lower).
    int bestUpper[kNumTeams] = {kRaceNever, kRaceNever};
    int bestUpperPlayer[kNumTeams] = {kNoPlayer, kNoPlayer};
    for (int team = 0; team < kNumTeams; ++team) {
        int order[kPlayersPerTeam];
        for (int i = 0; i < kPlayersPerTeam; ++i) order[i] = FirstPlayerOf(team) + i;
        std::stable_sort(order, order + kPlayersPerTeam,
                         [&](int a, int b) { return r.players[a].lower < r.players[b].lower; });
        for (int k : order) {
            BallRacePlayer& p = r.players[k];
            if (p.lower >= kRaceNever) break;
            int horizon = options.horizonTurns;
            if (!options.allUppers) {
                if (p.lower >= bestUpper[team]) break;
                // Only a take no later than the other team's lower bound decides
                // the race: longer guaranteed routes are not needed.
                const int limit = bestLower[Opponent(team)];
                if (p.lower > limit) break;
                if (limit < kRaceNever)
                    horizon = std::min(horizon, TurnsBy(runners[k].firstSlot, limit / 2));
            }
            const GuaranteedReach g = ComputeGuaranteedReach(s, k, runners, r.windows, horizon);
            p.upper = EarliestTake(r.windows, runners[k].firstSlot,
                                   [&](Cell c) { return int(g.turns[c.x][c.y]); }, true, &p.upperCell);
            p.upperComputed = true;
            if (p.upper < bestUpper[team]) {
                bestUpper[team] = p.upper;
                bestUpperPlayer[team] = k;
            }
        }
    }

    for (int team = 0; team < kNumTeams; ++team)
        if (bestUpper[team] < kRaceNever && bestUpper[team] <= bestLower[Opponent(team)]) {
            const int k = bestUpperPlayer[team];
            r.kind = BallRaceKind::Taken;
            r.team = team;
            r.player = k;
            r.cell = r.players[k].upperCell;
            r.moment = r.players[k].upper;
            return r;
        }
    if (r.endCertain && r.endMoment < std::min(bestLower[kWhite], bestLower[kBlack])) {
        r.kind = BallRaceKind::Restart;
        r.team = r.endTeam;
        r.moment = r.endMoment;
        r.keeper = r.endKeeper;
        return r;
    }
    r.kind = BallRaceKind::Contested;
    int cut = std::min(bestUpper[kWhite], bestUpper[kBlack]);
    if (r.endCertain) cut = std::min(cut, r.endMoment);
    for (int k = 0; k < kNumPlayers; ++k)
        if (r.players[k].lower < kRaceNever && r.players[k].lower <= cut) r.contestants |= 1u << k;
    return r;
}

GameState BallRaceTakenState(const GameState& s, const BallRaceResult& r) {
    const int k = r.player;
    const int slot = r.moment / 2;
    const bool byStep = r.moment % 2 == 1;   // taken with the ball's step at an End Turn
    // Exact: at most one move of the winner in slot 0, then the End Turn.
    if (slot == 0) {
        GameState t = s;
        const PlayerState& p = t.players[k];
        if (p.pos != r.cell) {
            const std::vector<Cell> d = TeamOf(k) == t.teamToMove && !p.moved ? MoveDestinations(t, k)
                                                                              : std::vector<Cell>();
            if (std::find(d.begin(), d.end(), r.cell) != d.end())
                ApplyUnchecked(t, Action::Move(k, r.cell), Outcome::Certain());
        }
        if (t.players[k].pos == r.cell && t.ball.IsFree() && byStep) {
            const std::vector<Outcome> outcomes = OutcomesUnchecked(t, Action::EndTurn());
            Outcome through = outcomes.front();
            for (const Outcome& o : outcomes)
                if (o.kind == OutcomeKind::BallThrough) through = o;
            ApplyUnchecked(t, Action::EndTurn(), through);
        }
        if (t.phase == Phase::Play && t.ball.holder == k) return t;
    }
    // A later take: the players' cells at that moment are unknown. The winner
    // is put on his cell with the ball and the other team moves first, as it
    // will before he gets there; everybody else keeps cell and inertia.
    GameState t = s;
    const int winnerTeam = TeamOf(k);
    if (t.teamToMove == winnerTeam) {   // an End Turn of the winner's team is passed
        for (int q = 0; q < kNumPlayers; ++q) {
            PlayerState& p = t.players[q];
            if (TeamOf(q) == winnerTeam && p.moved) { p.prevTurnStart = p.turnStart; p.turnStart = p.pos; }
            p.moved = false;
            p.attacked = false;
        }
        t.teamToMove = Opponent(winnerTeam);
        t.ballPlayed = false;
        t.committed = false;
        ++t.halfTurns;
        ++t.turnId;
    }
    PlayerState& w = t.players[k];
    w.pos = w.turnStart = w.prevTurnStart = r.cell;
    w.moved = false;
    t.ball.holder = k;
    t.ball.pos = r.cell;
    t.ball.vel = {0, 0};
    t.ball.stepPhase = 0;
    t.ball.attackTeam = winnerTeam;
    t.ball.lastTouchTeam = winnerTeam;
    t.ball.lastTouchWasPass = false;
    return t;
}

namespace {

// Can `o` stand on c after n of his turns (inertia alone)?
bool ReachableIn(const Runner& o, Cell c, int n) {
    return InField(c) && InField(o.from) && Axis().turns[0][o.from.x][o.sign.x + 1][c.x] <= n &&
           Axis().turns[1][o.from.y][o.sign.y + 1][c.y] <= n;
}

// The factor a player at p puts on a pass from a to b passing untouched (as the
// engine's segment: candidates inside the segment's box, not on its end).
double PassFactorAt(Cell a, Cell b, Cell p) {
    if (p == b || p == a) return 1;
    if ((a.x - p.x) * (p.x - b.x) < 0 || (a.y - p.y) * (p.y - b.y) < 0) return 1;
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double dist = std::fabs(dx * (p.y - a.y) - dy * (p.x - a.x)) / std::sqrt(dx * dx + dy * dy);
    return 1 - InterceptionWeight(dist);
}

// The holder of `t` (his team to move, the ball not played) passes into the
// partner's feet: the worst-case chance of no touch (opponents outside
// `ahead.exact` anywhere they reach in `ahead.opponentHeadStart` turns) times
// the partner's breakaway bound after the pass.
double PassIntoFeetBound(const GameState& t, int partner, const BreakawayAhead& ahead, int horizonTurns,
                         double floor) {
    if (t.ballPlayed || t.ball.IsFree() || TeamOf(t.ball.holder) != t.teamToMove) return 0;
    const Cell a = t.players[t.ball.holder].pos, b = t.players[partner].pos;
    if (!InField(b) || !CanPassTo(t, b)) return 0;
    double through = 1;
    for (int q = 0; q < kNumPlayers && through > 0; ++q) {
        if (q == t.ball.holder || q == partner) continue;
        const PlayerState& p = t.players[q];
        if (!InField(p.pos)) continue;
        if (TeamOf(q) == t.teamToMove || (ahead.exact >> q & 1u) || ahead.opponentHeadStart == 0) {
            through *= PassFactorAt(a, b, p.pos);
            continue;
        }
        Runner o = RunnerOf(t, q);
        if (ahead.origin) o.sign = RunnerOf(*ahead.origin, q).sign;
        double worst = 1;
        for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
            for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y)
                if (ReachableIn(o, {x, y}, ahead.opponentHeadStart))
                    worst = std::min(worst, PassFactorAt(a, b, {x, y}));
        through *= worst;
    }
    if (through <= floor) return 0;
    const Action pass = Action::Pass(b);
    const std::vector<Outcome> outcomes = OutcomesUnchecked(t, pass);
    for (const Outcome& o : outcomes) {
        if (o.kind != OutcomeKind::BallThrough && o.kind != OutcomeKind::Certain) continue;
        GameState u = t;
        ApplyUnchecked(u, pass, o);
        if (u.phase != Phase::Play || u.ball.holder != partner) return 0;
        BreakawayAhead next = ahead;
        next.holderMoved = false;
        return through * BreakawayShotLowerBound(u, horizonTurns, floor / through, next);
    }
    return 0;
}

}  // namespace

double TwoVsOneLowerBound(const GameState& s, int partner, int defender, int horizonTurns, double floor) {
    if (s.phase != Phase::Play || s.ball.IsFree() || s.keeperRestartTeam >= 0) return 0;
    const int holder = s.ball.holder;
    const int team = TeamOf(holder);
    if (partner == holder || TeamOf(partner) != team || TeamOf(defender) == team) return 0;
    if (s.teamToMove == team) {
        const double alone = BreakawayShotLowerBound(s, horizonTurns, floor);
        return std::max(alone, PassIntoFeetBound(s, partner, BreakawayAhead(), horizonTurns, std::max(floor, alone)));
    }
    // The defence is to move. Its other players move too: none of them may be
    // able to get next to the holder for a tackle in this turn.
    const Cell h = s.players[holder].pos;
    for (int q = FirstPlayerOf(s.teamToMove); q < FirstPlayerOf(s.teamToMove) + kPlayersPerTeam; ++q) {
        if (q == defender || !InField(s.players[q].pos) || s.players[q].attacked) continue;
        const Runner o = RunnerOf(s, q);
        const int n = s.players[q].moved ? 0 : 1;
        if (std::max(std::abs(o.from.x - h.x), std::abs(o.from.y - h.y)) <= 1) return 0;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if ((dx || dy) && ReachableIn(o, {h.x + dx, h.y + dy}, n)) return 0;
    }
    std::vector<Cell> cells;
    if (!s.players[defender].moved) cells = MoveDestinations(s, defender);
    if (std::find(cells.begin(), cells.end(), s.players[defender].pos) == cells.end())
        cells.push_back(s.players[defender].pos);
    BreakawayAhead ahead;
    ahead.opponentHeadStart = 1;   // the defence's other players have moved in this turn
    ahead.exact = 1u << defender;
    ahead.origin = &s;
    double value = 1;
    for (Cell d : cells) {
        GameState t = s;
        if (d != t.players[defender].pos) ApplyUnchecked(t, Action::Move(defender, d), Outcome::Certain());
        if (!t.players[defender].attacked && GetAttackKind(t, defender) != AttackKind::None) return 0;
        ApplyUnchecked(t, Action::EndTurn(), Outcome::Certain());
        if (t.phase != Phase::Play || t.ball.holder != holder) return 0;
        double v = BreakawayShotLowerBound(t, horizonTurns, floor, ahead);
        if (v < value) v = std::max(v, PassIntoFeetBound(t, partner, ahead, horizonTurns, std::max(floor, v)));
        value = std::min(value, v);
        if (value <= floor) return 0;
    }
    return value;
}

BreakawayAhead BallRaceTakenAhead(const GameState& s, const BallRaceResult& r) {
    BreakawayAhead ahead;
    if (r.kind != BallRaceKind::Taken) return ahead;
    const int slot = r.moment / 2;
    const bool byStep = r.moment % 2 == 1;
    if (slot == 0) return ahead;   // the exact state
    const auto teamOf = [&](int j) { return j % 2 == 0 ? s.teamToMove : Opponent(s.teamToMove); };
    // The winner's first slot with the ball: the take's own slot if he moved
    // onto it, otherwise his first slot after the step.
    int act = slot;
    if (byStep) {
        act = slot + 1;
        while (teamOf(act) != r.team) ++act;
    }
    int opponentSlots = 0;
    for (int j = 0; j < act; ++j)
        if (teamOf(j) != r.team) ++opponentSlots;
    ahead.opponentHeadStart = std::max(0, opponentSlots - 1);   // the taken state gives one
    ahead.holderMoved = !byStep;
    return ahead;
}

std::string BallRaceResult::Describe(const GameState& s) const {
    std::string text;
    char line[160];
    const auto teamName = [](int side) { return side == kWhite ? "White" : "Black"; };
    switch (kind) {
        case BallRaceKind::NotFree: return "not a free ball in play\n";
        case BallRaceKind::Taken:
            std::snprintf(line, sizeof line, "TAKEN by %s: %s on %s at %s (guaranteed)\n", teamName(team),
                          PlayerName(player).c_str(), CellName(cell).c_str(), RaceMomentName(moment).c_str());
            break;
        case BallRaceKind::Restart:
            std::snprintf(line, sizeof line, "RESTART by %s: %s at %s, nobody can reach the ball before\n",
                          teamName(team), keeper ? "keeper" : "out", RaceMomentName(moment).c_str());
            break;
        case BallRaceKind::Contested: {
            std::string names;
            for (int k = 0; k < kNumPlayers; ++k)
                if (contestants >> k & 1) names += " " + PlayerName(k);
            std::snprintf(line, sizeof line, "CONTESTED:%s\n", names.c_str());
            break;
        }
    }
    text += line;
    std::snprintf(line, sizeof line, "slot 0 = %s to move; ball path:", teamName(s.teamToMove));
    text += line;
    for (const BallWindow& w : windows) {
        text += " " + CellName(w.cell);
        if (w.arrival >= 0) text += "@" + RaceMomentName(w.arrival);
        if (w.openEnd) text += "(rests)";
        else if (w.lastMoveSlot >= 0) text += "..s" + std::to_string(w.lastMoveSlot);
        else text += "(stand only)";
    }
    if (endMoment < kRaceNever) {
        std::snprintf(line, sizeof line, "; then %s at %s%s", endKeeper ? "keeper" : "out",
                      RaceMomentName(endMoment).c_str(), endCertain ? "" : " (depends on the mouth)");
        text += line;
    }
    text += "\n";
    for (int k = 0; k < kNumPlayers; ++k) {
        const BallRacePlayer& p = players[k];
        std::snprintf(line, sizeof line, "  %s %-4s lower %-6s %-4s upper %-6s %s\n", PlayerName(k).c_str(),
                      CellName(s.players[k].pos).c_str(), RaceMomentName(p.lower).c_str(),
                      p.lower < kRaceNever ? CellName(p.lowerCell).c_str() : "",
                      p.upperComputed ? RaceMomentName(p.upper).c_str() : "-",
                      p.upperComputed && p.upper < kRaceNever ? CellName(p.upperCell).c_str() : "");
        text += line;
    }
    return text;
}

}  // namespace bot
}  // namespace soccer
