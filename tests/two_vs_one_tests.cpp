// Tests of the 2 v 1 lower bound (TwoVsOneLowerBound, src/bot/ball_race.h).
//
// Random 2 v 1 near the goal: the holder W1, his partner W2 and the defender
// B1; everybody else outside the field. The defence is to move. An exact
// search of a restricted game gives a value the bound may never exceed:
//   - the defender: every move, every attack (a won or knocked-away ball and
//     every later loss count 0), in any order;
//   - the attackers: moves of both, a pass into the partner's feet, a shot
//     (only its goal counts, a rebound counts 0); in the last turn only the
//     holder moves.
// Restricting the attackers lowers the exact value, so the test stays valid
// (it may only find more failures). The bound is called with the same number
// of the holder's turns.
// A second defender B2 anywhere (he moves in the exact search too) tests the
// bound's treatment of the other players.
// Also: the 2 v 1 bound is never below the breakaway bound when the defence is
// to move (the defender seen at his cell can do less than one seen anywhere).
#include "bot/ball_race.h"
#include "bot/hash.h"
#include "engine/notation.h"
#include "engine/rules.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace soccer;
using namespace soccer::bot;

namespace {

int g_checks = 0, g_failures = 0;
long g_nodes = 0;

void Check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        if (g_failures <= 20) std::printf("FAIL: %s\n", what.c_str());
    }
}

constexpr int kH = 0, kP = 1, kD = 4, kD2 = 5;

GameState EmptyPlay(uint64_t seed) {
    GameState s = NewMatch(seed);
    s.phase = Phase::Play;
    s.halfTurns = 10;
    for (int k = 0; k < kNumPlayers; ++k) {
        PlayerState& p = s.players[k];
        p = PlayerState();
        p.pos = p.turnStart = p.prevTurnStart = {-20 - k, -20};
    }
    s.ball = BallState();
    s.ball.holder = kNoPlayer;
    return s;
}

double Defence(const GameState& s, int turns, double alpha);

// The attackers' turn: the end states (and the best shot on the way).
void AttackTurn(const GameState& s, bool last, double* bestShot, std::vector<GameState>* ends,
                std::unordered_map<uint64_t, bool>* seen) {
    if (!seen->emplace(PositionHash(s), true).second) return;
    ++g_nodes;
    for (const Action& a : LegalActions(s)) {
        if (a.type == ActionType::EndTurn) {
            if (!last) ends->push_back(s);
            continue;
        }
        if (a.type == ActionType::Shot) {
            double goal = 0;
            for (const Outcome& o : OutcomesUnchecked(s, a))
                if (o.kind == OutcomeKind::Goal) goal += o.probability;
            *bestShot = std::max(*bestShot, goal);
            continue;
        }
        if (a.type == ActionType::Pass) {
            if (a.target != s.players[kP].pos || s.ball.holder != kH) continue;
            for (const Outcome& o : OutcomesUnchecked(s, a)) {
                if (o.kind != OutcomeKind::BallThrough && o.kind != OutcomeKind::Certain) continue;
                GameState t = s;
                ApplyUnchecked(t, a, o);
                if (t.phase == Phase::Play && t.ball.holder == kP && !last)   // no shot after a pass in this turn
                    AttackTurn(t, last, bestShot, ends, seen);
            }
            continue;
        }
        if (a.type == ActionType::Move && (a.player == kH || a.player == kP) && (!last || a.player == s.ball.holder)) {
            GameState t = s;
            ApplyUnchecked(t, a, Outcome::Certain());
            AttackTurn(t, last, bestShot, ends, seen);
        }
    }
}

