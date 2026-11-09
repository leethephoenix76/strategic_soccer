#include "bot/race.h"

#include "bot/hash.h"
#include "bot/reach.h"
#include "engine/notation.h"
#include "engine/rules.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace soccer {
namespace bot {

namespace {

// The untouched step of a free ball at End Turn (the outcome where nobody touches it).
Outcome Untouched(const std::vector<Outcome>& outs) {
    for (const Outcome& o : outs)
        if (o.kind == OutcomeKind::BallThrough) return o;
    return outs.front();
}

struct Solver {
    int runner[kNumTeams];
    long states = 0;
    std::unordered_map<uint64_t, double> memo;

    // Moves of the runner of the team to move: his destinations, or none (stay).
    void Moves(const GameState& s, std::vector<Cell>& out) const {
        out.clear();
        const int p = runner[s.teamToMove];
        if (p == kNoPlayer || s.players[p].moved) return;
        std::vector<Cell> d = MoveDestinations(s, p);
        TeamOrder(d, s.teamToMove);
        for (Cell c : d)
            if (c != s.players[p].pos && Validate(s, Action::Move(p, c)) == ActionError::None) out.push_back(c);
    }

    // A ball taken earlier is worth a little more to its team (so that the line of
    // best play takes it as soon as it can, and the other side holds out longest).
    static double Taken(int team, int depth) { return team == kWhite ? 1.0 - 1e-6 * (64 - depth) : 1e-6 * (64 - depth); }

    // After the mover's move (or none): his turn ends.
    double AfterMove(const GameState& t, int depth) {
        if (t.phase != Phase::Play) return 0.5;
        if (!t.ball.IsFree()) return Taken(TeamOf(t.ball.holder), depth);
        double v = 0;
        for (const Outcome& o : OutcomesUnchecked(t, Action::EndTurn())) {
            GameState u = t;
            ApplyUnchecked(u, Action::EndTurn(), o);
            v += o.probability * Value(u, depth - 1);
        }
        return v;
    }

    // Probability that White gets the ball; White maximizes, Black minimizes.
    double Value(const GameState& s, int depth) {
        if (s.phase != Phase::Play) return 0.5;
        if (!s.ball.IsFree()) return Taken(TeamOf(s.ball.holder), depth);
        if (depth <= 0) return 0.5;
        const uint64_t key = PositionHash(s) * 0x9e3779b97f4a7c15ULL + static_cast<uint64_t>(depth);
        auto it = memo.find(key);
        if (it != memo.end()) return it->second;
        ++states;
        const bool white = s.teamToMove == kWhite;
        const int p = runner[s.teamToMove];
        double best = AfterMove(s, depth);   // stay
        std::vector<Cell> moves;
        Moves(s, moves);
        for (Cell c : moves) {
            if (white ? best >= 1 - 1e-6 * (64 - depth) : best <= 1e-6 * (64 - depth)) break;   // the best possible now
            GameState t = s;
            ApplyUnchecked(t, Action::Move(p, c), Outcome::Certain());
            const double v = AfterMove(t, depth);
            best = white ? std::max(best, v) : std::min(best, v);
        }
        memo.emplace(key, best);
        return best;
    }
};

// The same game with alpha-beta: the decision nodes with a window, the chance
// nodes (the ball's step) with Star1 (values within [0, 1]), a table of bounds,
// the moves towards the ball first. The value is exactly that of Solver.
struct FastSolver {
    int runner[kNumTeams];
    long states = 0;
    struct Bounds {
        double lo = 0, hi = 1;
    };
    std::unordered_map<uint64_t, Bounds> table;
    std::vector<Cell> path;   // the ball's cells at the start of each half-turn from the root, untouched
    int rootHalfTurns = 0;

    static double Taken(int team, int depth) { return Solver::Taken(team, depth); }

    // Distance of a cell to the ball's untouched path from half-turn h on (for the order).
    int PathDistance(Cell c, int h) const {
        int best = 99;
        for (int i = std::max(0, h); i < static_cast<int>(path.size()) && i <= h + 6; ++i)
            best = std::min(best, std::max(0, ChebyshevDistance(c, path[i]) - (i - h) / 2));
        return best;
    }

