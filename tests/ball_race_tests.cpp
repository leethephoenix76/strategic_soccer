// Tests of the free-ball race solver (src/bot/ball_race.h).
//
// 1. Inertia: the per-axis bound equals the engine's breadth-first reach for a
//    lone player, for every start, velocity sign and target.
// 2. Certificates against an exact search: random races of one or two runners
//    per team near a free ball (the other players far outside), every legal
//    order of moves of the runners, the ball untouched on its steps. A Taken
//    result must be forced by that team, a Restart must happen whatever both
//    teams do. Contested results are counted to measure how often the bounds
//    leave a decided race to the search.
#include "bot/ball_race.h"
#include "bot/reach_front.h"
#include "bot/run_border.h"
#include "engine/notation.h"
#include "engine/rules.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace soccer;
using namespace soccer::bot;

namespace {

int g_checks = 0, g_failures = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        if (g_failures <= 20) std::printf("FAIL: %s\n", what.c_str());
    }
}

const Cell kAway{-20, -20};   // outside the field: never blocks, never reaches

GameState EmptyPlay(uint64_t seed) {
    GameState s = NewMatch(seed);
    s.phase = Phase::Play;
    s.halfTurns = 10;
    for (int k = 0; k < kNumPlayers; ++k) {
        PlayerState& p = s.players[k];
        p = PlayerState();
        p.pos = p.turnStart = p.prevTurnStart = {kAway.x - k, kAway.y};
    }
    s.ball = BallState();
    s.ball.holder = kNoPlayer;
    return s;
}

// ---------------------------------------------------------------- 1. inertia

void TestInertia() {
    for (int x0 = 0; x0 < kFieldWidth; ++x0)
        for (int y0 = kFirstRow; y0 <= kLastRow; ++y0)
            for (int sx = -1; sx <= 1; ++sx)
                for (int sy = -1; sy <= 1; ++sy) {
                    // Engine breadth-first search over (cell, sign).
                    int best[kFieldWidth][kGridHeight];
                    for (auto& row : best)
                        for (int& v : row) v = 255;
                    bool seen[kFieldWidth][kGridHeight][3][3] = {};
                    std::vector<std::pair<Cell, Cell>> layer{{{x0, y0}, {sx, sy}}}, next;
                    seen[x0][y0][sx + 1][sy + 1] = true;
                    best[x0][y0] = 0;
                    GameState s = EmptyPlay(1);
                    for (int t = 1; !layer.empty(); ++t) {
                        next.clear();
                        for (auto [c, v] : layer) {
                            PlayerState& p = s.players[0];
                            p.pos = p.turnStart = c;
                            p.prevTurnStart = {c.x - v.x, c.y - v.y};
                            p.moved = false;
                            for (Cell d : MoveDestinations(s, 0)) {
                                const Cell nv{Sign(d.x - c.x), Sign(d.y - c.y)};
                                if (seen[d.x][d.y][nv.x + 1][nv.y + 1]) continue;
                                seen[d.x][d.y][nv.x + 1][nv.y + 1] = true;
                                next.push_back({d, nv});
                                if (best[d.x][d.y] == 255) best[d.x][d.y] = t;
                            }
                        }
                        layer.swap(next);
                    }
                    for (int x = 0; x < kFieldWidth; ++x)
                        for (int y = kFirstRow; y <= kLastRow; ++y)
                            Check(RaceTurns({x0, y0}, {sx, sy}, {x, y}) == best[x][y],
                                  "inertia " + CellName({x0, y0}) + " sign " + std::to_string(sx) + "," +
                                      std::to_string(sy) + " to " + CellName({x, y}));
                }
}

// ---------------------------------------------------------------- 2. exact race

enum Goal { kGoalWhite = 0, kGoalBlack = 1, kGoalRestart = 2 };

struct Exact {
    unsigned movers = 0;
    long nodes = 0;
    std::unordered_map<std::string, bool> memo;

    std::string Key(const GameState& s) const {
        std::string k;
        k.reserve(96);
        for (int i = 0; i < kNumPlayers; ++i) {
            if (!(movers >> i & 1)) continue;
            const PlayerState& p = s.players[i];
            const int v[] = {p.pos.x, p.pos.y, p.turnStart.x - p.prevTurnStart.x, p.turnStart.y - p.prevTurnStart.y,
                             p.pos.x - p.turnStart.x, p.pos.y - p.turnStart.y, p.moved};
            for (int x : v) k.push_back(static_cast<char>(x + 64));
        }
        const int b[] = {s.ball.pos.x, s.ball.pos.y, s.ball.vel.x, s.ball.vel.y, s.ball.stepPhase, s.teamToMove};
        for (int x : b) k.push_back(static_cast<char>(x + 64));
        return k;
    }

