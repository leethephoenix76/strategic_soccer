#include "bot/episode_value.h"

#include "bot/ball_race.h"
#include "bot/goal_episode.h"
#include "bot/hash.h"
#include "bot/reach.h"
#include "bot/team_moves.h"
#include "engine/rules.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace soccer {
namespace bot {

namespace {

struct Entry {
    double lower = 0, upper = 1;
    int turns = -1;
};

class Solver {
public:
    Solver(unsigned attackers, unsigned defenders, const EpisodeValueOptions& o)
        : attackers_(attackers), defenders_(defenders), options_(o) {}

    // Iterative deepening: the value never falls with more turns, so the value
    // with two turns fewer is a floor of the next search's window (exact
    // values above it stay exact).
    double Root(const GameState& s, int turns, double floor) {
        for (int k = 0; k < kNumPlayers; ++k)
            if (options_.frontOpponents >> k & 1u)
                frontReach_[k] = ComputeReach(s.players[k].pos, s.players[k].Velocity());
        double known = 0;
        for (int t = turns % 2 == 0 ? 2 : 1; t < turns && !stopped; t += 2) {
            rootTurns_ = t;
            const double v = Search(s, t, std::max(floor, known > 0 ? known - 1e-12 : 0.0), 1);
            if (!stopped && v > known) known = v;
        }
        rootTurns_ = turns;
        rootValue_ = -1;
        rootFound_ = false;
        rootFloor_ = std::max(floor, known > 0 ? known - 1e-12 : 0.0);
        return Search(s, turns, rootFloor_, 1);
    }

    long nodes = 0, movementStates = 0, leadTried = 0, leadCandidates = 0, freeNodes = 0, leadRaceLost = 0;
    std::size_t n_reserve_ = 64;
    bool stopped = false;

private:
    unsigned attackers_, defenders_;
    EpisodeValueOptions options_;
    int attackTeam_ = -1;
    std::unordered_map<uint64_t, Entry> table_;
    std::unordered_map<uint64_t, bool> raceCache_;
    int depth_ = 0;
    double rootFloor_ = 0;

public:
    double rootValue_ = -1;
    GameState rootState_;
    Action rootAction_ = Action::EndTurn();
    bool rootFound_ = false;

private:
    // The moving front (options_.frontOpponents): their reach, and the root's turns
    // (the moment of a node is rootTurns_ - turns: 0 the attack's turn now, 1 the reply...).
    ReachMap frontReach_[kNumPlayers];
    int rootTurns_ = 0;

    // A free ball: whoever shoots must first stand on it (within a deflection, 2
    // cells) at one of the attack's turns, where it is by then on its untouched
    // path, and dribble from there at most 2 cells a turn.
    double UpperBoundFree(const GameState& s, int turns) const {
        const bool attackNow = s.teamToMove == attackTeam_;
        const int own = attackNow ? (turns + 1) / 2 : turns / 2;
        if (own <= 0) return 0;
        std::vector<Cell> path{s.ball.pos};
        for (Cell c : PredictBallPath(s)) path.push_back(c);
        double bound = 0;
        int phase = s.ball.stepPhase, steps = 0;
        int h = 0;   // half-turns from now
        for (int k = 0; k < own; ++k) {
            const int at = attackNow ? 2 * k : 2 * k + 1;   // half-turn of the attack's k-th turn
            for (; h < at; ++h) {                           // End Turns before it
                if (phase == 0) ++steps;
                phase ^= 1;
            }
            const Cell ball = path[std::min<std::size_t>(steps, path.size() - 1)];
            bool someone = false;
            for (int q = 0; q < kNumPlayers && !someone; ++q) {
                if (!(attackers_ >> q & 1u)) continue;
                const PlayerState& p = s.players[q];
                int moves;
                ReachMap r;
                if (attackNow) {
                    r = p.moved ? ComputeReach(p.pos, p.Displacement()) : ComputeReach(p.pos, p.Velocity());
                    moves = p.moved ? k : k + 1;
                } else {
                    r = ComputeReach(p.pos, p.Velocity());
                    moves = k + 1;
                }
                for (int dx = -2; dx <= 2 && !someone; ++dx)
                    for (int dy = -2; dy <= 2 && !someone; ++dy) {
                        const Cell near{ball.x + dx, ball.y + dy};
                        if (InField(near) && r.Turns(near) <= moves) someone = true;
                    }
            }
            if (someone) bound = std::max(bound, BestOpenWithin(ball, 2 * (own - 1 - k) + 2));
        }
        return bound;
    }