// Attackers to move; `turns` attacking turns left (this one included).
double Attack(const GameState& s, int turns, double beta) {
    double best = 0;
    std::vector<GameState> ends;
    std::unordered_map<uint64_t, bool> seen;
    AttackTurn(s, turns == 1, &best, &ends, &seen);
    if (turns == 1 || best >= beta) return best;
    // Most promising first: the holder's own bound.
    std::vector<std::pair<double, int>> order;
    for (int i = 0; i < static_cast<int>(ends.size()); ++i) {
        GameState e = ends[i];
        ApplyUnchecked(e, Action::EndTurn(), Outcome::Certain());
        ends[i] = e;
        order.push_back({-BreakawayShotLowerBound(e, turns - 1), i});
    }
    std::sort(order.begin(), order.end());
    for (const auto& entry : order) {
        const int i = entry.second;
        if (ends[i].phase != Phase::Play || ends[i].ball.IsFree()) continue;
        best = std::max(best, Defence(ends[i], turns - 1, best));
        if (best >= beta) return best;
    }
    return best;
}

// The defender's turn (only B1 acts), then the attackers with `turns` turns.
double DefenceTurn(const GameState& s, int turns, double alpha, double* value,
                   std::unordered_map<uint64_t, bool>* seen) {
    if (!seen->emplace(PositionHash(s), true).second) return *value;
    ++g_nodes;
    for (const Action& a : LegalActions(s)) {
        if (*value <= alpha) return *value;
        if (a.type == ActionType::EndTurn) {
            GameState t = s;
            ApplyUnchecked(t, a, Outcome::Certain());
            if (t.phase == Phase::Play && !t.ball.IsFree() && TeamOf(t.ball.holder) == kWhite)
                *value = std::min(*value, Attack(t, turns, *value));
            else
                *value = 0;
            continue;
        }
        if (a.player != kD && a.player != kD2) continue;
        if (a.type == ActionType::Move) {
            GameState t = s;
            ApplyUnchecked(t, a, Outcome::Certain());
            DefenceTurn(t, turns, alpha, value, seen);
            continue;
        }
        if (a.type == ActionType::Attack) {
            // Expected over the outcomes; only "kept" goes on.
            double kept = 0;
            GameState keptState;
            bool any = false;
            for (const Outcome& o : OutcomesUnchecked(s, a))
                if (o.kind == OutcomeKind::Kept) {
                    kept += o.probability;
                    keptState = s;
                    ApplyUnchecked(keptState, a, o);
                    any = true;
                }
            double v = 0;
            if (any && kept > 0) {
                double inner = 1;
                std::unordered_map<uint64_t, bool> innerSeen;
                DefenceTurn(keptState, turns, -1, &inner, &innerSeen);
                v = kept * inner;
            }
            *value = std::min(*value, v);
        }
    }
    return *value;
}

double Defence(const GameState& s, int turns, double alpha) {
    double value = 1;
    std::unordered_map<uint64_t, bool> seen;
    DefenceTurn(s, turns, alpha, &value, &seen);
    return value;
}

std::string Cells(const GameState& s) { return FormatPosition(s); }