    // 1: reaches the goal, 0: does not, -1: undecided (not terminal yet).
    static int Terminal(const GameState& s, Goal goal) {
        if (s.phase != Phase::Play) return goal == kGoalRestart;
        if (!s.ball.IsFree()) return goal == TeamOf(s.ball.holder);
        return -1;
    }

    // The states a team can end its movement in (or a terminal state reached
    // by taking the ball), every order of its runners' moves.
    void Options(const GameState& s, std::set<std::string>& seen, std::vector<GameState>& out) {
        if (!seen.insert(Key(s)).second) return;
        out.push_back(s);
        if (!s.ball.IsFree()) return;
        for (int k = FirstPlayerOf(s.teamToMove); k < FirstPlayerOf(s.teamToMove) + kPlayersPerTeam; ++k) {
            if (!(movers >> k & 1) || s.players[k].moved) continue;
            for (Cell d : MoveDestinations(s, k)) {
                if (d == s.players[k].pos) continue;
                GameState t = s;
                ApplyUnchecked(t, Action::Move(k, d), Outcome());
                Options(t, seen, out);
            }
        }
    }

    // Can the goal be forced within `slots` turns? White and Black goals: the
    // team of the goal chooses, the other resists. Restart: it must happen
    // whatever both teams do.
    bool Can(const GameState& s, Goal goal, int slots) {
        const int t = Terminal(s, goal);
        if (t >= 0) return t == 1;
        if (slots <= 0) return false;
        const std::string key = Key(s) + char(goal) + char(slots);
        auto it = memo.find(key);
        if (it != memo.end()) return it->second;
        ++nodes;
        std::set<std::string> seen;
        std::vector<GameState> options;
        Options(s, seen, options);
        const bool chooser = goal != kGoalRestart && s.teamToMove == static_cast<int>(goal);
        bool result = !chooser;
        for (const GameState& o : options) {
            bool ok;
            const int ot = Terminal(o, goal);
            if (ot >= 0) {
                ok = ot == 1;
            } else {
                GameState e = o;
                const std::vector<Outcome> outcomes = OutcomesUnchecked(e, Action::EndTurn());
                Outcome through = outcomes.front();
                for (const Outcome& x : outcomes)
                    if (x.kind == OutcomeKind::BallThrough) through = x;
                ApplyUnchecked(e, Action::EndTurn(), through);
                ok = Can(e, goal, slots - 1);
            }
            if (chooser && ok) { result = true; break; }
            if (!chooser && !ok) { result = false; break; }
        }
        memo[key] = result;
        return result;
    }
};

struct Tally {
    int taken = 0, restart = 0, contested = 0;
    int contestedDecided = 0;   // contested by the bounds, but one side can force it within the depth
    int takenBeyond = 0, restartBeyond = 0;   // certificates beyond the exact depth (not checked)
};

void TestAgainstExact(int cases, int maxRunners, int slots, uint64_t seed, Tally& tally, int spread = 4) {
    std::mt19937_64 rng(seed);
    auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    for (int n = 0; n < cases; ++n) {
        GameState s = EmptyPlay(seed + n);
        s.teamToMove = rnd(0, 1);
        s.ball.pos = {rnd(0, 10), rnd(2, 18)};
        s.ball.vel = rnd(0, 3) == 0 ? Cell{0, 0} : Cell{rnd(-3, 3), rnd(-3, 3)};
        s.ball.dec = {-Sign(s.ball.vel.x), -Sign(s.ball.vel.y)};
        s.ball.stepPhase = rnd(0, 1);
        s.ball.lastTouchTeam = rnd(0, 1);
        s.ball.lastTouchWasPass = rnd(0, 1);

        int counts[2] = {1, 1};
        if (maxRunners == 3) counts[rnd(0, 1)] = 2;
        if (maxRunners >= 4) counts[0] = counts[1] = 2;
        unsigned movers = 0;
        bool ok = true;
        for (int team = 0; team < kNumTeams && ok; ++team)
            for (int i = 0; i < counts[team]; ++i) {
                const int k = FirstPlayerOf(team) + i;
                Cell c;
                int tries = 0;
                do {
                    c = {s.ball.pos.x + rnd(-spread, spread), s.ball.pos.y + rnd(-spread, spread)};
                } while ((!InField(c) || c == s.ball.pos || PlayerAt(s, c) != kNoPlayer) && ++tries < 100);
                if (tries >= 100) { ok = false; break; }
                PlayerState& p = s.players[k];
                p.pos = p.turnStart = c;
                const Cell v{rnd(-2, 2), rnd(-2, 2)};
                p.prevTurnStart = {c.x - v.x, c.y - v.y};
                if (team == s.teamToMove && rnd(0, 3) == 0) {   // has already moved in this turn
                    p.moved = true;
                    p.turnStart = {std::max(0, std::min(10, c.x - rnd(-1, 1))),
                                   std::max(1, std::min(19, c.y - rnd(-1, 1)))};
                    p.prevTurnStart = p.turnStart;
                }
                movers |= 1u << k;
            }
        if (!ok) continue;

        const BallRaceResult r = SolveBallRace(s);
        Exact exact;
        exact.movers = movers;
        const int horizon = 2 * slots - 1;   // the last moment the exact search sees
        const std::string where = FormatPosition(s) + "\n" + r.Describe(s);
        if (r.kind == BallRaceKind::Taken) {
            ++tally.taken;
            if (r.moment > horizon) { ++tally.takenBeyond; continue; }
            Check(exact.Can(s, static_cast<Goal>(r.team), slots), "taken not forced: " + where);
        } else if (r.kind == BallRaceKind::Restart) {
            ++tally.restart;
            if (r.moment > horizon) { ++tally.restartBeyond; continue; }
            Check(exact.Can(s, kGoalRestart, slots), "restart not forced: " + where);
        } else if (r.kind == BallRaceKind::Contested) {
            ++tally.contested;
            if (exact.Can(s, kGoalWhite, slots) || exact.Can(s, kGoalBlack, slots) ||
                exact.Can(s, kGoalRestart, slots))
                ++tally.contestedDecided;
        }
    }
}