    // The ball of a pass ahead, untouched, is proven the defence's, or goes out
    // or to a keeper before anybody gets it (ball race; cached by position).
    bool RaceLost(const GameState& t, Cell to) {
        GameState c = t;
        ApplyUnchecked(c, Action::Pass(to), PassThrough(c, to));
        if (c.phase != Phase::Play || !c.ball.IsFree()) return false;
        const uint64_t key = PositionHash(c);
        auto it = raceCache_.find(key);
        if (it != raceCache_.end()) return it->second;
        movementStates += 25;   // the solver's cost, counted towards the limit
        const BallRaceResult race = SolveBallRace(c);
        const bool lost = (race.kind == BallRaceKind::Taken && race.team != attackTeam_) || race.kind == BallRaceKind::Restart;
        raceCache_.emplace(key, lost);
        return lost;
    }

    // The untouched outcome of a pass (BallThrough, or the certain one).
    static Outcome PassThrough(const GameState& s, Cell to) {
        const std::vector<Outcome> outs = OutcomesUnchecked(s, Action::Pass(to));
        for (const Outcome& o : outs)
            if (o.kind == OutcomeKind::BallThrough) return o;
        return outs.back();
    }

    // The best open shot of the attack from a cell within Chebyshev distance r of `c`.
    double BestOpenWithin(Cell c, int r) const {
        static thread_local std::vector<double> open[2];
        std::vector<double>& t = open[attackTeam_];
        if (t.empty())
            for (int x = 0; x < kFieldWidth; ++x)
                for (int y = 0; y < kGridHeight; ++y) t.push_back(InField(x, y) ? OpenShotProbability({x, y}, attackTeam_) : 0.0);
        double best = 0;
        for (int x = std::max(0, c.x - r); x <= std::min(kFieldWidth - 1, c.x + r); ++x)
            for (int y = std::max(kFirstRow, c.y - r); y <= std::min(kLastRow, c.y + r); ++y)
                best = std::max(best, t[x * kGridHeight + y]);
        return best;
    }

    // The episode goes on: the attack holds the ball (or, with passes ahead, it is free).
    bool Alive(const GameState& c) const {
        if (c.phase != Phase::Play || c.keeperRestartTeam >= 0) return false;
        if (c.ball.IsFree()) return options_.leadPasses;
        return TeamOf(c.ball.holder) == attackTeam_;
    }

    bool FrontCanStand(int k, Cell c, int moment) const {
        return InField(c) && frontReach_[k].Turns(c) <= (moment + 1) / 2;
    }
    bool FrontInBox(Cell a, Cell b, int moment) const {
        for (int k = 0; k < kNumPlayers; ++k) {
            if (!(options_.frontOpponents >> k & 1u)) continue;
            for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
                for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y)
                    if (Cell{x, y} != a && FrontCanStand(k, {x, y}, moment)) return true;
        }
        return false;
    }
    bool FrontNextTo(Cell h, int moment) const {
        for (int k = 0; k < kNumPlayers; ++k) {
            if (!(options_.frontOpponents >> k & 1u)) continue;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy)
                    if ((dx || dy) && FrontCanStand(k, {h.x + dx, h.y + dy}, moment)) return true;
        }
        return false;
    }
    // The shot with each front opponent who can get into its zone on his worst cell.
    double FrontShot(const GameState& s, int moment) const {
        double p = ShotProbability(s, s.ball.holder);
        if (!options_.frontOpponents || p <= 0) return p;
        const Cell from = s.players[s.ball.holder].pos;
        for (int k = 0; k < kNumPlayers; ++k) {
            if (!(options_.frontOpponents >> k & 1u)) continue;
            double worst = 1;
            for (int x = 0; x < kFieldWidth; ++x)
                for (int y = kFirstRow; y <= kLastRow; ++y)
                    if (Cell{x, y} != from && FrontCanStand(k, {x, y}, moment))
                        worst = std::min(worst, ShotBlockFactor(from, attackTeam_, {x, y}));
            if (worst < 1) p = p / ShotBlockFactor(from, attackTeam_, s.players[k].pos) * worst;
        }
        return p;
    }

