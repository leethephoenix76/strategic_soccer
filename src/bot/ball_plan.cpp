#include "bot/ball_plan.h"

#include "bot/ball_race.h"
#include "bot/reach.h"
#include "bot/team_moves.h"
#include "engine/notation.h"
#include "engine/rules.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>

namespace soccer {
namespace bot {

namespace {

bool InBox(Cell a, Cell b, Cell p) {
    return (a.x - p.x) * (p.x - b.x) >= 0 && (a.y - p.y) * (p.y - b.y) >= 0;
}

unsigned TeamBits(int team) { return 0xFu << FirstPlayerOf(team); }

// Reach of player k's next own moves from the state as it is.
ReachMap NextReach(const GameState& s, int k) {
    const PlayerState& p = s.players[k];
    if (TeamOf(k) == s.teamToMove && p.moved) return ComputeReach(p.pos, p.Displacement());
    return ComputeReach(p.pos, p.Velocity());
}

// Own turns of player k up to moment m (a move in slot i is at 2i).
int TurnsBefore(const GameState& s, int k, int m) {
    int n = 0;
    for (int i = RaceFirstSlot(s, k); 2 * i <= m; i += 2) ++n;
    return n;
}

// Moves player k has made by half-turn h of the chains and routes (h = 0: the
// current turn of the team to move, 1: the other team's reply, ...; a move in
// half-turn h counts at h): our players h / 2 + 1 (h / 2 if he has already
// moved), the other team's (h + 1) / 2.
int HalfTurnMoves(const GameState& s, int k, int h) {
    if (TeamOf(k) != s.teamToMove) return (h + 1) / 2;
    return h / 2 + (s.players[k].moved ? 0 : 1);
}

bool CanStandInBox(const ReachMap& r, int turns, Cell a, Cell b) {
    for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
        for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y)
            if (InField(x, y) && r.Turns({x, y}) <= turns) return true;
    return false;
}

void Add(PlanParticipants& p, const GameState& s, int k, const std::string& why) {
    const unsigned bit = 1u << k;
    if (TeamOf(k) == s.teamToMove) p.own |= bit;
    else p.opponent |= bit;
    if (p.why[k].empty()) p.why[k] = why;
    else if (p.why[k].find(why) == std::string::npos) p.why[k] += ", " + why;
}

// Opponents (of the team to move in `s`, which has just ended its half-turn in
// the plan) who can tackle a carrier standing on one of `carriers` in their reply.
void ReplyTacklers(const GameState& s, const std::vector<Cell>& carriers, PlanParticipants& p) {
    const int them = Opponent(s.teamToMove);
    for (int k = FirstPlayerOf(them); k < FirstPlayerOf(them) + kPlayersPerTeam; ++k) {
        const PlayerState& pl = s.players[k];
        const ReachMap r = ComputeReach(pl.turnStart, pl.Velocity());
        bool found = false;
        for (Cell c : carriers) {
            for (int dx = -1; dx <= 1 && !found; ++dx)
                for (int dy = -1; dy <= 1 && !found; ++dy) {
                    const Cell q{c.x + dx, c.y + dy};
                    if (q == c || !InField(q) || r.Turns(q) > 1) continue;
                    const Cell disp{q.x - pl.turnStart.x, q.y - pl.turnStart.y};
                    if (AttackDirectionAllowed(q, c, pl.Velocity(), disp)) found = true;
                }
            if (found) {
                Add(p, s, k, "tackle " + CellName(c));
                break;
            }
        }
    }
}

// Interception or deflection candidates of the pass segment as the engine
// chooses them: players in the bounding box, except the passer and the player
// on the target.
void PassBox(const GameState& s, Cell from, Cell target, int passer, PlanParticipants& p) {
    for (int k = 0; k < kNumPlayers; ++k) {
        const Cell c = s.players[k].pos;
        if (k == passer || c == target || !InBox(from, target, c)) continue;
        Add(p, s, k, "pass box");
    }
}

// The race for a ball passed into space (`s`: right after the pass, the team
// that passed still to move).
void SpaceRace(const GameState& s, PlanParticipants& p) {
    const BallRaceResult race = SolveBallRace(s);
    int decided = kRaceNever;
    if (race.kind == BallRaceKind::Taken || race.kind == BallRaceKind::Restart) decided = race.moment;
    else if (race.kind == BallRaceKind::Contested) {
        decided = 0;
        for (int k = 0; k < kNumPlayers; ++k)
            if (race.contestants >> k & 1u) decided = std::max(decided, race.players[k].lower);
    }
    if (race.kind == BallRaceKind::NotFree) return;
    if (race.kind == BallRaceKind::Taken) Add(p, s, race.player, "takes " + CellName(race.cell));
    for (int k = 0; k < kNumPlayers; ++k) {
        if (race.kind == BallRaceKind::Contested && (race.contestants >> k & 1u)) Add(p, s, k, "race");
        const ReachMap r = NextReach(s, k);
        Cell previous = s.ball.pos;
        for (const BallWindow& w : race.windows) {
            if (w.arrival > decided) break;
            // Stepping onto the ball while it lies here.
            int deadline = w.openEnd ? decided : (w.lastMoveSlot >= 0 ? 2 * w.lastMoveSlot + 1 : w.arrival);
            if (deadline > decided) deadline = decided;
            if (deadline >= 0 && r.Turns(w.cell) <= TurnsBefore(s, k, deadline)) Add(p, s, k, "pickup " + CellName(w.cell));
            // Standing in the bounding box of the step that brings it here.
            if (w.arrival >= 0 && w.cell != previous &&
                CanStandInBox(r, TurnsBefore(s, k, w.arrival), previous, w.cell))
                Add(p, s, k, "step box " + CellName(w.cell));
            previous = w.cell;
        }
    }
}

// Outside players who may interfere by blocking.
void Closure(const GameState& s, PlanParticipants& p, const std::vector<int>& movers) {
    const int us = s.teamToMove;
    auto square = [](Cell centre, Cell c) { return ChebyshevDistance(centre, c) <= 1; };
    // Opponents of the reply, their centre of movement.
    for (int j = 0; j < kNumPlayers; ++j) {
        if (!(p.opponent >> j & 1u)) continue;
        const PlayerState& pj = s.players[j];
        const Cell centre{pj.turnStart.x + Sign(pj.Velocity().x), pj.turnStart.y + Sign(pj.Velocity().y)};
        for (int k = 0; k < kNumPlayers; ++k) {
            const unsigned bit = 1u << k;
            if (k == j || ((p.own | p.opponent) & bit)) continue;
            bool meets = false;
            if (TeamOf(k) == us) {
                const ReachMap r = NextReach(s, k);
                for (int dx = -1; dx <= 1 && !meets; ++dx)
                    for (int dy = -1; dy <= 1 && !meets; ++dy) {
                        const Cell c{centre.x + dx, centre.y + dy};
                        if (InField(c) && r.Turns(c) <= (s.players[k].moved ? 0 : 1)) meets = true;
                    }
            } else {
                meets = square(centre, s.players[k].pos);
            }
            if (meets) {
                p.closure |= bit;
                if (p.why[k].empty()) p.why[k] = "closure (blocks " + PlayerName(j) + ")";
            }
        }
    }
    // Our movers (holder, receiver) and outside players standing in their square.
    for (int m : movers) {
        const PlayerState& pm = s.players[m];
        if (pm.moved) continue;
        const Cell centre{pm.turnStart.x + Sign(pm.Velocity().x), pm.turnStart.y + Sign(pm.Velocity().y)};
        for (int k = 0; k < kNumPlayers; ++k) {
            const unsigned bit = 1u << k;
            if (k == m || ((p.own | p.opponent | p.closure) & bit)) continue;
            if (TeamOf(k) == us && square(centre, s.players[k].pos)) {
                p.closure |= bit;
                p.why[k] = "closure (stands by " + PlayerName(m) + ")";
            }
        }
    }
}

struct Lists {
    const GameState* s;
    GoalEpisode episode;
    std::map<unsigned, std::unique_ptr<TeamMovementList>> byFrozen;
    const TeamMovementList& Get(unsigned frozen) {
        auto it = byFrozen.find(frozen);
        if (it != byFrozen.end()) return *it->second;
        std::unique_ptr<TeamMovementList> list(new TeamMovementList(EnumerateTeamMovements(*s, episode, 0, {}, frozen)));
        return *byFrozen.emplace(frozen, std::move(list)).first->second;
    }
};

bool Matches(const GameState& st, const BallPlan& plan, int holder) {
    if (st.players[holder].pos != plan.from) return false;
    if (plan.kind == PlanKind::PassFeet) return st.players[plan.receiver].pos == plan.target;
    if (plan.kind == PlanKind::PassSpace) {
        const int first = FirstPlayerOf(st.teamToMove);
        for (int k = first; k < first + kPlayersPerTeam; ++k)
            if (st.players[k].pos == plan.target) return false;
    }
    return true;
}

long Count(const TeamMovementList& list, const BallPlan& plan, int holder, const GameState** representative) {
    long n = 0;
    int fewest = 1 << 20;
    for (const TeamMovementState& st : list.states) {
        if (st.episode.status != GoalEpisodeStatus::OurAttack && st.episode.status != GoalEpisodeStatus::TheirCounterattack)
            continue;
        if (!st.state.ball.IsFree() && st.state.ball.holder != holder) continue;
        if (!Matches(st.state, plan, holder)) continue;
        ++n;
        if (representative && st.count < fewest) {
            fewest = st.count;
            *representative = &st.state;
        }
    }
    return n;
}

void PlayOutcome(GameState& s, const Action& a) {
    const std::vector<Outcome> outcomes = OutcomesUnchecked(s, a);
    Outcome chosen = outcomes.front();
    for (const Outcome& o : outcomes)
        if (o.kind == OutcomeKind::BallThrough) chosen = o;
    ApplyUnchecked(s, a, chosen);
}

}  // namespace

std::string PlanName(const GameState& s, const BallPlan& p) {
    (void)s;
    switch (p.kind) {
    case PlanKind::Hold: return "hold " + CellName(p.from);
    case PlanKind::Shot: return "shot " + CellName(p.from);
    case PlanKind::PassFeet: return CellName(p.from) + ">" + CellName(p.target) + " " + PlayerName(p.receiver);
    case PlanKind::PassSpace: return CellName(p.from) + ">" + CellName(p.target) + " space";
    }
    return "?";
}

std::vector<PlanParticipants> AnalyzeBallPlans(const GameState& s, bool replies) {
    std::vector<PlanParticipants> result;
    if (s.phase != Phase::Play || s.ball.IsFree() || TeamOf(s.ball.holder) != s.teamToMove || s.ballPlayed)
        return result;
    const int us = s.teamToMove, them = Opponent(us);
    const int holder = s.ball.holder;
    Lists lists{&s, GoalEpisode{s.ball.attackTeam, GoalEpisodeStatus::OurAttack}, {}};
    const TeamMovementList& full = lists.Get(0);

    // Where the holder can play the ball from, and where each partner can stand.
    std::vector<Cell> froms;
    std::vector<Cell> partnerCells[kNumPlayers];
    for (const TeamMovementState& st : full.states) {
        if (st.state.ball.holder != holder) continue;
        const Cell h = st.state.players[holder].pos;
        if (std::find(froms.begin(), froms.end(), h) == froms.end()) froms.push_back(h);
        for (int k = FirstPlayerOf(us); k < FirstPlayerOf(us) + kPlayersPerTeam; ++k) {
            const Cell c = st.state.players[k].pos;
            if (k != holder && std::find(partnerCells[k].begin(), partnerCells[k].end(), c) == partnerCells[k].end())
                partnerCells[k].push_back(c);
        }
    }
    std::sort(froms.begin(), froms.end(), [](Cell a, Cell b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });

    std::vector<BallPlan> plans;
    for (Cell from : froms) {
        plans.push_back({PlanKind::Hold, from, {}, kNoPlayer});
        plans.push_back({PlanKind::Shot, from, {}, kNoPlayer});
        for (int y = from.y - kMaxPassDistance; y <= from.y + kMaxPassDistance; ++y)
            for (int x = from.x - kMaxPassDistance; x <= from.x + kMaxPassDistance; ++x) {
                const Cell d{x, y};
                if (!InField(d) || d == from) continue;
                // A pass onto an opponent's cell gives him the ball: not a plan.
                bool opponentThere = false;
                for (int k = FirstPlayerOf(them); k < FirstPlayerOf(them) + kPlayersPerTeam; ++k)
                    if (s.players[k].pos == d) opponentThere = true;
                if (opponentThere) continue;
                plans.push_back({PlanKind::PassSpace, from, d, kNoPlayer});
                for (int k = FirstPlayerOf(us); k < FirstPlayerOf(us) + kPlayersPerTeam; ++k)
                    if (k != holder && std::find(partnerCells[k].begin(), partnerCells[k].end(), d) != partnerCells[k].end())
                        plans.push_back({PlanKind::PassFeet, from, d, k});
            }
    }

    std::map<std::string, std::pair<long, long>> replyCache;
    for (const BallPlan& plan : plans) {
        PlanParticipants p;
        p.plan = plan;
        const GameState* rep = nullptr;
        p.ownFull = Count(full, plan, holder, &rep);
        if (p.ownFull == 0 || !rep) continue;   // no legal half-turn plays it
        GameState at = *rep;                    // the state at the moment of the ball action
        if (plan.kind != PlanKind::Hold && plan.kind != PlanKind::Shot && !CanPassTo(at, plan.target)) continue;
        Add(p, s, holder, "holder");
        std::vector<int> movers{holder};

        GameState after = at;   // after the ball action (the team that played still to move)
        std::vector<Cell> carriers;
        switch (plan.kind) {
        case PlanKind::Hold:
            carriers.push_back(plan.from);
            break;
        case PlanKind::Shot:
            for (int k = 0; k < kNumPlayers; ++k)
                if (k != holder && ShotBlockFactor(plan.from, us, at.players[k].pos) < 1) Add(p, s, k, "shot zone");
            break;
        case PlanKind::PassFeet: {
            Add(p, s, plan.receiver, "receiver");
            movers.push_back(plan.receiver);
            PassBox(at, plan.from, plan.target, holder, p);
            carriers.push_back(plan.target);
            if (!at.players[plan.receiver].moved) {   // he may run on with the ball
                const ReachMap r = ComputeReach(plan.target, at.players[plan.receiver].Velocity());
                for (int x = 0; x < kFieldWidth; ++x)
                    for (int y = kFirstRow; y <= kLastRow; ++y)
                        if (r.Turns({x, y}) == 1) carriers.push_back({x, y});
            }
            PlayOutcome(after, Action::Pass(plan.target));
            break;
        }
        case PlanKind::PassSpace:
            PassBox(at, plan.from, plan.target, holder, p);
            PlayOutcome(after, Action::Pass(plan.target));
            if (after.phase == Phase::Play && after.ball.IsFree()) SpaceRace(after, p);
            break;
        }
        if (!carriers.empty()) ReplyTacklers(at, carriers, p);
        Closure(at, p, movers);

        // Our half-turn with the outside players standing.
        const unsigned frozen = TeamBits(us) & ~p.own;
        p.ownPlan = Count(lists.Get(frozen), plan, holder, nullptr);

        if (replies && plan.kind != PlanKind::Shot) {
            GameState reply = after;
            PlayOutcome(reply, Action::EndTurn());
            if (reply.phase == Phase::Play && reply.teamToMove == them) {
                const unsigned theirFrozen = TeamBits(them) & ~p.opponent;
                const std::string key = FormatPosition(reply) + "|" + std::to_string(theirFrozen);
                auto it = replyCache.find(key);
                if (it == replyCache.end()) {
                    const GoalEpisode e{reply.ball.attackTeam, GoalEpisodeStatus::OurAttack};
                    const long all = static_cast<long>(EnumerateTeamMovements(reply, e).states.size());
                    const long part = static_cast<long>(EnumerateTeamMovements(reply, e, 0, {}, theirFrozen).states.size());
                    it = replyCache.emplace(key, std::make_pair(all, part)).first;
                }
                p.replyFull = it->second.first;
                p.replyPlan = it->second.second;
            }
        }
        result.push_back(p);
    }
    return result;
}

bool PlanOfActions(const GameState& start, const std::vector<Action>& actions, BallPlan* plan) {
    GameState s = start;
    if (s.ball.IsFree() || TeamOf(s.ball.holder) != s.teamToMove) return false;
    const int holder = s.ball.holder;
    for (const Action& a : actions) {
        if (a.type == ActionType::Pass) {
            const int k = PlayerAt(s, a.target);
            plan->from = s.players[holder].pos;
            plan->target = a.target;
            plan->receiver = k != kNoPlayer && TeamOf(k) == s.teamToMove ? k : kNoPlayer;
            plan->kind = plan->receiver == kNoPlayer ? PlanKind::PassSpace : PlanKind::PassFeet;
            return true;
        }
        if (a.type == ActionType::Shot) {
            plan->kind = PlanKind::Shot;
            plan->from = s.players[holder].pos;
            return true;
        }
        if (a.type == ActionType::EndTurn) break;
        if (a.type == ActionType::Move) ApplyUnchecked(s, a, Outcome::Certain());
    }
    plan->kind = PlanKind::Hold;
    plan->from = s.players[holder].pos;
    return true;
}


namespace {

struct Region {
    bool cell[kFieldWidth][kGridHeight] = {};
    bool any = false;
};

Region ReachRegion(const ReachMap& r, int turns) {
    Region g;
    for (int x = 0; x < kFieldWidth; ++x)
        for (int y = kFirstRow; y <= kLastRow; ++y)
            if (r.Turns({x, y}) <= turns) g.cell[x][y] = g.any = true;
    return g;
}

bool Meets(const Region& a, const Region& b) {
    for (int x = 0; x < kFieldWidth; ++x)
        for (int y = kFirstRow; y <= kLastRow; ++y)
            if (a.cell[x][y] && b.cell[x][y]) return true;
    return false;
}

Region Dilate(const Region& a) {
    Region g;
    for (int x = 0; x < kFieldWidth; ++x)
        for (int y = kFirstRow; y <= kLastRow; ++y)
            if (a.cell[x][y])
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy)
                        if (InField(x + dx, y + dy)) g.cell[x + dx][y + dy] = g.any = true;
    return g;
}

// Cells of the bounding boxes of passes from a cell of `from` to a cell of `to`
// in pass range; `possible` tells whether there is such a pass at all.
Region PassBoxes(const Region& from, const Region& to, bool* possible) {
    Region g;
    *possible = false;
    for (int ax = 0; ax < kFieldWidth; ++ax)
        for (int ay = kFirstRow; ay <= kLastRow; ++ay) {
            if (!from.cell[ax][ay]) continue;
            for (int bx = std::max(0, ax - kMaxPassDistance); bx <= std::min(kFieldWidth - 1, ax + kMaxPassDistance); ++bx)
                for (int by = std::max(kFirstRow, ay - kMaxPassDistance); by <= std::min(kLastRow, ay + kMaxPassDistance); ++by) {
                    if (!to.cell[bx][by] || (ax == bx && ay == by)) continue;
                    *possible = true;
                    for (int x = std::min(ax, bx); x <= std::max(ax, bx); ++x)
                        for (int y = std::min(ay, by); y <= std::max(ay, by); ++y) g.cell[x][y] = true;
                }
        }
    g.any = *possible;
    return g;
}

Region ShotZones(const Region& from, int team, double* best) {
    Region g;
    *best = 0;
    for (int ax = 0; ax < kFieldWidth; ++ax)
        for (int ay = kFirstRow; ay <= kLastRow; ++ay) {
            if (!from.cell[ax][ay]) continue;
            *best = std::max(*best, OpenShotProbability({ax, ay}, team));
            for (int x = 0; x < kFieldWidth; ++x)
                for (int y = kFirstRow; y <= kLastRow; ++y)
                    if (ShotBlockFactor({ax, ay}, team, {x, y}) < 1) g.cell[x][y] = g.any = true;
        }
    return g;
}

}  // namespace