// A few hand-made positions with a known answer.
void TestKnown() {
    // A runner standing on the ball's next cell takes it with the step.
    {
        GameState s = EmptyPlay(3);
        s.teamToMove = kWhite;
        s.ball.pos = {5, 10};
        s.ball.vel = {0, -2};
        s.ball.stepPhase = 1;
        s.players[4].pos = s.players[4].turnStart = s.players[4].prevTurnStart = {5, 8};   // B1 on f12
        s.players[0].pos = s.players[0].turnStart = s.players[0].prevTurnStart = {9, 14};  // W1 far
        const BallRaceResult r = SolveBallRace(s);
        Check(r.kind == BallRaceKind::Taken && r.team == kBlack && r.player == 4 && r.moment == 3,
              "standing runner: " + r.Describe(s));
    }
    // Nobody near a ball rolling out: restart for the other team.
    {
        GameState s = EmptyPlay(4);
        s.teamToMove = kBlack;
        s.ball.pos = {1, 10};
        s.ball.vel = {-2, 0};
        s.ball.stepPhase = 0;
        s.ball.lastTouchTeam = kBlack;
        s.players[0].pos = s.players[0].turnStart = s.players[0].prevTurnStart = {9, 14};
        s.players[4].pos = s.players[4].turnStart = s.players[4].prevTurnStart = {9, 4};
        const BallRaceResult r = SolveBallRace(s);
        Check(r.kind == BallRaceKind::Restart && r.team == kWhite && !r.keeper, "out: " + r.Describe(s));
    }
    // A free ball reaching an empty goal mouth: the keeper's restart.
    {
        GameState s = EmptyPlay(5);
        s.teamToMove = kWhite;
        s.ball.pos = {5, 3};
        s.ball.vel = {0, -2};
        s.ball.stepPhase = 1;
        s.ball.lastTouchTeam = kWhite;
        s.players[0].pos = s.players[0].turnStart = s.players[0].prevTurnStart = {9, 14};
        s.players[4].pos = s.players[4].turnStart = s.players[4].prevTurnStart = {9, 4};
        const BallRaceResult r = SolveBallRace(s);
        Check(r.kind == BallRaceKind::Restart && r.keeper && r.team == kBlack, "keeper: " + r.Describe(s));
    }
}