    // The distinct states the team to move reaches by moving its participants
    // in `mask` (each at most once, in any order), the state itself first.
    // Without random events or a free ball a state is fixed by the movers'
    // cells and moved flags, so those are the key.
    std::vector<GameState> Movements(const GameState& s, unsigned mask) {
        std::vector<GameState> out;
        out.reserve(n_reserve_);
        out.push_back(s);
        int movers[kPlayersPerTeam], n = 0;
        const int first = FirstPlayerOf(s.teamToMove);
        for (int k = first; k < first + kPlayersPerTeam; ++k)
            if ((mask >> k & 1u) && !s.players[k].moved) movers[n++] = k;
        if (n == 0) return out;
        std::vector<uint32_t> seen;
        const auto key = [&](const GameState& t) {
            uint32_t v = 0;
            for (int i = 0; i < n; ++i) {
                const PlayerState& p = t.players[movers[i]];
                v = v << 10 | static_cast<uint32_t>(p.pos.x) << 6 | static_cast<uint32_t>(p.pos.y) << 1 |
                    (p.moved ? 1u : 0u);
            }
            return v;
        };
        seen.push_back(key(s));
        for (std::size_t i = 0; i < out.size(); ++i) {
            for (int m = 0; m < n; ++m) {
                const int k = movers[m];
                if (out[i].players[k].moved) continue;
                const GameState parent = out[i];
                for (Cell cell : MoveDestinations(parent, k)) {
                    if (cell == parent.players[k].pos) continue;
                    GameState child = parent;
                    ApplyUnchecked(child, Action::Move(k, cell), Outcome::Certain());
                    const uint32_t v = key(child);
                    if (std::find(seen.begin(), seen.end(), v) != seen.end()) continue;
                    seen.push_back(v);
                    out.push_back(child);
                }
            }
        }
        if (out.size() > n_reserve_) n_reserve_ = out.size();
        return out;
    }

    static uint64_t Key(const GameState& s, int turns) {
        return PositionHash(s) * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(turns);
    }

    int AttackTeam(const GameState& s) const {
        for (int k = 0; k < kNumPlayers; ++k)
            if (attackers_ >> k & 1u) return TeamOf(k);
        return TeamOf(s.ball.holder);
    }

    // The best open shot from the cells a player standing on `cell` with
    // `velocity` can stand on within `own` turns (cached; ComputeReach).
    static double BestReachableShot(Cell cell, Cell velocity, int own, int team) {
        constexpr int kTurns = 16, kV = 5;
        static thread_local std::vector<float> cache;
        static thread_local std::vector<double> open;   // OpenShotProbability by team and cell
        if (open.empty()) {
            open.resize(std::size_t(2) * kFieldWidth * kGridHeight, 0.0);
            for (int t = 0; t < 2; ++t)
                for (int x = 0; x < kFieldWidth; ++x)
                    for (int y = kFirstRow; y <= kLastRow; ++y)
                        open[(std::size_t(t) * kFieldWidth + x) * kGridHeight + y] = OpenShotProbability(Cell{x, y}, t);
        }
        const auto bestFor = [&](const ReachMap& map, double best[kTurns]) {
            for (int i = 0; i < kTurns; ++i) best[i] = 0;
            for (int x = 0; x < kFieldWidth; ++x)
                for (int y = kFirstRow; y <= kLastRow; ++y) {
                    const int t = map.turns[x][y];
                    if (t == kUnreachable || t >= kTurns) continue;
                    best[t] = std::max(best[t], open[(std::size_t(team) * kFieldWidth + x) * kGridHeight + y]);
                }
            for (int i = 1; i < kTurns; ++i) best[i] = std::max(best[i], best[i - 1]);
        };
        if (own <= 0) return OpenShotProbability(cell, team);
        own = std::min(own, kTurns - 1);
        if (Abs(velocity.x) > 2 || Abs(velocity.y) > 2) {
            double best[kTurns];
            bestFor(ComputeReach(cell, velocity), best);
            return best[own];
        }
        if (cache.empty()) cache.assign(std::size_t(kFieldWidth) * kGridHeight * kV * kV * kTurns * 2, -1.0f);
        const std::size_t base =
            (((std::size_t(team) * kFieldWidth + cell.x) * kGridHeight + cell.y) * kV + (velocity.x + 2)) * kV +
            (velocity.y + 2);
        float* entry = &cache[base * kTurns];
        if (entry[own] < 0) {
            double best[kTurns];
            bestFor(ComputeReach(cell, velocity), best);
            // Stored rounded up, so the bound stays a bound.
            for (int i = 0; i < kTurns; ++i) {
                float stored = static_cast<float>(best[i]);
                if (static_cast<double>(stored) < best[i]) stored = std::nextafter(stored, 2.0f);
                entry[i] = stored;
            }
        }
        return entry[own];
    }