std::vector<BallChain> AnalyzeBallChains(const GameState& s, int rounds) {
    std::vector<BallChain> result;
    if (s.phase != Phase::Play || s.ball.IsFree() || TeamOf(s.ball.holder) != s.teamToMove) return result;
    const int us = s.teamToMove, them = Opponent(us);
    // region[k][m]: where player k can be by moment m (0..2*rounds+1).
    const int moments = 2 * rounds + 2;
    std::vector<std::vector<Region>> region(kNumPlayers, std::vector<Region>(moments));
    for (int k = 0; k < kNumPlayers; ++k) {
        const ReachMap r = NextReach(s, k);
        for (int m = 0; m < moments; ++m) region[k][m] = ReachRegion(r, HalfTurnMoves(s, k, m));
    }
    auto meetsOpponents = [&](const Region& zone, int moment, unsigned* mask) {
        for (int j = FirstPlayerOf(them); j < FirstPlayerOf(them) + kPlayersPerTeam; ++j)
            if (Meets(region[j][moment], zone)) *mask |= 1u << j;
    };
    struct Node { int carrier; unsigned own, opponent; std::string text; };
    std::function<void(int, Node)> walk = [&](int round, Node n) {
        if (round == rounds) {
            result.push_back({n.text, n.own, n.opponent, rounds, false, 0});
            return;
        }
        const Region& carrierNow = region[n.carrier][2 * round];
        // A shot in this round ends the chain.
        {
            double best = 0;
            const Region zone = ShotZones(carrierNow, us, &best);
            Node m = n;
            meetsOpponents(zone, 2 * round, &m.opponent);
            result.push_back({m.text + (m.text.empty() ? "" : ", ") + PlayerName(n.carrier) + " shot", m.own, m.opponent,
                              round + 1, true, best});
        }
        // Keep the ball, or pass to a partner; then the reply's tackles on the carrier.
        for (int next = FirstPlayerOf(us); next < FirstPlayerOf(us) + kPlayersPerTeam; ++next) {
            Node m = n;
            if (next == n.carrier) {
                m.text += (m.text.empty() ? "" : ", ") + PlayerName(n.carrier) + " hold";
            } else {
                bool possible = false;
                const Region boxes = PassBoxes(carrierNow, region[next][2 * round], &possible);
                if (!possible) continue;
                meetsOpponents(boxes, 2 * round, &m.opponent);
                m.own |= 1u << next;
                m.carrier = next;
                m.text += (m.text.empty() ? "" : ", ") + std::string(">") + PlayerName(next);
            }
            meetsOpponents(Dilate(region[m.carrier][2 * round]), 2 * round + 1, &m.opponent);
            walk(round + 1, m);
        }
    };
    walk(0, Node{s.ball.holder, 1u << s.ball.holder, 0, ""});
    return result;
}