void TestAgainstExact(int cases, int turns, uint64_t seed, bool second, bool attackersMove = false) {
    std::mt19937_64 rng(seed);
    const auto rnd = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    int done = 0, positive = 0, raised = 0;
    double sumBound = 0, sumExact = 0, sumAlone = 0;
    const auto start = std::chrono::steady_clock::now();
    while (done < cases) {
        GameState s = EmptyPlay(seed + done);
        const auto place = [&](int k, int x0, int x1, int y0, int y1) {
            PlayerState& p = s.players[k];
            p.pos = p.turnStart = {rnd(x0, x1), rnd(y0, y1)};
            p.prevTurnStart = {p.pos.x - rnd(-2, 2), p.pos.y - rnd(-2, 2)};
        };
        // White attacks the goal at row 0: the play within a few rows of it.
        place(kH, 1, 9, 3, 9);
        place(kP, std::max(0, s.players[kH].pos.x - 3), std::min(10, s.players[kH].pos.x + 3),
              std::max(1, s.players[kH].pos.y - 3), std::min(19, s.players[kH].pos.y + 2));
        place(kD, std::max(0, s.players[kH].pos.x - 3), std::min(10, s.players[kH].pos.x + 3),
              1, std::min(19, s.players[kH].pos.y + 1));
        if (second) place(kD2, 0, 10, 1, 19);   // anywhere: the bound sees him by his reach
        if (s.players[kP].pos == s.players[kH].pos || s.players[kD].pos == s.players[kH].pos ||
            s.players[kD].pos == s.players[kP].pos)
            continue;
        if (second && (s.players[kD2].pos == s.players[kH].pos || s.players[kD2].pos == s.players[kP].pos ||
                       s.players[kD2].pos == s.players[kD].pos))
            continue;
        if (std::max(std::abs(s.players[kD].pos.x - s.players[kH].pos.x),
                     std::abs(s.players[kD].pos.y - s.players[kH].pos.y)) <= 1)
            continue;   // a tackle now: not a 2 v 1 to prove
        s.ball.holder = kH;
        s.ball.pos = s.players[kH].pos;
        s.ball.attackTeam = kWhite;
        s.teamToMove = attackersMove ? kWhite : kBlack;
        s.keeperRestartTeam = -1;
        const double bound = TwoVsOneLowerBound(s, kP, kD, turns);
        const double alone = BreakawayShotLowerBound(s, turns);
        Check(bound >= alone - 1e-12, "2 v 1 below the breakaway bound: " + Cells(s));
        if (attackersMove) {
            if (bound > 0) {
                const double exact = Attack(s, turns, 2);
                char what[300];
                std::snprintf(what, sizeof what, "attackers to move: bound %.4f > exact %.4f: %s", bound, exact, Cells(s).c_str());
                Check(bound <= exact + 1e-9, what);
                ++positive;
                if (bound > alone + 1e-12) ++raised;
                sumBound += bound; sumExact += exact; sumAlone += alone;
            }
            ++done;
            continue;
        }
        if (bound <= 0) {
            ++done;
            continue;
        }
        const double exact = Defence(s, turns, -1);
        char what[300];
        std::snprintf(what, sizeof what, "bound %.4f > exact %.4f: %s", bound, exact, Cells(s).c_str());
        Check(bound <= exact + 1e-9, what);
        if (bound > exact + 1e-9 && g_failures <= 3) {
            BreakawayAhead ah;
            ah.opponentHeadStart = 1;
            ah.exact = 1u << kD;
            ah.origin = &s;
            for (Cell d : MoveDestinations(s, kD)) {
                GameState t = s;
                if (d != t.players[kD].pos) ApplyUnchecked(t, Action::Move(kD, d), Outcome::Certain());
                const int attack = static_cast<int>(GetAttackKind(t, kD));
                ApplyUnchecked(t, Action::EndTurn(), Outcome::Certain());
                const double vA = BreakawayShotLowerBound(t, turns, 0, ah);
                const double ex = Attack(t, turns, 2);
                std::printf("   D->%s attack %d: vA %.4f, exact %.4f%s\n", CellName(d).c_str(), attack, vA, ex,
                            vA > ex + 1e-9 ? "  <<<" : "");
            }
        }
        ++positive;
        if (bound > alone + 1e-12) ++raised;
        sumBound += bound;
        sumExact += exact;
        sumAlone += alone;
        ++done;
    }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("%s%s%d turns: %d positions, %d with a bound (%d raised above the breakaway bound); "
                "mean breakaway %.3f, 2 v 1 %.3f, exact %.3f; %ld nodes, %.1f s\n",
                attackersMove ? "attackers to move, " : "", second ? "second defender, " : "", turns, done, positive, raised, positive ? sumAlone / positive : 0.0,
                positive ? sumBound / positive : 0.0, positive ? sumExact / positive : 0.0, g_nodes, sec);
}

}  // namespace

int main(int argc, char** argv) {
    const int cases = argc > 1 ? std::atoi(argv[1]) : 200;
    const int turns = argc > 2 ? std::atoi(argv[2]) : 2;
    TestAgainstExact(cases, 1, 11, false);
    TestAgainstExact(cases, turns, 13, false);
    TestAgainstExact(cases, 1, 17, true);
    TestAgainstExact(cases / 4, turns, 19, true);
    TestAgainstExact(cases, 1, 23, true, true);
    TestAgainstExact(cases / 4, turns, 29, true, true);
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "ALL OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