    // The bound after the attack ends its turn in `t` (its players keep the
    // displacement of this turn as their velocity).
    double UpperBoundAfterAttack(const GameState& t, int turns) const {
        const int own = (turns - 1) / 2;   // own turns after the defence's turn
        if (own <= 0) return 0;
        double best = 0;
        for (int k = 0; k < kNumPlayers; ++k)
            if (attackers_ >> k & 1u)
                best = std::max(best, BestReachableShot(t.players[k].pos, t.players[k].Displacement(), own, attackTeam_));
        return best;
    }

    // The best open shot the attack can reach in its remaining own turns: no
    // line can be worth more.
    double UpperBound(const GameState& s, int turns) const {
        const int team = AttackTeam(s);
        const int own = s.teamToMove == team ? (turns + 1) / 2 : turns / 2;
        if (own <= 0) return 0;
        double best = 0;
        const bool mover = s.teamToMove == team;
        for (int k = 0; k < kNumPlayers; ++k) {
            if (!(attackers_ >> k & 1u)) continue;
            const PlayerState& p = s.players[k];
            const double v = mover && p.moved ? BestReachableShot(p.pos, p.Displacement(), own - 1, team)
                             : mover ? BestReachableShot(p.turnStart, p.Velocity(), own, team)
                                     : BestReachableShot(p.pos, p.Velocity(), own, team);
            best = std::max(best, v);
        }
        return best;
    }

    double ShotValue(const GameState& s) const { return ShotProbability(s, s.ball.holder); }

    // The value after `a` from `s` by its outcomes: `cont` values the states
    // in which the attack keeps the ball (else 0).
    // As ByOutcomes, with the window (alpha, beta) of the expectation: when one
    // outcome continues (probability P; the others are worth 0), its value is
    // wanted only within (alpha / P, beta / P) - the alpha-beta window scaled.
    template <class F>
    double ByOutcomesWindow(const GameState& s, const Action& a, double alpha, double beta, F cont) {
        const std::vector<Outcome> outs = OutcomesUnchecked(s, a);
        GameState kept[4];
        double prob[4];
        int continuing = 0;
        bool many = false;
        for (const Outcome& o : outs) {
            GameState c = s;
            ApplyUnchecked(c, a, o);
            if (!Alive(c)) continue;
            if (options_.leadPasses && c.ball.IsFree() && o.kind != OutcomeKind::BallThrough &&
                o.kind != OutcomeKind::Certain && o.probability < options_.rareOutcome)
                continue;
            if (continuing == 4) { many = true; break; }
            kept[continuing] = c;
            prob[continuing++] = o.probability;
        }
        if (many) return ByOutcomes(s, a, [&](const GameState& c) { return cont(c, 0.0, 1.0); });
        if (continuing == 0) return 0;
        if (continuing == 1) {
            const double p = prob[0];
            if (p <= 0) return 0;
            if (alpha >= p) return p;   // an upper bound at or below alpha: fails low
            return p * cont(kept[0], alpha / p, std::min(1.0, beta / p));
        }
        double v = 0;
        for (int i = 0; i < continuing; ++i) v += prob[i] * cont(kept[i], 0.0, 1.0);
        return v;
    }