namespace {

constexpr int kVelocities = 25;
inline int StateOf(Cell c, Cell v) { return ((c.x * kGridHeight + c.y) * 5 + (v.x + 2)) * 5 + (v.y + 2); }
constexpr int kStates = kFieldWidth * kGridHeight * kVelocities;
inline Cell CellOfState(int i) { const int c = i / kVelocities; return {c / kGridHeight, c % kGridHeight}; }
inline Cell VelOfState(int i) { const int v = i % kVelocities; return {v / 5 - 2, v % 5 - 2}; }

// States (cell, velocity) of a lone player after exactly t moves, t = 0..maxT.
std::vector<std::vector<char>> StateReach(Cell cell, Cell vel, int maxT) {
    std::vector<std::vector<char>> r(maxT + 1, std::vector<char>(kStates, 0));
    r[0][StateOf(cell, vel)] = 1;
    for (int t = 1; t <= maxT; ++t)
        for (int i = 0; i < kStates; ++i) {
            if (!r[t - 1][i]) continue;
            const Cell c = CellOfState(i), v = VelOfState(i);
            const Cell centre{c.x + Sign(v.x), c.y + Sign(v.y)};
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    const Cell d{centre.x + dx, centre.y + dy};
                    if (InField(d)) r[t][StateOf(d, {d.x - c.x, d.y - c.y})] = 1;
                }
        }
    return r;
}

