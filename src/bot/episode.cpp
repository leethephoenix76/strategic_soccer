#include "bot/episode.h"

#include "bot/ball_race.h"
#include "bot/reach.h"
#include "engine/notation.h"
#include "engine/rules.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace soccer {
namespace bot {

namespace {

constexpr int kMaxPlies = 16;

// A set of cells: one bit per column, one word per row (rows 0..20).
struct CellSet {
    uint16_t row[kGridHeight] = {};

    bool Empty() const {
        for (uint16_t r : row)
            if (r) return false;
        return true;
    }
    bool Intersects(const CellSet& o) const {
        for (int y = 0; y < kGridHeight; ++y)
            if (row[y] & o.row[y]) return true;
        return false;
    }
    void Add(Cell c) { row[c.y] |= static_cast<uint16_t>(1u << c.x); }
    bool Has(Cell c) const { return row[c.y] >> c.x & 1u; }
    CellSet& operator|=(const CellSet& o) {
        for (int y = 0; y < kGridHeight; ++y) row[y] |= o.row[y];
        return *this;
    }
    CellSet operator&(const CellSet& o) const {
        CellSet r;
        for (int y = 0; y < kGridHeight; ++y) r.row[y] = row[y] & o.row[y];
        return r;
    }
};

Outcome Untouched(const std::vector<Outcome>& outs) {
    for (const Outcome& o : outs)
        if (o.kind == OutcomeKind::BallThrough) return o;
    return outs.front();
}

// ---- The attack episode by the race to the goal (7.67) ----------------------

constexpr int kNever = 1000;

// The ply at whose end a player can stand on each cell (0: where he is now).
struct PlyMap {
    int ply[kFieldWidth][kGridHeight];
    int At(Cell c) const { return ply[c.x][c.y]; }
};

PlyMap PlayerPlies(const GameState& s, int k) {
    const PlayerState& p = s.players[k];
    const bool mover = TeamOf(k) == s.teamToMove;
    ReachMap map;
    int first;   // the ply of his first own move from now
    if (mover && p.moved) {
        map = ComputeReach(p.pos, p.Displacement());
        first = 3;
    } else if (mover) {
        map = ComputeReach(p.turnStart, p.Velocity());
        first = 1;
    } else {
        map = ComputeReach(p.pos, p.Velocity());
        first = 2;
    }
    PlyMap m;
    for (int x = 0; x < kFieldWidth; ++x)
        for (int y = 0; y < kGridHeight; ++y) {
            const int turns = map.turns[x][y];
            m.ply[x][y] = turns == kUnreachable ? kNever : turns == 0 ? 0 : first + 2 * (turns - 1);
        }
    return m;
}

// The carrier of a free ball: he stands on it at the end of ply `pickup` in
// `cell`, then moves on his own plies (from rest: no head start towards the goal).
PlyMap CarrierPlies(Cell cell, int pickup) {
    const ReachMap map = ComputeReach(cell, Cell{0, 0});
    PlyMap m;
    for (int x = 0; x < kFieldWidth; ++x)
        for (int y = 0; y < kGridHeight; ++y) {
            const int turns = map.turns[x][y];
            m.ply[x][y] = turns == kUnreachable ? kNever : pickup + 2 * turns;
        }
    return m;
}

bool NotBehind(Cell c, Cell ref, int goalRow) { return Abs(c.y - goalRow) <= Abs(ref.y - goalRow); }

std::string PlyText(int ply) { return ply >= kNever ? "never" : "ply " + std::to_string(ply); }

}  // namespace

const char* AttackEpisodeName(AttackEpisodeKind kind) {
    switch (kind) {
        case AttackEpisodeKind::None: return "none";
        case AttackEpisodeKind::OneVsZero: return "1 v 0";
        case AttackEpisodeKind::TwoVsOne: return "2 v 1";
        case AttackEpisodeKind::General: return "general";
    }
    return "?";
}

AttackEpisode AnalyzeAttackEpisode(const GameState& s, bool explain, bool ballRace) {
    AttackEpisode e;
    if (s.phase != Phase::Play) return e;
    PlyMap plies[kNumPlayers];
    for (int k = 0; k < kNumPlayers; ++k) plies[k] = PlayerPlies(s, k);

    // The carrier and when he has the ball.
    Cell ref;
    PlyMap carrierPlies;
    if (!s.ball.IsFree()) {
        e.carrier = s.ball.holder;
        e.team = TeamOf(e.carrier);
        e.carrierPly = 0;
        ref = s.players[e.carrier].pos;
        carrierPlies = plies[e.carrier];
    } else if (ballRace) {
        // Who takes the ball first by the race solver: the moment of a move
        // in slot i (or of the End Turn after it) is the end of ply i + 1.
        const BallRaceResult r = SolveBallRace(s);
        if (r.kind != BallRaceKind::Taken || r.player == kNoPlayer) {
            if (explain)
                for (int k = 0; k < kNumPlayers; ++k)
                    e.reason[k] = r.kind == BallRaceKind::Restart ? "the ball leaves play first"
                                                                  : "contested free ball (race solver)";
            return e;   // kind None: everybody takes part
        }
        e.team = r.team;
        e.carrier = r.player;
        e.carrierPly = r.moment < 0 ? 0 : r.moment / 2 + 1;
        ref = r.cell;
        carrierPlies = CarrierPlies(ref, e.carrierPly);
    } else {
        // The untouched path and the earliest ply each player can stand on the
        // ball's cell (as the race module's bounds); a team whose runner is a
        // whole round earlier gets the ball, otherwise the ball is contested.
        Cell path[kMaxPlies + 1];
        int n = 0;
        GameState t = s;
        while (n <= kMaxPlies && t.phase == Phase::Play && t.ball.IsFree()) {
            path[n++] = t.ball.pos;
            ApplyUnchecked(t, Action::EndTurn(), Untouched(OutcomesUnchecked(t, Action::EndTurn())));
        }
        int best[kNumTeams] = {kNever, kNever}, runner[kNumTeams] = {kNoPlayer, kNoPlayer};
        for (int k = 0; k < kNumPlayers; ++k)
            for (int h = 0; h < n; ++h) {
                // During ply h + 1 the ball rests on path[h].
                const int ply = h + 1;
                if ((ply % 2 == 1) != (TeamOf(k) == s.teamToMove)) continue;
                if (plies[k].At(path[h]) <= ply) {
                    if (ply < best[TeamOf(k)]) {
                        best[TeamOf(k)] = ply;
                        runner[TeamOf(k)] = k;
                    }
                    break;
                }
            }
        int winner = -1;
        if (best[kWhite] + 2 <= best[kBlack]) winner = kWhite;
        if (best[kBlack] + 2 <= best[kWhite]) winner = kBlack;
        if (winner < 0) {
            if (explain)
                for (int k = 0; k < kNumPlayers; ++k) e.reason[k] = "contested free ball";
            e.attackers = e.defenders = 0;
            return e;   // kind None: everybody takes part
        }
        e.team = winner;
        e.carrier = runner[winner];
        e.carrierPly = best[winner];
        ref = path[best[winner] - 1];
        carrierPlies = CarrierPlies(ref, e.carrierPly);
    }
    const int team = e.team;
    const int goalRow = AttackedGoalRow(team);
    const int defending = Opponent(team);
    constexpr int kHorizon = 2 * kMaxPlies;

    // An attacker's way to the goal: the cells not behind his reference cell,
    // between the columns of the goal posts widened to his column and one more.
    struct Way {
        int player;
        Cell ref;
        const PlyMap* time;
    };
    const auto onWay = [&](const Way& w, Cell c) {
        const int left = std::min(w.ref.x, kGoalLeft) - 1, right = std::max(w.ref.x, kGoalRight) + 1;
        return c.x >= left && c.x <= right && NotBehind(c, w.ref, goalRow);
    };
    const auto dist = [&](Cell c) { return Abs(c.y - goalRow); };
    // progress[t]: the nearest row to the goal an attacker of the way can stand on by ply t.
    const auto progressOf = [&](const Way& w, int progress[kHorizon + 1]) {
        for (int t = 0; t <= kHorizon; ++t) progress[t] = dist(w.ref);
        for (int y = kFirstRow; y <= kLastRow; ++y)
            for (int x = 0; x < kFieldWidth; ++x) {
                const Cell c{x, y};
                const int ply = w.time->At(c);
                if (!onWay(w, c) || ply > kHorizon) continue;
                for (int t = ply; t <= kHorizon; ++t) progress[t] = std::min(progress[t], dist(c));
            }
    };
    std::vector<Way> ways{{e.carrier, ref, &carrierPlies}};
    int carrierProgress[kHorizon + 1];
    progressOf(ways[0], carrierProgress);
    // The episode ends when the carrier can stand on the best shooting cell of
    // his way (the highest open shot probability); whoever comes later is late.
    double bestShot = -1;
    int end = kHorizon;
    for (int y = kFirstRow; y <= kLastRow; ++y)
        for (int x = 0; x < kFieldWidth; ++x) {
            const Cell c{x, y};
            const int ply = carrierPlies.At(c);
            if (!onWay(ways[0], c) || ply > kHorizon) continue;
            const double p = OpenShotProbability(c, team);
            if (p > bestShot || (p == bestShot && ply < end)) {
                bestShot = p;
                end = ply;
            }
        }
    e.endPly = end;
    e.attackers = 1u << e.carrier;
    if (explain) e.reason[e.carrier] = std::string("carrier, has the ball at ") + PlyText(e.carrierPly);

    // Partners who keep pace: level with or ahead of the carrier's best progress
    // on his way, when they get there.
    for (int q = FirstPlayerOf(team); q < FirstPlayerOf(team) + kPlayersPerTeam; ++q) {
        if (q == e.carrier) continue;
        Cell where{};
        bool pace = false;
        for (int y = kFirstRow; y <= kLastRow && !pace; ++y)
            for (int x = 0; x < kFieldWidth && !pace; ++x) {
                const Cell c{x, y};
                const int ply = plies[q].At(c);
                if (ply <= end && onWay(ways[0], c) && dist(c) <= carrierProgress[ply]) {
                    pace = true;
                    where = c;
                }
            }
        if (pace) {
            e.attackers |= 1u << q;
            ways.push_back({q, s.players[q].pos, &plies[q]});
            if (explain)
                e.reason[q] = "keeps pace: " + CellName(where) + " at " + PlyText(plies[q].At(where)) +
                              ", the carrier " + std::to_string(carrierProgress[plies[q].At(where)]) +
                              " rows from the goal by then";
        } else if (explain) {
            e.reason[q] = "behind the play";
        }
    }
    // The attack's progress: the most advanced of its players.
    int progress[kHorizon + 1];
    for (int t = 0; t <= kHorizon; ++t) progress[t] = carrierProgress[t];
    for (std::size_t i = 1; i < ways.size(); ++i) {
        int own[kHorizon + 1];
        progressOf(ways[i], own);
        for (int t = 0; t <= kHorizon; ++t) progress[t] = std::min(progress[t], own[t]);
    }

    // Defenders who get goal-side in time: onto a cell of an attacker's way at
    // least as near the goal as the attack can be one ply earlier (a tie goes to
    // the defence: a ball pushed forward waits there during the defenders' turn).
    for (int d = FirstPlayerOf(defending); d < FirstPlayerOf(defending) + kPlayersPerTeam; ++d) {
        bool in = false;
        std::string why;
        for (int y = kFirstRow; y <= kLastRow && !in; ++y)
            for (int x = 0; x < kFieldWidth && !in; ++x) {
                const Cell c{x, y};
                const int ply = plies[d].At(c);
                if (ply > end + 1) continue;
                bool way = false;
                for (const Way& w : ways) way = way || onWay(w, c);
                if (!way) continue;
                const int attack = progress[std::max(0, ply - 1)];
                if (dist(c) <= attack) {
                    in = true;
                    if (explain)
                        why = "goal-side: " + CellName(c) + " at " + PlyText(ply) + ", the attack " +
                              std::to_string(attack) + " rows from the goal by then";
                }
            }
        if (in) {
            e.defenders |= 1u << d;
            if (explain) e.reason[d] = why;
        } else if (explain) {
            e.reason[d] = "can not get goal-side in time";
        }
    }
    e.outside = ((1u << kNumPlayers) - 1) & ~(e.attackers | e.defenders);
    int nA = 0, nD = 0;
    for (int k = 0; k < kNumPlayers; ++k) {
        nA += e.attackers >> k & 1u;
        nD += e.defenders >> k & 1u;
    }
    e.kind = nD == 0 ? AttackEpisodeKind::OneVsZero
           : (nA == 2 && nD == 1) ? AttackEpisodeKind::TwoVsOne : AttackEpisodeKind::General;
    return e;
}

std::vector<Action> OutsiderMoves(const GameState& s, const AttackEpisode& e) {
    std::vector<Action> moves;
    if (s.phase != Phase::Play || e.team < 0 || !e.outside) return moves;
    GameState t = s;
    const int attackedRow = AttackedGoalRow(e.team);
    const Cell ball = s.ball.pos;
    const Cell carrier = e.carrier != kNoPlayer && !s.ball.IsFree() ? s.players[e.carrier].pos : ball;
    const int first = FirstPlayerOf(s.teamToMove);
    for (int k = first; k < first + kPlayersPerTeam; ++k) {
        if (!(e.outside >> k & 1u) || t.players[k].moved) continue;
        const bool defender = TeamOf(k) != e.team;
        // The cells he runs for, and the goal line that breaks ties.
        CellSet target;
        for (int y = kFirstRow; y <= kLastRow; ++y)
            for (int x = 0; x < kFieldWidth; ++x) {
                const Cell c{x, y};
                if (defender) {
                    // Between the ball and his own goal (the attacked goal's columns).
                    if (x >= std::min(ball.x, kGoalLeft) && x <= std::max(ball.x, kGoalRight) &&
                        NotBehind(c, ball, attackedRow))
                        target.Add(c);
                } else if (ChebyshevDistance(c, carrier) <= kMaxPassDistance && NotBehind(c, carrier, attackedRow)) {
                    target.Add(c);
                }
            }
        const int tieRow = attackedRow;
        const PlayerState& p = t.players[k];
        Cell bestCell = p.pos;
        int bestTurns = kNever, bestTie = kNever, bestSide = kNever;
        std::vector<Cell> options = MoveDestinations(t, k);
        options.insert(options.begin(), p.pos);
        for (Cell d : options) {
            const ReachMap map = ComputeReach(d, Cell{d.x - p.turnStart.x, d.y - p.turnStart.y});
            int turns = kNever;
            for (int y = kFirstRow; y <= kLastRow; ++y) {
                if (!target.row[y]) continue;
                for (int x = 0; x < kFieldWidth; ++x)
                    if (target.Has({x, y})) turns = std::min(turns, static_cast<int>(map.turns[x][y]));
            }
            const int tie = Abs(d.y - tieRow);
            // Then towards the middle: the goal's column for a defender, the
            // carrier's for an attacker (not off to a touchline).
            const int side = Abs(d.x - (defender ? kGoalCenterX : carrier.x));
            if (turns < bestTurns || (turns == bestTurns && (tie < bestTie || (tie == bestTie && side < bestSide)))) {
                bestTurns = turns;
                bestTie = tie;
                bestSide = side;
                bestCell = d;
            }
        }
        if (bestCell == p.pos) continue;
        const Action move = Action::Move(k, bestCell);
        ApplyUnchecked(t, move, Outcome::Certain());
        moves.push_back(move);
    }
    return moves;
}

}  // namespace bot
}  // namespace soccer