    template <class F>
    double ByOutcomes(const GameState& s, const Action& a, F cont) {
        double v = 0;
        for (const Outcome& o : OutcomesUnchecked(s, a)) {
            GameState c = s;
            ApplyUnchecked(c, a, o);
            if (!Alive(c)) continue;
            // Passes ahead: a free ball after a rare touch is not followed (worth 0:
            // the value stays a lower bound of the model).
            if (options_.leadPasses && c.ball.IsFree() && o.kind != OutcomeKind::BallThrough &&
                o.kind != OutcomeKind::Certain && o.probability < options_.rareOutcome)
                continue;
            v += o.probability * cont(c);
        }
        return v;
    }

    double AfterEnd(const GameState& s, int turns, double alpha, double beta) {
        const int moment = rootTurns_ - turns;
        if (options_.frontOpponents && s.teamToMove == attackTeam_ && !s.ball.IsFree() &&
            FrontNextTo(s.players[s.ball.holder].pos, moment + 1))
            return 0;
        if (options_.frontOpponents && s.ball.IsFree()) {
            // A front opponent who can step onto the ball in the next turn, or stand
            // in the box of the step this End Turn makes, takes it away.
            if (s.teamToMove == attackTeam_)
                for (int k = 0; k < kNumPlayers; ++k)
                    if ((options_.frontOpponents >> k & 1u) && FrontCanStand(k, s.ball.pos, moment + 1)) return 0;
            if (s.ball.stepPhase == 0 &&
                FrontInBox(s.ball.pos, Cell{s.ball.pos.x + s.ball.vel.x, s.ball.pos.y + s.ball.vel.y}, moment))
                return 0;
        }
        if (s.ball.IsFree())
            return ByOutcomesWindow(s, Action::EndTurn(), alpha, beta, [&](const GameState& c, double a, double b) {
                return Search(c, turns - 1, a, b);
            });
        GameState c = s;
        const std::vector<Outcome> outs = OutcomesUnchecked(c, Action::EndTurn());
        ApplyUnchecked(c, Action::EndTurn(), outs.front());
        if (!Alive(c)) return 0;
        return Search(c, turns - 1, alpha, beta);
    }

    double Search(const GameState& s, int turns, double alpha, double beta) {
        if (attackTeam_ < 0) attackTeam_ = AttackTeam(s);
        if (!Alive(s) || turns <= 0) return 0;
        if (s.ball.IsFree()) ++freeNodes;
        if (stopped) return 0;
        ++nodes;
        if (movementStates > options_.nodeLimit) { stopped = true; return 0; }
        const double ub = s.ball.IsFree() ? std::min(UpperBound(s, turns), UpperBoundFree(s, turns)) : UpperBound(s, turns);
        if (ub <= alpha) return ub;
        beta = std::min(beta, ub);
        const uint64_t key = Key(s, turns) ^ (options_.frontOpponents ? static_cast<uint64_t>(rootTurns_ - turns) * 0xC2B2AE3D27D4EB4Full : 0);
        auto it = table_.find(key);
        if (it != table_.end()) {
            const Entry& e = it->second;
            if (e.lower >= beta) return e.lower;
            if (e.upper <= alpha) return e.upper;
            if (e.lower == e.upper) return e.lower;
            alpha = std::max(alpha, e.lower);
            beta = std::min(beta, e.upper);
        }
        const double a0 = alpha, b0 = beta;
        const bool root = depth_ == 0;
        ++depth_;
        const double v = s.teamToMove == attackTeam_ ? AttackTurn(s, turns, alpha, beta, root)
                                                     : DefenceTurn(s, turns, alpha, beta);
        --depth_;
        if (stopped) return v;
        Entry& e = table_[key];
        if (e.turns != turns) e = Entry{0, 1, turns};
        if (v <= a0) e.upper = std::min(e.upper, v);
        else if (v >= b0) e.lower = std::max(e.lower, v);
        else e.lower = e.upper = v;
        return v;
    }