// orMap[set][x]: the set of masks {m | x : m in set} (sets of 4-bit masks as 16-bit words).
const std::vector<uint16_t>& OrMap() {
    static std::vector<uint16_t> map = [] {
        std::vector<uint16_t> m(65536 * 16);
        for (int set = 0; set < 65536; ++set)
            for (int x = 0; x < 16; ++x) {
                uint16_t out = 0;
                for (int k = 0; k < 16; ++k)
                    if (set >> k & 1) out |= static_cast<uint16_t>(1u << (k | x));
                m[set * 16 + x] = out;
            }
        return m;
    }();
    return map;
}

}  // namespace

std::vector<std::vector<BallRouteBest>> BallRoutes(const GameState& s, int rounds) {
    std::vector<std::vector<BallRouteBest>> best(rounds, std::vector<BallRouteBest>(16));
    if (s.phase != Phase::Play || s.ball.IsFree() || TeamOf(s.ball.holder) != s.teamToMove || rounds <= 0) return best;
    const int us = s.teamToMove, them = Opponent(us);
    const int firstOpp = FirstPlayerOf(them), firstUs = FirstPlayerOf(us);
    const std::vector<uint16_t>& orMap = OrMap();
    std::vector<std::vector<int>> oppTurns(kPlayersPerTeam);
    std::vector<ReachMap> oppReach;
    for (int i = 0; i < kPlayersPerTeam; ++i) {
        oppReach.push_back(NextReach(s, firstOpp + i));
        for (int m = 0; m <= 2 * rounds + 1; ++m) oppTurns[i].push_back(HalfTurnMoves(s, firstOpp + i, m));
    }
    auto canStand = [&](int i, Cell c, int moment) { return InField(c) && oppReach[i].Turns(c) <= oppTurns[i][moment]; };
    auto tacklers = [&](Cell c, int round) {
        int mask = 0;
        for (int i = 0; i < kPlayersPerTeam; ++i)
            for (int dx = -1; dx <= 1 && !(mask >> i & 1); ++dx)
                for (int dy = -1; dy <= 1 && !(mask >> i & 1); ++dy)
                    if ((dx || dy) && canStand(i, {c.x + dx, c.y + dy}, 2 * round + 1)) mask |= 1 << i;
        return mask;
    };
    auto boxers = [&](Cell a, Cell b, int round) {
        int mask = 0;
        for (int i = 0; i < kPlayersPerTeam; ++i)
            for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x) && !(mask >> i & 1); ++x)
                for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y) && !(mask >> i & 1); ++y)
                    if (Cell{x, y} != a && canStand(i, {x, y}, 2 * round)) mask |= 1 << i;
        return mask;
    };
    auto zoners = [&](Cell c, int round) {
        int mask = 0;
        for (int i = 0; i < kPlayersPerTeam; ++i)
            for (int x = 0; x < kFieldWidth && !(mask >> i & 1); ++x)
                for (int y = kFirstRow; y <= kLastRow && !(mask >> i & 1); ++y)
                    if (ShotBlockFactor(c, us, {x, y}) < 1 && canStand(i, {x, y}, 2 * round)) mask |= 1 << i;
        return mask;
    };
    std::vector<std::vector<std::vector<char>>> ours(kPlayersPerTeam);
    for (int i = 0; i < kPlayersPerTeam; ++i) {
        const PlayerState& p = s.players[firstUs + i];
        ours[i] = StateReach(p.turnStart, p.Velocity(), rounds);
    }
    const int cells = kFieldWidth * kGridHeight;
    const int layer = kPlayersPerTeam * kStates;
    // Parents for the route: start[r] (carrier, state) <- moved <- start; atCell <- moved;
    // start[r + 1] <- moved (hold) or atCell (pass).
    struct Par { int from = -1; int8_t mask = -1; int8_t pass = 0; };
    std::vector<std::vector<uint16_t>> start(rounds + 1, std::vector<uint16_t>(layer, 0));
    std::vector<std::vector<Par>> startPar(rounds + 1, std::vector<Par>(layer * 16));
    std::vector<std::vector<Par>> movedPar(rounds, std::vector<Par>(layer * 16));
    std::vector<std::vector<Par>> cellPar(rounds, std::vector<Par>(kPlayersPerTeam * cells * 16));
    struct ShotPar { int c = -1, cell = -1; int8_t mask = -1; };
    std::vector<std::vector<ShotPar>> shotPar(rounds, std::vector<ShotPar>(16));
    const int h = s.ball.holder - firstUs;
    start[0][h * kStates + StateOf(s.players[s.ball.holder].turnStart, s.players[s.ball.holder].Velocity())] = 1;
    std::vector<int> boxCache(cells * cells);
    auto mark = [&](uint16_t& target, std::vector<Par>& pars, int index, uint16_t add, int from, uint16_t fromSet, int x, int pass) {
        // Record a parent for every mask newly set in target: add = orMap[fromSet][x].
        const uint16_t fresh = add & ~target;
        if (!fresh) return;
        for (int m = 0; m < 16; ++m)
            if ((fromSet >> m & 1) && (fresh >> (m | x) & 1) && pars[index * 16 + (m | x)].from < 0)
                pars[index * 16 + (m | x)] = {from, static_cast<int8_t>(m), static_cast<int8_t>(pass)};
        target |= add;
    };
    for (int r = 0; r < rounds; ++r) {
        std::fill(boxCache.begin(), boxCache.end(), -1);
        std::vector<uint16_t> atCell(kPlayersPerTeam * cells, 0), moved(layer, 0);
        for (int c = 0; c < kPlayersPerTeam; ++c)
            for (int i = 0; i < kStates; ++i) {
                const uint16_t set = start[r][c * kStates + i];
                if (!set) continue;
                const Cell p = CellOfState(i), v = VelOfState(i);
                const Cell centre{p.x + Sign(v.x), p.y + Sign(v.y)};
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -1; dy <= 1; ++dy) {
                        const Cell d{centre.x + dx, centre.y + dy};
                        if (!InField(d)) continue;
                        const int j = StateOf(d, {d.x - p.x, d.y - p.y});
                        mark(moved[c * kStates + j], movedPar[r], c * kStates + j, set, c * kStates + i, set, 0, 0);
                        const int ci = c * cells + d.x * kGridHeight + d.y;
                        mark(atCell[ci], cellPar[r], ci, set, c * kStates + j, set, 0, 0);
                    }
            }
        std::vector<int> tackleMask(cells, -1);
        auto tackleAt = [&](Cell c) {
            int& t = tackleMask[c.x * kGridHeight + c.y];
            if (t < 0) t = tacklers(c, r);
            return t;
        };
        std::vector<uint16_t>& nxt = start[r + 1];
        for (int c = 0; c < kPlayersPerTeam; ++c)
            for (int i = 0; i < kStates; ++i) {
                const uint16_t set = moved[c * kStates + i];
                if (!set) continue;
                const int t = tackleAt(CellOfState(i));
                mark(nxt[c * kStates + i], startPar[r + 1], c * kStates + i, orMap[set * 16 + t], c * kStates + i, set, t, 0);
            }
        for (int c = 0; c < kPlayersPerTeam; ++c)
            for (int x = 0; x < kFieldWidth; ++x)
                for (int y = kFirstRow; y <= kLastRow; ++y) {
                    const int ci = c * cells + x * kGridHeight + y;
                    const uint16_t set = atCell[ci];
                    if (!set) continue;
                    const Cell from{x, y};
                    double open = OpenShotProbability(from, us);
                    if (open > 0) {
                        const int z = zoners(from, r);
                        // Each opponent who can get into the blocking zone by then
                        // stands on his worst cell for us; the others can not be there.
                        for (int i = 0; i < kPlayersPerTeam; ++i) {
                            if (!(z >> i & 1)) continue;
                            double worst = 1;
                            for (int bx = 0; bx < kFieldWidth; ++bx)
                                for (int by = kFirstRow; by <= kLastRow; ++by)
                                    if (Cell{bx, by} != from && canStand(i, {bx, by}, 2 * r))
                                        worst = std::min(worst, ShotBlockFactor(from, us, {bx, by}));
                            open *= worst;
                        }
                        for (int m = 0; m < 16; ++m)
                            if ((set >> m & 1) && open > best[r][m | z].shot) {
                                best[r][m | z] = {open, from, firstUs + c, r, 0u, ""};
                                shotPar[r][m | z] = {c, ci, static_cast<int8_t>(m)};
                            }
                    }
                    for (int q = 0; q < kPlayersPerTeam; ++q) {
                        if (q == c) continue;
                        for (int i = 0; i < kStates; ++i) {
                            if (!ours[q][r + 1][i]) continue;
                            const Cell d = CellOfState(i);
                            if (d == from || Abs(d.x - x) > kMaxPassDistance || Abs(d.y - y) > kMaxPassDistance) continue;
                            int& box = boxCache[(x * kGridHeight + y) * cells + d.x * kGridHeight + d.y];
                            if (box < 0) box = boxers(from, d, r);
                            const int add = box | tackleAt(d);
                            mark(nxt[q * kStates + i], startPar[r + 1], q * kStates + i, orMap[set * 16 + add], ci, set, add, 1);
                        }
                    }
                }
    }
    // The route of each best shot, read back through the parents.
    for (int r = 0; r < rounds; ++r)
        for (int m = 0; m < 16; ++m) {
            if (best[r][m].shot <= 0) continue;
            const ShotPar& sp = shotPar[r][m];
            std::vector<std::string> steps{PlayerName(firstUs + sp.c) + " shot " + CellName(best[r][m].cell)};
            unsigned own = 1u << (firstUs + sp.c);
            enum { AtCell, Moved, Start } kind = AtCell;
            int rr = r, index = sp.cell, mask = sp.mask;
            for (int guard = 0; guard < 100; ++guard) {
                if (kind == AtCell) {
                    index = cellPar[rr][index * 16 + mask].from;
                    kind = Moved;
                } else if (kind == Moved) {
                    const int st = movedPar[rr][index * 16 + mask].from;
                    const Cell from = CellOfState(st % kStates), to = CellOfState(index % kStates);
                    if (from != to) steps.push_back(PlayerName(firstUs + st / kStates) + " " + CellName(from) + "-" + CellName(to));
                    own |= 1u << (firstUs + st / kStates);
                    index = st;
                    kind = Start;
                } else {
                    if (rr == 0) break;
                    const Par& p = startPar[rr][index * 16 + mask];
                    const Cell here = CellOfState(index % kStates);
                    if (p.pass) {
                        const int fromCell = p.from % cells;
                        steps.push_back(">" + PlayerName(firstUs + index / kStates) + " " +
                                        CellName({fromCell / kGridHeight, fromCell % kGridHeight}) + ">" + CellName(here));
                        kind = AtCell;
                    } else {
                        steps.push_back("end of turn " + std::to_string(rr));
                        kind = Moved;
                    }
                    index = p.from;
                    mask = p.mask;
                    --rr;
                }
            }
            std::string text;
            for (auto it = steps.rbegin(); it != steps.rend(); ++it) text += (text.empty() ? "" : ", ") + *it;
            best[r][m].route = text;
            best[r][m].own = own;
        }
    // By round r keep the earlier rounds' bests; a set of opponents allows what a subset allows.
    for (int r = 1; r < rounds; ++r)
        for (int m = 0; m < 16; ++m)
            if (best[r - 1][m].shot > best[r][m].shot) best[r][m] = best[r - 1][m];
    for (int r = 0; r < rounds; ++r)
        for (int m = 0; m < 16; ++m)
            for (int sub = m;; sub = (sub - 1) & m) {
                if (best[r][sub].shot > best[r][m].shot) best[r][m] = best[r][sub];
                if (sub == 0) break;
            }
    return best;
}
}  // namespace bot
}  // namespace soccer