// The reach map inside a turn (07.11.2026): a player who has moved stands in
// the first half-turn and then carries his displacement; one who has not
// moves at once. The pass windows equal the race's.
void TestReachInsideTurn() {
    GameState s = EmptyPlay(6);
    s.teamToMove = kWhite;
    PlayerState& moved = s.players[0];
    moved.prevTurnStart = {5, 12};
    moved.turnStart = {5, 11};
    moved.pos = {5, 10};
    moved.moved = true;
    PlayerState& fresh = s.players[1];
    fresh.pos = fresh.turnStart = fresh.prevTurnStart = {2, 10};
    PlayerState& other = s.players[4];
    other.pos = other.turnStart = other.prevTurnStart = {9, 4};
    // The map needs every player on the field: the rest stand far away.
    const Cell rest[] = {{0, 19}, {1, 19}, {8, 1}, {9, 1}, {10, 1}};
    for (int k : {2, 3, 5, 6, 7}) {
        PlayerState& p = s.players[k];
        p.pos = p.turnStart = p.prevTurnStart = rest[k < 4 ? k - 2 : k - 3];
    }
    s.ball.holder = 1;
    s.ball.pos = fresh.pos;
    const ReachFront f = ComputeReachFront(s, 4);
    Check(f.stop != ReachFrontStop::InvalidStart && f.frames.size() == 5, "inside a turn: the map is built");
    if (f.frames.size() != 5) return;
    int movedCells1 = 0, freshCells1 = 0, movedCells3 = 0;
    for (int i = 0; i < kReachFrontCells; ++i) {
        movedCells1 += (f.frames[1].players[i] & 1u) != 0;
        freshCells1 += (f.frames[1].players[i] & 2u) != 0;
        movedCells3 += (f.frames[3].players[i] & 1u) != 0;
    }
    Check(movedCells1 == 1, "a moved player stands in half-turn 1");
    Check(freshCells1 > 1, "a player who has not moved moves in half-turn 1");
    Check(movedCells3 > 1, "a moved player moves again in half-turn 3");
    // His inertia is the displacement of this turn: one row towards y = 1.
    const int ahead = ReachFrontIndex({5, 8}), behind = ReachFrontIndex({5, 12});
    Check((f.frames[3].players[ahead] & 1u) && !(f.frames[3].players[behind] & 1u),
          "a moved player's inertia is his displacement");
    // A free ball's windows alone equal the race's.
    GameState t = s;
    t.ball.holder = kNoPlayer;
    t.ball.pos = {5, 6};
    t.ball.vel = {0, -3};
    t.ball.stepPhase = 0;
    const std::vector<BallWindow> w = BallPathWindows(t);
    const BallRaceResult r = SolveBallRace(t);
    bool same = w.size() == r.windows.size();
    for (std::size_t i = 0; same && i < w.size(); ++i)
        same = w[i].cell == r.windows[i].cell && w[i].arrival == r.windows[i].arrival &&
               w[i].lastMoveSlot == r.windows[i].lastMoveSlot && w[i].openEnd == r.windows[i].openEnd;
    Check(same, "pass windows equal the race's");
}

// The border of a run (run_border.h) is FIXED by the user's decision of
// 08.11.2026: g8_47b after 2g9 - B2 against W4 runs g7-g5-g3-g1 and W4
// follows f8-f6-f4-f2, the border between files f and g; against W3 B2
// escapes along e7, c5, a3 to a1 (session log 06.11.2026). A change of the
// procedure must not change these lines.
void TestRunBorder47b() {
    GameState s;
    const bool parsed = ParsePosition("w 47 0-0 e11 e10/d10 i8 d9/c9 j10 g9/h11 e12 c13 @B2 t:B", &s);
    Check(parsed, "run border: 47b position");
    if (!parsed) return;
    auto line = [](const RunBorderResult& r) {
        std::string out;
        for (std::size_t i = 0; i < r.cells.size(); ++i) out += (i ? " " : "") + CellName(r.cells[i]);
        return out;
    };
    const RunBorderResult w4 = SolveRunBorder(s, 5, 3);
    Check(w4.valid && w4.rowsLeft == 0 && w4.fileOffset == 1, "run border: B2 against W4 reaches the last row on g");
    Check(line(w4) == "f8 g7 f6 g5 f4 g3 f2 g1", "run border: B2 against W4 line f/g, got " + line(w4));
    const RunBorderResult w3 = SolveRunBorder(s, 5, 2);
    Check(line(w3) == "h7 e7 f5 c5 d3 a3 b3 a1", "run border: B2 against W3 escapes to a1, got " + line(w3));
}

}  // namespace

int main(int argc, char** argv) {
    const int cases = argc > 1 ? std::atoi(argv[1]) : 400;
    TestInertia();
    TestKnown();
    TestReachInsideTurn();
    TestRunBorder47b();
    Tally one, two, crowd;
    TestAgainstExact(cases, 2, 6, 1000, one);
    TestAgainstExact(cases / 4, 3, 5, 5000, two);
    TestAgainstExact(cases / 8, 4, 4, 9000, crowd, 2);
    for (const auto& [name, t] : {std::pair<const char*, Tally&>{"1 v 1", one}, {"2 v 1", two},
                                  {"2 v 2 crowded", crowd}})
        std::printf("%s: taken %d (%d beyond the exact depth), restart %d (%d beyond), contested %d "
                    "(%d of them decided within the depth)\n",
                    name, t.taken, t.takenBeyond, t.restart, t.restartBeyond, t.contested, t.contestedDecided);
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "ALL OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