    // The root's best decision so far (the attack's turn at the root): the state
    // after its movements and the ball action (Shot, Pass or EndTurn).
    void Note(bool root, double v, const GameState& t, const Action& a) {
        if (root && v > rootValue_ && v > rootFloor_) {
            rootValue_ = v;
            rootState_ = t;
            rootAction_ = a;
            rootFound_ = true;
        }
    }

    double AttackTurn(const GameState& s, int turns, double alpha, double beta, bool root = false) {
        const GoalEpisode episode{attackTeam_, GoalEpisodeStatus::OurAttack};
        const unsigned frozen = ((1u << kNumPlayers) - 1) & ~attackers_;
        (void)episode; (void)frozen;
        // While a pass ahead rolls, the attack's players behind the ball (further
        // from the goal than it) stand: they can not get to it before the runner
        // nor help the move it starts (a restriction of the attack: the value
        // stays a lower bound of the model).
        unsigned movers = attackers_;
        if (options_.leadPasses && s.ball.IsFree()) {
            const int goal = AttackedGoalRow(attackTeam_);
            for (int k = 0; k < kNumPlayers; ++k)
                if ((movers >> k & 1u) && Abs(s.players[k].pos.y - goal) > Abs(s.ball.pos.y - goal)) movers &= ~(1u << k);
        }
        const std::vector<GameState> states = Movements(s, movers);
        movementStates += static_cast<long>(states.size());
        // Order: the best shot first.
        struct Item { double shot; std::size_t index; };
        std::vector<Item> items;
        items.reserve(states.size());
        for (std::size_t i = 0; i < states.size(); ++i) {
            const GameState& t = states[i];
            const bool canShoot = !t.ballPlayed && t.ball.holder != kNoPlayer && TeamOf(t.ball.holder) == attackTeam_;
            items.push_back({canShoot ? FrontShot(t, rootTurns_ - turns) : 0.0, i});
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.shot > b.shot; });
        double best = 0;
        for (const Item& item : items) {
            if (stopped) break;
            const GameState& t = states[item.index];
            if (!Alive(t)) continue;
            if (item.shot > 0) Note(root, item.shot, t, Action::Shot());
            best = std::max(best, item.shot);
            if (best >= beta) return best;
            alpha = std::max(alpha, best);
            // Nothing after the turn can beat alpha: only the shot counts here.
            if (UpperBoundAfterAttack(t, turns) <= alpha) continue;
            // Keep the ball.
            {
                const double keep = AfterEnd(t, turns, alpha, beta);
                Note(root, keep, t, Action::EndTurn());
                best = std::max(best, keep);
            }
            if (best >= beta) return best;
            alpha = std::max(alpha, best);
            // A pass into a partner's feet, then the turn ends.
            if (!t.ballPlayed && !t.ball.IsFree()) {
                for (int q = 0; q < kNumPlayers; ++q) {
                    if (!(attackers_ >> q & 1u) || q == t.ball.holder) continue;
                    const Cell from = t.players[t.ball.holder].pos, to = t.players[q].pos;
                    if (Abs(to.x - from.x) > kMaxPassDistance || Abs(to.y - from.y) > kMaxPassDistance) continue;
                    if (options_.frontOpponents && FrontInBox(from, to, rootTurns_ - turns)) continue;
                    const double v = ByOutcomesWindow(t, Action::Pass(to), alpha, beta,
                                                      [&](const GameState& c, double a, double b) {
                                                          return AfterEnd(c, turns, a, b);
                                                      });
                    Note(root, v, t, Action::Pass(to));
                    best = std::max(best, v);
                    if (best >= beta) return best;
                    alpha = std::max(alpha, best);
                }
            }
            // A pass ahead: into an empty cell a partner can reach within two of
            // his turns; the rest of the turn (the partners who have not moved) follows.
            // Each is bounded first: whoever shoots later must have picked the ball up
            // on its path (or within a deflection of it) and dribbled from there, at
            // most 2 cells a turn; the passes are tried by that bound, best first.
            if (options_.leadPasses && !t.ballPlayed && !t.ball.IsFree()) {
                const Cell from = t.players[t.ball.holder].pos;
                const int ownLeft = (turns - 1) / 2;   // the attack's turns after this one
                if (ownLeft <= 0) continue;
                ReachMap reach[kNumPlayers];
                int partners[kNumPlayers], nPartners = 0;
                for (int q = 0; q < kNumPlayers; ++q) {
                    if (!(attackers_ >> q & 1u) || q == t.ball.holder) continue;
                    const PlayerState& p = t.players[q];
                    reach[nPartners] = p.moved ? ComputeReach(p.pos, p.Displacement()) : ComputeReach(p.pos, p.Velocity());
                    partners[nPartners++] = q;
                }
                const PlayerState& hp = t.players[t.ball.holder];
                const ReachMap holderReach = hp.moved ? ComputeReach(hp.pos, hp.Displacement()) : ComputeReach(hp.pos, hp.Velocity());
                struct Lead { double bound; Cell to; };
                std::vector<Lead> leads;
                for (int dy = -kMaxPassDistance; dy <= kMaxPassDistance; ++dy)
                    for (int dx = -kMaxPassDistance; dx <= kMaxPassDistance; ++dx) {
                        const Cell to{from.x + dx, from.y + dy};
                        if (!InField(to) || to == from || PlayerAt(t, to) != kNoPlayer) continue;
                        // Ahead only: nearer the goal than the passer (a restriction of the
                        // attack: the value stays a lower bound of the model).
                        if (Abs(to.y - AttackedGoalRow(attackTeam_)) >= Abs(from.y - AttackedGoalRow(attackTeam_))) continue;
                        // A partner who can still step onto the target in this turn takes
                        // the ball into his feet instead (moved there first): not a pass ahead.
                        bool reachable = false, now = false;
                        for (int i = 0; i < nPartners; ++i) {
                            const int turnsTo = reach[i].Turns(to);
                            if (turnsTo <= 2) reachable = true;
                            if (!t.players[partners[i]].moved && turnsTo <= 1) now = true;
                        }
                        if (!reachable || now) continue;
                        if (options_.frontOpponents && FrontInBox(from, to, rootTurns_ - turns)) continue;
                        // The ball's path untouched: on the target through our turn now and
                        // the reply, then one step for each further round. Whoever shoots must
                        // stand on the ball (within a deflection: 2 cells) at some own turn j
                        // and then dribbles at most 2 cells in each of the ownLeft - j turns.
                        GameState c = t;
                        ApplyUnchecked(c, Action::Pass(to), PassThrough(c, to));
                        std::vector<Cell> path{to};
                        if (c.phase == Phase::Play && c.ball.IsFree())
                            for (Cell cell : PredictBallPath(c)) path.push_back(cell);
                        double bound = 0;
                        for (int j = 1; j <= ownLeft; ++j) {
                            const Cell ball = path[std::min<std::size_t>(j, path.size() - 1)];
                            bool someone = false;
                            for (int q = 0; q < kNumPlayers && !someone; ++q) {
                                if (!(attackers_ >> q & 1u)) continue;
                                const PlayerState& p = t.players[q];
                                const int moves = p.moved ? j : j + 1;
                                const ReachMap& r = q == t.ball.holder
                                                        ? holderReach
                                                        : reach[std::find(partners, partners + nPartners, q) - partners];
                                for (int ddx = -2; ddx <= 2 && !someone; ++ddx)
                                    for (int ddy = -2; ddy <= 2 && !someone; ++ddy) {
                                        const Cell near{ball.x + ddx, ball.y + ddy};
                                        if (InField(near) && r.Turns(near) <= moves) someone = true;
                                    }
                            }
                            if (someone) bound = std::max(bound, BestOpenWithin(ball, 2 * (ownLeft - j) + 2));
                        }
                        if (c.phase != Phase::Play) continue;
                        ++leadCandidates;
                        if (bound > alpha) leads.push_back({bound, to});
                    }
                std::stable_sort(leads.begin(), leads.end(), [](const Lead& x, const Lead& y) { return x.bound > y.bound; });
                for (const Lead& lead : leads) {
                    if (stopped || lead.bound <= alpha) break;
                    // The untouched ball proven to the defence, out or to a keeper: the
                    // pass lives only by a deflection - not followed (a lower bound).
                    if (RaceLost(t, lead.to)) { ++leadRaceLost; continue; }
                    ++leadTried;
                    const double v = ByOutcomesWindow(t, Action::Pass(lead.to), alpha, std::min(beta, lead.bound),
                                                      [&](const GameState& c, double x, double y) {
                                                          return AttackTurn(c, turns, x, y);
                                                      });
                    Note(root, v, t, Action::Pass(lead.to));
                    best = std::max(best, v);
                    if (best >= beta) return best;
                    alpha = std::max(alpha, best);
                }
            }
        }
        return best;
    }

    // The defence: tackles (each participant once, now or after moving) and movements.
    double DefenceTurn(const GameState& s, int turns, double alpha, double beta) {
        double best = 1;
        // A tackle now.
        for (const Action& a : LegalActions(s)) {
            if (stopped) break;
            if (a.type != ActionType::Attack || !(defenders_ >> a.player & 1u)) continue;
            const double v = ByOutcomesWindow(s, a, alpha, beta, [&](const GameState& c, double x, double y) {
                return DefenceTurn(c, turns, x, y);
            });
            best = std::min(best, v);
            if (best <= alpha) return best;
            beta = std::min(beta, best);
        }
        // Movements, then tackles by those who can, then the end of the turn.
        const GoalEpisode episode{Opponent(attackTeam_), GoalEpisodeStatus::OurAttack};
        const unsigned frozen = ((1u << kNumPlayers) - 1) & ~defenders_;
        (void)episode; (void)frozen;
        const std::vector<GameState> states = Movements(s, defenders_);
        movementStates += static_cast<long>(states.size());
        // Order: the holder's worst shot first.
        std::vector<std::pair<double, std::size_t>> order;
        order.reserve(states.size());
        for (std::size_t i = 0; i < states.size(); ++i) {
            const GameState& t = states[i];
            order.push_back({t.ball.IsFree() ? 0.0 : ShotProbability(t, t.ball.holder), i});
        }
        std::stable_sort(order.begin(), order.end());
        for (const auto& item : order) {
            if (stopped) break;
            const GameState& t = states[item.second];
            if (!Alive(t)) return 0;
            const double v = TacklesThenEnd(t, turns, alpha, beta);
            best = std::min(best, v);
            if (best <= alpha) return best;
            beta = std::min(beta, best);
        }
        return best;
    }

    double TacklesThenEnd(const GameState& t, int turns, double alpha, double beta) {
        double best = AfterEnd(t, turns, alpha, beta);
        if (best <= alpha) return best;
        for (const Action& a : LegalActions(t)) {
            if (stopped) break;
            if (a.type != ActionType::Attack || !(defenders_ >> a.player & 1u)) continue;
            const double v = ByOutcomesWindow(t, a, alpha, std::min(beta, best),
                                              [&](const GameState& c, double x, double y) {
                                                  return TacklesThenEnd(c, turns, x, y);
                                              });
            best = std::min(best, v);
            if (best <= alpha) return best;
        }
        return best;
    }
};

}  // namespace

EpisodeValueResult EpisodeValue(const GameState& s, unsigned attackers, unsigned defenders,
                                const EpisodeValueOptions& options) {
    Solver solver(attackers, defenders, options);
    EpisodeValueResult r;
    r.value = solver.Root(s, options.turns, std::max(0.0, options.floor));
    r.nodes = solver.nodes;
    r.movementStates = solver.movementStates;
    r.leadTried = solver.leadTried;
    r.leadCandidates = solver.leadCandidates;
    r.freeNodes = solver.freeNodes;
    r.leadRaceLost = solver.leadRaceLost;
    r.rootFound = solver.rootFound_ && !solver.stopped;
    r.rootState = solver.rootState_;
    r.rootAction = solver.rootAction_;
    r.rootValue = solver.rootValue_;
    r.complete = !solver.stopped;
    return r;
}

}  // namespace bot
}  // namespace soccer