    double AfterMove(const GameState& t, int depth, double alpha, double beta, double mass = 1) {
        if (t.phase != Phase::Play) return 0.5;
        if (!t.ball.IsFree()) return Taken(TeamOf(t.ball.holder), depth);
        std::vector<Outcome> outs = OutcomesUnchecked(t, Action::EndTurn());
        if (outs.size() == 1) {
            GameState u = t;
            ApplyUnchecked(u, Action::EndTurn(), outs.front());
            return Value(u, depth - 1, alpha, beta, mass);
        }
        // Star1: the outcomes not searched yet are within [0, 1].
        std::sort(outs.begin(), outs.end(), [](const Outcome& a, const Outcome& b) { return a.probability > b.probability; });
        double sum = 0, rest = 1;
        for (const Outcome& o : outs) {
            rest -= o.probability;
            if (rest < 0) rest = 0;
            const double a = (alpha - sum - rest) / o.probability, b = (beta - sum) / o.probability;
            GameState u = t;
            ApplyUnchecked(u, Action::EndTurn(), o);
            const double v = Value(u, depth - 1, std::max(0.0, a), std::min(1.0, b), mass * o.probability);
            sum += o.probability * v;
            if (sum >= beta) return sum;          // a lower bound, at least beta whatever the rest
            if (sum + rest <= alpha) return sum + rest;
        }
        return sum;
    }

    // The ball's untouched path from a state: its cell at the start of each
    // half-turn and the length of the step it makes at that half-turn's end.
    struct BallPath {
        std::vector<Cell> cell;
        std::vector<int> step;
    };
    std::unordered_map<uint64_t, BallPath> paths;

    const BallPath& PathOf(const GameState& s) {
        const BallState& b = s.ball;
        const uint64_t key = (uint64_t(b.pos.x) << 40) | (uint64_t(b.pos.y) << 32) | (uint64_t(b.vel.x + 16) << 24) |
                             (uint64_t(b.vel.y + 16) << 16) | (uint64_t(b.dec.x + 2) << 12) | (uint64_t(b.dec.y + 2) << 8) |
                             (uint64_t(b.stepPhase) << 4) | uint64_t(s.teamToMove);
        auto it = paths.find(key);
        if (it != paths.end()) return it->second;
        BallPath& bp = paths[key];
        GameState t = s;
        for (int h = 0; h <= 16 && t.phase == Phase::Play && t.ball.IsFree(); ++h) {
            bp.cell.push_back(t.ball.pos);
            const Cell before = t.ball.pos;
            ApplyUnchecked(t, Action::EndTurn(), Untouched(OutcomesUnchecked(t, Action::EndTurn())));
            bp.step.push_back(ChebyshevDistance(before, t.ball.pos));
        }
        return bp;
    }

    // Own turns of `team`'s runner from now to the end of half-turn h (h = 0: this one).
    static int MovesBy(const GameState& s, int team, int p, int h) {
        if (team == s.teamToMove) return h / 2 + (s.players[p].moved ? 0 : 1);
        return (h + 1) / 2;
    }

    // The runner p can not come near the ball within `depth` half-turns (every
    // cell he can be in stays more than 2 cells from the ball's path, so he can
    // neither take it nor touch it on its way, up to chances below 1e-3).
    bool Out(const GameState& s, int p, int depth) {
        if (p == kNoPlayer) return true;
        const BallPath& bp = PathOf(s);
        const int team = TeamOf(p);
        const Cell pos = s.players[p].pos;
        for (int h = 0; h < depth && h < static_cast<int>(bp.cell.size()); ++h)
            if (ChebyshevDistance(pos, bp.cell[h]) - 2 * MovesBy(s, team, p, h) - bp.step[h] <= 2) return false;
        return true;
    }

    // A cheap guess for an unlikely branch: who could be at the ball's path first
    // (at most 2 cells a turn, the others ignored); a tie or nobody: 1/2.
    double Guess(const GameState& s, int depth) {
        const BallPath& bp = PathOf(s);
        int first[kNumTeams] = {99, 99};
        for (int team = 0; team < kNumTeams; ++team) {
            const int p = runner[team];
            if (p == kNoPlayer) continue;
            for (int h = 0; h < depth && h < static_cast<int>(bp.cell.size()); ++h)
                if (ChebyshevDistance(s.players[p].pos, bp.cell[h]) <= 2 * MovesBy(s, team, p, h)) {
                    first[team] = h;
                    break;
                }
        }
        if (first[kWhite] < first[kBlack]) return Taken(kWhite, depth - first[kWhite]);
        if (first[kBlack] < first[kWhite]) return Taken(kBlack, depth - first[kBlack]);
        return 0.5;
    }

    double epsilon = 0;   // branches less likely than this are guessed
    long maxStates = 0;   // beyond this, the rest is guessed

    double Value(const GameState& s, int depth, double alpha, double beta, double mass = 1) {
        if (s.phase != Phase::Play) return 0.5;
        if (!s.ball.IsFree()) return Taken(TeamOf(s.ball.holder), depth);
        if (depth <= 0) return 0.5;
        if (mass < epsilon || (maxStates > 0 && states >= maxStates)) return Guess(s, depth);
        const uint64_t key = PositionHash(s) * 0x9e3779b97f4a7c15ULL + static_cast<uint64_t>(depth);
        Bounds& known = table[key];
        if (known.lo >= beta) return known.lo;
        if (known.hi <= alpha) return known.hi;
        if (known.lo == known.hi) return known.lo;
        alpha = std::max(alpha, known.lo);
        beta = std::min(beta, known.hi);
        // Nothing beats taking the ball now: the window ends there.
        const double top = Taken(kWhite, depth), bottom = Taken(kBlack, depth);
        if (alpha >= top) return top;
        if (beta <= bottom) return bottom;
        const double aw = alpha, bw = beta;
        alpha = std::max(alpha, bottom - 1e-12);
        beta = std::min(beta, top + 1e-12);
        ++states;
        const bool white = s.teamToMove == kWhite;
        const int p = runner[s.teamToMove];
        const int q = runner[Opponent(s.teamToMove)];
        // A runner who can not get near the ball, when the other one can not either
        // or he can not get near him, only stands (his moves change nothing).
        const bool outP = Out(s, p, depth), outQ = Out(s, q, depth);
        // (The others still stand where they are and may take the ball on its way.)
        bool still = outP && outQ;
        if (!still && outP && p != kNoPlayer && q != kNoPlayer) {
            const int turns = MovesBy(s, TeamOf(p), p, depth - 1) + MovesBy(s, TeamOf(q), q, depth - 1);
            still = ChebyshevDistance(s.players[p].pos, s.players[q].pos) > 2 * turns + 2;
        }
        // The candidates: stay, then the moves, nearest to the ball's path first.
        std::vector<Cell> moves;
        if (p != kNoPlayer && !s.players[p].moved && !still) {
            std::vector<Cell> d = MoveDestinations(s, p);
            TeamOrder(d, s.teamToMove);
            for (Cell c : d)   // (the destinations are valid moves: ValidateMove)
                if (c != s.players[p].pos && InField(c)) moves.push_back(c);
            const int h = s.halfTurns - rootHalfTurns + 1;
            // (keys once per move; the ball's cell first)
            int order[16];
            const int n = static_cast<int>(std::min<std::size_t>(moves.size(), 16));
            for (int i = 0; i < n; ++i) order[i] = moves[i] == s.ball.pos ? -1 : PathDistance(moves[i], h);
            for (int i = 1; i < n; ++i)   // insertion sort, stable
                for (int j = i; j > 0 && order[j] < order[j - 1]; --j) {
                    std::swap(order[j], order[j - 1]);
                    std::swap(moves[j], moves[j - 1]);
                }
        }
        const double a0 = aw, b0 = bw;
        double best = white ? -1 : 2;
        auto consider = [&](double v) {
            if (white) {
                best = std::max(best, v);
                alpha = std::max(alpha, v);
            } else {
                best = std::min(best, v);
                beta = std::min(beta, v);
            }
            return alpha >= beta;
        };
        bool cut = false;
        for (Cell c : moves) {
            GameState t = s;
            ApplyUnchecked(t, Action::Move(p, c), Outcome::Certain());
            if ((cut = consider(AfterMove(t, depth, alpha, beta, mass)))) break;
        }
        if (!cut) consider(AfterMove(s, depth, alpha, beta, mass));   // stay
        // Store: a fail low (best <= a0) is an upper bound, a fail high a lower bound.
        Bounds& b = table[key];
        if (best <= a0) b.hi = std::min(b.hi, best);
        else if (best >= b0) b.lo = std::max(b.lo, best);
        else b.lo = b.hi = best;
        return best;
    }
};

}  // namespace

std::string RaceResult::Describe() const {
    if (!valid) return "no race";
    char buf[200];
    std::string w = runner[kWhite] == kNoPlayer ? "-" : PlayerName(runner[kWhite]);
    std::string b = runner[kBlack] == kNoPlayer ? "-" : PlayerName(runner[kBlack]);
    std::snprintf(buf, sizeof buf, "race %s(>=+%d) - %s(>=+%d): P(White) %.3f", w.c_str(), bound[kWhite], b.c_str(),
                  bound[kBlack], pWhite);
    std::string out = buf;
    if (winner >= 0) {
        std::snprintf(buf, sizeof buf, ", %s takes it in %s at +%d", winner == kWhite ? "White" : "Black",
                      CellName(pickup).c_str(), pickupHalfTurn);
        out += buf;
    }
    return out;
}

RaceResult Race(const GameState& s, const RaceOptions& options) {
    RaceResult r;
    if (s.phase != Phase::Play || !s.ball.IsFree()) return r;
    r.valid = true;
    const int H = std::max(1, options.horizon);

    // The ball's cells at the start of each half-turn, untouched.
    std::vector<Cell> ball;
    {
        GameState t = s;
        for (int h = 0; h <= H && t.phase == Phase::Play && t.ball.IsFree(); ++h) {
            ball.push_back(t.ball.pos);
            ApplyUnchecked(t, Action::EndTurn(), Untouched(OutcomesUnchecked(t, Action::EndTurn())));
        }
    }
    // The earliest half-turn each player could stand on the ball's cell (reach tables).
    for (int k = 0; k < kNumPlayers; ++k) {
        const int team = TeamOf(k);
        const ReachMap reach = PlayerReach(s, k);
        const bool mover = team == s.teamToMove;
        int earliest = -1;
        for (int h = mover ? 0 : 1; h < static_cast<int>(ball.size()); h += 2) {
            // Own moves made by the end of this half-turn (this one included).
            int moves = mover ? h / 2 + 1 : (h + 1) / 2;
            if (mover && s.players[k].moved) --moves;   // he has moved in the current turn already
            if (moves <= 0) continue;
            if (reach.Turns(ball[h]) <= moves) {
                earliest = h;
                break;
            }
        }
        if (earliest < 0) continue;
        if (r.bound[team] < 0 || earliest < r.bound[team]) {
            r.bound[team] = earliest;
            r.runner[team] = k;
        }
    }

    if (options.clearMargin > 0) {
        const int w = r.bound[kWhite], b = r.bound[kBlack];
        int clear = -1;
        if (w >= 0 && (b < 0 || b - w >= options.clearMargin)) clear = kWhite;
        if (b >= 0 && (w < 0 || w - b >= options.clearMargin)) clear = kBlack;
        if (clear >= 0) {
            r.pWhite = clear == kWhite ? 1 : 0;
            r.path = ball;
            return r;
        }
    }

    FastSolver fast;
    Solver solver;
    fast.runner[kWhite] = solver.runner[kWhite] = r.runner[kWhite];
    fast.runner[kBlack] = solver.runner[kBlack] = r.runner[kBlack];
    fast.path = ball;
    fast.rootHalfTurns = s.halfTurns;
    fast.epsilon = options.epsilon;
    fast.maxStates = options.maxStates;
    // The value of a state after the mover's move (exact: a full window).
    auto after = [&](const GameState& t, int depth) {
        return options.reference ? solver.AfterMove(t, depth) : fast.AfterMove(t, depth, -1, 2);
    };
    const double v0 = options.reference ? solver.Value(s, H) : fast.Value(s, H, -1, 2);
    r.pWhite = std::round(v0 * 1000) / 1000;   // (without the small bonus for an early pick-up)
    r.path = ball;
    if (!options.line) {
        r.states = options.reference ? solver.states : fast.states;
        return r;
    }

    // The line of best play: the best move (the earliest in the order among equal
    // ones), the untouched step of the ball.
    r.firstMove[kWhite] = r.runner[kWhite] == kNoPlayer ? Cell{} : s.players[r.runner[kWhite]].pos;
    r.firstMove[kBlack] = r.runner[kBlack] == kNoPlayer ? Cell{} : s.players[r.runner[kBlack]].pos;
    bool seen[kNumTeams] = {false, false};
    GameState t = s;
    for (int h = 0; h < H && t.phase == Phase::Play && t.ball.IsFree(); ++h) {
        const bool white = t.teamToMove == kWhite;
        const int p = solver.runner[t.teamToMove];
        double best = after(t, H - h);
        Cell bestCell = p == kNoPlayer ? Cell{} : t.players[p].pos;
        std::vector<Cell> moves;
        solver.Moves(t, moves);
        for (Cell c : moves) {
            GameState u = t;
            ApplyUnchecked(u, Action::Move(p, c), Outcome::Certain());
            const double v = after(u, H - h);
            if (white ? v > best + 1e-12 : v < best - 1e-12) {
                best = v;
                bestCell = c;
            }
        }
        if (!seen[t.teamToMove] && p != kNoPlayer) {
            r.firstMove[t.teamToMove] = bestCell;
            seen[t.teamToMove] = true;
        }
        if (p != kNoPlayer && bestCell != t.players[p].pos) ApplyUnchecked(t, Action::Move(p, bestCell), Outcome::Certain());
        if (!t.ball.IsFree()) {
            r.winner = TeamOf(t.ball.holder);
            r.pickup = t.ball.pos;
            r.pickupHalfTurn = h;
            r.taken = t;
            break;
        }
        ApplyUnchecked(t, Action::EndTurn(), Untouched(OutcomesUnchecked(t, Action::EndTurn())));
        if (!t.ball.IsFree() && t.phase == Phase::Play) {
            r.winner = TeamOf(t.ball.holder);
            r.pickup = t.ball.pos;
            r.pickupHalfTurn = h + 1;
            r.taken = t;
            break;
        }
    }
    r.states = options.reference ? solver.states : fast.states;
    return r;
}

}  // namespace bot
}  // namespace soccer
