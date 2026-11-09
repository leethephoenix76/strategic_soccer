// Engine tests. Portable C++17, no Windows dependencies: the engine is checked
// directly through its public API (src/engine/rules.h).
#include "engine/notation.h"
#include "engine/rules.h"

#include <cmath>

#include <cstdio>
#include <set>
#include <utility>

using namespace soccer;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checks;                                                     \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

// ------------------------------------------------------------------ helpers

// A position in play with every player parked far from the action.
static GameState Scenario(int teamToMove = kWhite) {
    GameState s = NewMatch(12345);
    s.phase = Phase::Play;
    s.teamToMove = teamToMove;
    for (int k = 0; k < kNumPlayers; ++k) {
        Cell c = TeamOf(k) == kWhite ? Cell{10, 9 + (k % 4) * 2} : Cell{0, 10 + (k % 4) * 2};
        s.players[k].pos = s.players[k].turnStart = s.players[k].prevTurnStart = c;
    }
    s.ball.holder = kNoPlayer;
    s.ball.pos = {5, 5};
    return s;
}

// Puts a player at `pos`; `prev` is his position a turn ago (sets his velocity).
static void Put(GameState& s, int k, Cell pos, Cell prev) {
    s.players[k].pos = s.players[k].turnStart = pos;
    s.players[k].prevTurnStart = prev;
    s.players[k].moved = s.players[k].attacked = false;
}
static void Put(GameState& s, int k, Cell pos) { Put(s, k, pos, pos); }

static void GiveBall(GameState& s, int k) {
    s.ball.holder = k;
    s.ball.attackTeam = TeamOf(k);
    s.ball.pos = s.players[k].pos;
    s.ball.vel = {};
}

static bool HasEvent(const EventLog& log, EventType t) {
    for (const Event& e : log)
        if (e.type == t) return true;
    return false;
}

static bool Contains(const std::vector<Cell>& cells, Cell c) {
    for (Cell x : cells)
        if (x == c) return true;
    return false;
}

static double SumProbability(const std::vector<Outcome>& outs) {
    double sum = 0;
    for (const Outcome& o : outs) sum += o.probability;
    return sum;
}

// ------------------------------------------------------------------ tests

static void TestDeadlock() {
    // Free ball in the corner, the three neighbouring cells taken by White.
    GameState s = Scenario();
    s.ball.pos = {0, 1};
    Put(s, 0, {1, 1}); Put(s, 1, {0, 2}); Put(s, 2, {1, 2});
    CHECK(BallSealedBy(s, kWhite));
    CHECK(!BallSealedBy(s, kBlack));

    // Free ball at the side line with three players around: can be poked diagonally.
    s = Scenario();
    s.ball.pos = {0, 5};
    Put(s, 0, {0, 4}); Put(s, 1, {0, 6}); Put(s, 2, {1, 5});
    CHECK(!BallSealedBy(s, kWhite));

    // A pocket in the corner walled off by four players.
    s = Scenario();
    s.ball.pos = {0, 1};
    Put(s, 0, {2, 1}); Put(s, 1, {2, 2}); Put(s, 2, {1, 3}); Put(s, 3, {0, 3});
    CHECK(BallSealedBy(s, kWhite));
    Put(s, 3, {9, 9});   // a gap in the wall
    CHECK(!BallSealedBy(s, kWhite));

    // Ball holder in the corner covered by three team-mates.
    s = Scenario();
    Put(s, 0, {0, 1}); Put(s, 1, {1, 1}); Put(s, 2, {0, 2}); Put(s, 3, {1, 2});
    GiveBall(s, 0);
    CHECK(BallSealedBy(s, kWhite));
    CHECK(!BallSealedBy(s, kBlack));

    // Ball holder at the side line: can be tackled from a diagonal.
    s = Scenario();
    Put(s, 0, {0, 5}); Put(s, 1, {0, 4}); Put(s, 2, {0, 6}); Put(s, 3, {1, 5});
    GiveBall(s, 0);
    CHECK(!BallSealedBy(s, kWhite));
}

static void TestDeadlockPenalty() {
    GameState s = Scenario(kWhite);
    s.ball.pos = {0, 1};
    Put(s, 0, {1, 1}); Put(s, 1, {0, 2}); Put(s, 2, {1, 2});
    EventLog log;
    Apply(s, Action::EndTurn(), &log);   // White: warning
    CHECK(HasEvent(log, EventType::SealWarning));
    CHECK(s.teamToMove == kBlack && s.phase == Phase::Play);
    log.clear();
    Apply(s, Action::EndTurn(), &log);   // Black
    CHECK(!HasEvent(log, EventType::SealPenalty));
    log.clear();
    Apply(s, Action::EndTurn(), &log);   // White still sealing: penalty
    CHECK(HasEvent(log, EventType::SealPenalty));
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kBlack);
}

static void TestPoke() {
    // Black at (1,6), ball at (0,5), the corridor (0,6)-(1,5) closed by White.
    GameState s = Scenario(kBlack);
    s.ball.pos = {0, 5};
    Put(s, 0, {0, 4}); Put(s, 1, {0, 6}); Put(s, 2, {1, 5}); Put(s, 4, {1, 6});
    CHECK(GetAttackKind(s, 4) == AttackKind::Poke);

    // One own player in the corridor: no poke.
    Put(s, 2, {5, 15}); Put(s, 5, {1, 5});
    CHECK(GetAttackKind(s, 4) == AttackKind::None);

    // Corridor open: step onto the ball instead.
    Put(s, 5, {7, 7});
    CHECK(GetAttackKind(s, 4) == AttackKind::None);

    // Orthogonal neighbour of a free ball: no poke.
    s = Scenario(kBlack);
    s.ball.pos = {0, 5};
    Put(s, 0, {0, 4}); Put(s, 1, {0, 6}); Put(s, 4, {1, 5});
    CHECK(GetAttackKind(s, 4) == AttackKind::None);
}

static void TestAttackDirection() {
    GameState s = Scenario(kBlack);
    s.ball.pos = {0, 5};
    Put(s, 0, {0, 4}); Put(s, 1, {0, 6}); Put(s, 2, {1, 5});
    Put(s, 4, {1, 6}, {0, 6});   // running right: the ball is behind-left
    CHECK(GetAttackKind(s, 4) == AttackKind::None);
    Put(s, 4, {1, 6}, {1, 7});   // running up: allowed
    CHECK(GetAttackKind(s, 4) == AttackKind::Poke);
    Put(s, 4, {1, 6}, {1, 5});   // running down: forbidden, also after moving in this turn
    CHECK(GetAttackKind(s, 4) == AttackKind::None);
    s.players[4].moved = true;
    CHECK(GetAttackKind(s, 4) == AttackKind::None);
    Put(s, 4, {1, 6});           // standing: any direction
    CHECK(GetAttackKind(s, 4) == AttackKind::Poke);

    // Tackle: White holder at (5,10), Black at (6,11).
    s = Scenario(kBlack);
    Put(s, 0, {5, 10});
    GiveBall(s, 0);
    Put(s, 4, {6, 11}, {6, 12});   // moving up, the holder up-left: allowed
    CHECK(GetAttackKind(s, 4) == AttackKind::Tackle);
    Put(s, 4, {6, 11}, {7, 11});   // moving left: allowed
    CHECK(GetAttackKind(s, 4) == AttackKind::Tackle);
    Put(s, 4, {6, 11}, {5, 11});   // moving right: forbidden
    CHECK(GetAttackKind(s, 4) == AttackKind::None);
    Put(s, 4, {6, 10}, {6, 11});   // holder to the left, moving up: perpendicular, allowed
    CHECK(GetAttackKind(s, 4) == AttackKind::Tackle);
    Put(s, 4, {7, 11});            // not adjacent
    CHECK(GetAttackKind(s, 4) == AttackKind::None);
    s.teamToMove = kWhite;
    Put(s, 4, {6, 11});            // not the team to move
    CHECK(GetAttackKind(s, 4) == AttackKind::None);

    // Moved in this turn into a cell from which the holder is behind.
    s = Scenario(kBlack);
    Put(s, 0, {5, 10});
    GiveBall(s, 0);
    Put(s, 4, {5, 11});
    s.players[4].pos = {6, 11};
    s.players[4].moved = true;      // moved right, the holder up-left
    CHECK(GetAttackKind(s, 4) == AttackKind::None);
}

static void TestAttackRestrictsMove() {
    // Black at (6,11) standing, tackles the holder at (5,10), then may not move away from him.
    GameState s = Scenario(kBlack);
    Put(s, 0, {5, 10});
    GiveBall(s, 0);
    Put(s, 4, {6, 11});
    CHECK(Apply(s, Action::Attack(4)) == ActionError::None);
    std::vector<Cell> dest = MoveDestinations(s, 4);
    bool bad = false, own = false;
    for (Cell c : dest) {
        int dx = c.x - 6, dy = c.y - 11;
        if (-dx - dy < 0) bad = true;
        if (dx == 0 && dy == 0) own = true;
    }
    CHECK(!bad);
    CHECK(own);

    // One attack per turn, even after moving to another adjacent cell.
    CHECK(Apply(s, Action::Attack(4)) == ActionError::CannotAttack);
}

static void TestKeeper() {
    // White passes into its own goal mouth: back pass, Black restarts.
    GameState s = Scenario(kWhite);
    Put(s, 0, {5, 15});
    GiveBall(s, 0);
    CHECK(PassReachesOwnKeeper(s, {5, 18}));
    CHECK(!PassReachesOwnKeeper(s, {5, 12}));
    CHECK(!PassReachesOwnKeeper(s, {5, 16}));   // stops before the line
    EventLog log;
    CHECK(Apply(s, Action::Pass({5, 19}), &log) == ActionError::None);
    CHECK(HasEvent(log, EventType::BackPassPenalty));
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kBlack);

    // A player standing in the mouth cell receives the ball instead.
    s = Scenario(kWhite);
    Put(s, 0, {5, 15});
    Put(s, 1, {5, 19});
    GiveBall(s, 0);
    CHECK(!PassReachesOwnKeeper(s, {5, 18}));

    // A ball rolling along the end line across the mouth (last touch: White pass)
    // is taken by Black's keeper; Black restarts.
    s = Scenario(kWhite);
    s.ball.pos = {1, 1};
    s.ball.vel = {5, 0};
    s.ball.dec = {-1, 0};
    s.ball.lastTouchTeam = kWhite;
    s.ball.lastTouchWasPass = true;
    s.ball.stepPhase = 0;
    log.clear();
    Apply(s, Action::EndTurn(), &log);
    CHECK(HasEvent(log, EventType::KeeperTookBall));
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kBlack);

}

static void TestBallOut() {
    GameState s = Scenario(kWhite);
    s.ball.pos = {8, 10};
    s.ball.vel = {4, 0};
    s.ball.dec = {-1, 0};
    s.ball.lastTouchTeam = kWhite;
    EventLog log;
    CHECK(Apply(s, Action::EndTurn(), &log) == ActionError::None);
    CHECK(HasEvent(log, EventType::BallOut));
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kBlack && s.halfTurns == 1);
    CHECK(s.ball.pos == (Cell{kCenterX, kCenterY}));

    // The team of the last touch, rather than the team ending the turn, loses it.
    s = Scenario(kWhite);
    s.ball.pos = {2, 2};
    s.ball.vel = {0, -3};
    s.ball.dec = {0, 1};
    s.ball.lastTouchTeam = kBlack;
    log.clear();
    CHECK(Apply(s, Action::EndTurn(), &log) == ActionError::None);
    CHECK(HasEvent(log, EventType::BallOut));
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kWhite);

    // A player on the last in-field cell receives the ball before it can go out.
    s = Scenario(kWhite);
    Put(s, 4, {10, 10});
    s.ball.pos = {8, 10};
    s.ball.vel = {4, 0};
    s.ball.dec = {-1, 0};
    log.clear();
    CHECK(Apply(s, Action::EndTurn(), &log) == ActionError::None);
    CHECK(!HasEvent(log, EventType::BallOut));
    CHECK(s.phase == Phase::Play && s.ball.holder == 4);

    // Reaching the boundary is legal; the next outward step is an out.
    s = Scenario(kWhite);
    Put(s, 0, {8, 10});
    GiveBall(s, 0);
    log.clear();
    CHECK(Apply(s, Action::Pass({10, 10}), &log) == ActionError::None);
    CHECK(!HasEvent(log, EventType::BallOut));
    CHECK(s.ball.pos == (Cell{10, 10}) && s.ball.IsFree());
    CHECK(Apply(s, Action::EndTurn()) == ActionError::None);
    log.clear();
    CHECK(Apply(s, Action::EndTurn(), &log) == ActionError::None);
    CHECK(HasEvent(log, EventType::BallOut));
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kBlack && s.halfTurns == 2);

    // An outward diagonal from an edge cell leaves immediately. A player on
    // the neighbouring edge cell is not on the ball's in-field path.
    s = Scenario(kWhite);
    Put(s, 4, {3, 1});
    s.ball.pos = {2, 1};
    s.ball.vel = {1, -1};
    s.ball.dec = {-1, 1};
    s.ball.lastTouchTeam = kBlack;
    const std::vector<Cell> edgePath = PredictBallPath(s);
    CHECK(edgePath.size() == 1 && edgePath[0] == (Cell{2, 1}));
    const std::vector<Outcome> edgeOutcomes = Outcomes(s, Action::EndTurn());
    CHECK(edgeOutcomes.size() == 1 && edgeOutcomes[0].kind == OutcomeKind::BallThrough);
    log.clear();
    CHECK(Apply(s, Action::EndTurn(), &log) == ActionError::None);
    CHECK(HasEvent(log, EventType::BallOut));
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kWhite);

    // Arriving at that edge cell from inside the field still allows pickup.
    s = Scenario(kWhite);
    Put(s, 4, {3, 1});
    s.ball.pos = {2, 2};
    s.ball.vel = {1, -2};
    s.ball.dec = {-1, 1};
    log.clear();
    CHECK(Apply(s, Action::EndTurn(), &log) == ActionError::None);
    CHECK(!HasEvent(log, EventType::BallOut));
    CHECK(s.phase == Phase::Play && s.ball.holder == 4);

    // A free goal mouth takes precedence over the end-line out.
    s = Scenario(kWhite);
    s.ball.pos = {5, 3};
    s.ball.vel = {0, -4};
    s.ball.dec = {0, 1};
    log.clear();
    CHECK(Apply(s, Action::EndTurn(), &log) == ActionError::None);
    CHECK(HasEvent(log, EventType::KeeperTookBall));
    CHECK(!HasEvent(log, EventType::BallOut));
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kBlack);

    // The final half-turn finishes the match, even when the ball goes out.
    s = Scenario(kWhite);
    s.halfTurns = kHalfTurnsPerMatch - 1;
    s.ball.pos = {8, 10};
    s.ball.vel = {4, 0};
    s.ball.dec = {-1, 0};
    log.clear();
    CHECK(Apply(s, Action::EndTurn(), &log) == ActionError::None);
    CHECK(HasEvent(log, EventType::BallOut) && HasEvent(log, EventType::MatchOver));
    CHECK(s.phase == Phase::Finished && s.halfTurns == kHalfTurnsPerMatch);
}

static void TestTurnFlow() {
    GameState s = NewMatch(1);
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kWhite);
    CHECK(Apply(s, Action::Place(0, {5, 10})) == ActionError::None);   // on the ball
    CHECK(Apply(s, Action::Place(1, {5, 5})) == ActionError::OppositeHalf);
    CHECK(Apply(s, Action::Place(4, {5, 5})) == ActionError::NotYourPlayer);
    CHECK(Apply(s, Action::Place(0, {5, 11})) == ActionError::AlreadyMoved);
    EventLog log;
    Apply(s, Action::EndTurn(), &log);
    CHECK(s.teamToMove == kBlack && s.ball.holder == 0 && HasEvent(log, EventType::LineupStarted));
    log.clear();
    Apply(s, Action::EndTurn(), &log);
    CHECK(s.phase == Phase::Play && s.teamToMove == kWhite && HasEvent(log, EventType::TurnStarted));

    // Dribbling: the ball follows the holder.
    std::vector<Cell> dest = MoveDestinations(s, 0);
    CHECK(Contains(dest, {6, 9}));
    CHECK(Apply(s, Action::Move(0, {6, 9})) == ActionError::None);
    CHECK(s.ball.pos == (Cell{6, 9}));
    CHECK(Apply(s, Action::Move(0, {6, 8})) == ActionError::AlreadyMoved);

    // One ball action per turn.
    CHECK(Apply(s, Action::Pass({6, 12})) == ActionError::None);
    CHECK(Apply(s, Action::Shot()) != ActionError::None);
}

static void TestMatchLength() {
    GameState s = Scenario(kWhite);
    Put(s, 0, {5, 10});
    GiveBall(s, 0);
    EventLog log;
    for (int i = 0; i < kHalfTurnsPerHalf; ++i) Apply(s, Action::EndTurn(), &log);
    CHECK(HasEvent(log, EventType::HalfTime));
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kBlack);
    s.phase = Phase::Play;
    log.clear();
    for (int i = 0; i < kHalfTurnsPerHalf; ++i) Apply(s, Action::EndTurn(), &log);
    CHECK(HasEvent(log, EventType::MatchOver));
    CHECK(s.phase == Phase::Finished);
    CHECK(LegalActions(s).empty());
}

static void TestDeterminism() {
    // Same seed, same actions -> same result (needed for replays and bots).
    auto play = [](uint64_t seed) {
        GameState s = NewMatch(seed);
        for (int i = 0; i < 400 && s.phase != Phase::Finished; ++i) {
            std::vector<Action> acts = LegalActions(s);
            Apply(s, acts[s.rng.Index(static_cast<int>(acts.size()))]);
        }
        return std::make_pair(s.score[0] * 1000 + s.score[1], s.halfTurns * 100 + s.ball.pos.x * 21 + s.ball.pos.y);
    };
    CHECK(play(7) == play(7));
}

static void TestRandomPlayInvariants() {
    // Random legal actions never break the basic invariants.
    for (uint64_t seed = 1; seed <= 200; ++seed) {
        GameState s = NewMatch(seed);
        for (int i = 0; i < 1500 && s.phase != Phase::Finished; ++i) {
            std::vector<Action> acts = LegalActions(s);
            if (acts.empty()) break;
            const Action& a = acts[s.rng.Index(static_cast<int>(acts.size()))];
            if (std::fabs(SumProbability(Outcomes(s, a)) - 1) > 1e-9) {
                CHECK(!"outcome probabilities do not sum to 1");
                break;
            }
            if (Apply(s, a) != ActionError::None) {
                CHECK(!"a legal action was rejected");
                break;
            }
            std::set<std::pair<int, int>> cells;
            bool ok = true;
            for (int k = 0; k < kNumPlayers; ++k) {
                Cell c = s.players[k].pos;
                ok = ok && InField(c) && cells.insert({c.x, c.y}).second;
            }
            ok = ok && InField(s.ball.pos);
            if (!s.ball.IsFree()) ok = ok && s.players[s.ball.holder].pos == s.ball.pos;
            else ok = ok && (PlayerAt(s, s.ball.pos) == kNoPlayer || s.phase == Phase::Lineup);
            if (!ok) {
                CHECK(!"invariant broken");
                break;
            }
        }
    }
}

static void TestOutcomes() {
    // A pass along a column past an opponent standing on the line and a team-mate next to it.
    GameState s = Scenario(kWhite);
    Put(s, 0, {5, 15});
    GiveBall(s, 0);
    Put(s, 4, {5, 13});   // on the line: p = 0.6
    Put(s, 1, {6, 11});   // off the line, outside the bounding box: not involved
    Put(s, 2, {5, 11});   // a team-mate on the line further on
    std::vector<Outcome> outs = Outcomes(s, Action::Pass({5, 10}));
    CHECK(std::fabs(SumProbability(outs) - 1) < 1e-12);
    double intercept4 = 0, intercept1 = 0, intercept2 = 0, through = 0;
    for (const Outcome& o : outs) {
        if (o.kind == OutcomeKind::Intercepted && o.player == 4) intercept4 += o.probability;
        if (o.kind == OutcomeKind::Intercepted && o.player == 1) intercept1 += 1;
        if (o.kind == OutcomeKind::Intercepted && o.player == 2) intercept2 += o.probability;
        if (o.kind == OutcomeKind::BallThrough) through += o.probability;
    }
    CHECK(std::fabs(intercept4 - 0.36) < 1e-12);
    CHECK(intercept1 == 0);
    CHECK(std::fabs(intercept2 - 0.4 * 0.36) < 1e-12);
    CHECK(std::fabs(through - 0.4 * 0.4) < 1e-12);
    CHECK(std::fabs(PassThroughProbability(s, {5, 10}) - through) < 1e-12);
    CHECK(std::fabs(PassThroughProbability(s, {5, 11}) - 0.4) < 1e-12);
    CHECK(PassThroughProbability(s, {5, 15}) == 0);

    // Sampled outcomes follow the probabilities.
    const int n = 20000;
    int hits4 = 0, hitsThrough = 0;
    for (int i = 0; i < n; ++i) {
        GameState t = s;
        t.rng.Seed(1000 + i);
        Outcome o;
        Apply(t, Action::Pass({5, 10}), nullptr, &o);
        if (o.kind == OutcomeKind::Intercepted && o.player == 4) ++hits4;
        if (o.kind == OutcomeKind::BallThrough) ++hitsThrough;
    }
    auto close = [n](int hits, double p) { return std::fabs(hits - n * p) < 5 * std::sqrt(n * p * (1 - p)) + 1; };
    CHECK(close(hits4, intercept4));
    CHECK(close(hitsThrough, through));

    // A chosen outcome is applied exactly; an impossible one is rejected.
    GameState t = s;
    Outcome o;
    o.kind = OutcomeKind::Intercepted;
    o.player = 4;
    CHECK(ApplyOutcome(t, Action::Pass({5, 10}), o) == ActionError::None);
    CHECK(t.ball.holder == 4);
    t = s;
    o.player = 1;   // not on the way
    CHECK(ApplyOutcome(t, Action::Pass({5, 10}), o) == ActionError::BadOutcome);
    CHECK(t.ball.holder == 0);

    // Tackle from the front: 40% won, 20% knocked away, 40% kept.
    s = Scenario(kBlack);
    Put(s, 0, {5, 10});
    GiveBall(s, 0);
    Put(s, 4, {5, 9});
    outs = Outcomes(s, Action::Attack(4));
    CHECK(std::fabs(SumProbability(outs) - 1) < 1e-12);
    CHECK(outs.front().kind == OutcomeKind::Won && std::fabs(outs.front().probability - 0.4) < 1e-12);
    CHECK(outs.back().kind == OutcomeKind::Kept && std::fabs(outs.back().probability - 0.4) < 1e-12);

    // Shot: goal p, caught (1-p)^2, parried p(1-p) into 15 cells.
    s = Scenario(kWhite);
    Put(s, 0, {5, 3});
    GiveBall(s, 0);
    outs = Outcomes(s, Action::Shot());
    CHECK(std::fabs(SumProbability(outs) - 1) < 1e-12);
    CHECK(outs.size() == 17 && std::fabs(outs[0].probability - ShotProbability(s, 0)) < 1e-12);

    // Moves are certain; End Turn is random only when the free ball makes a step.
    CHECK(Outcomes(s, Action::Move(0, {5, 2})).size() == 1);
    CHECK(Outcomes(s, Action::Move(0, {9, 9})).empty());   // not valid
}

static void TestNotation() {
    Cell c;
    CHECK(CellName({5, 10}) == "f10");
    CHECK(CellName({0, 19}) == "a1" && CellName({10, 1}) == "k19");
    CHECK(ParseCell("a19", &c) && c == (Cell{0, 1}));
    CHECK(!ParseCell("l5", &c) && !ParseCell("a0", &c) && !ParseCell("a20", &c) && !ParseCell("f", &c));
    int k;
    CHECK(PlayerName(5) == "B2" && ParsePlayer("W4", &k) && k == 3 && !ParsePlayer("W5", &k));

    // Actions and outcomes survive a round trip.
    GameState s = Scenario(kWhite);
    Put(s, 0, {5, 10});
    GiveBall(s, 0);
    Put(s, 4, {5, 9});
    for (const Action& a : LegalActions(s)) {
        if (a.type == ActionType::EndTurn) continue;
        Action b;
        CHECK(ParseAction(s, FormatAction(s, a), &b));
        CHECK(b.type == a.type && b.player == a.player && b.target == a.target);
        for (const Outcome& o : Outcomes(s, a)) {
            if (o.kind == OutcomeKind::Certain) continue;
            Outcome q;
            CHECK(ParseOutcome(FormatOutcome(o), &q) && SameOutcome(o, q));
        }
    }

    // Positions survive a round trip.
    GameState p;
    std::string error;
    const std::string text = "b 37 1-0 f10/f11 e12 d14/d16 h12 f8 c7/b6 g6 j3 @e7,+1,-2,1 t:Wp";
    CHECK(ParsePosition(text, &p, &error));
    CHECK(FormatPosition(p) == text);
    CHECK(p.teamToMove == kBlack && p.halfTurns == 37 && p.score[0] == 1);
    CHECK(p.players[0].pos == (Cell{5, 10}) && p.players[0].Velocity() == (Cell{0, 1}));
    CHECK(p.ball.vel == (Cell{1, 2}) && p.ball.stepPhase == 1 && p.ball.lastTouchWasPass);
    CHECK(ParsePosition("w 0 0-0 f10 e12 d14 h12 f8 c7 g6 j3 @W1", &p) && p.ball.holder == 0);
    CHECK(!ParsePosition("w 0 0-0 f10 f10 d14 h12 f8 c7 g6 j3 @W1", &p));   // two players in a cell
}

// Plays a random game with several actions per turn and records it.
static GameRecord RandomGame(uint64_t seed, int maxActions) {
    GameRecord record;
    record.SetTag("Seed", std::to_string(seed));
    GameState s = NewMatch(seed);
    Rng chooser(seed * 7919);
    for (int i = 0; i < maxActions && s.phase != Phase::Finished; ++i) {
        std::vector<Action> acts = LegalActions(s);
        Action a = acts.back();   // End Turn
        if (acts.size() > 1 && chooser.Uniform() < 0.8) a = acts[chooser.Index(static_cast<int>(acts.size()) - 1)];
        RecordedAction r;
        r.action = a;
        Apply(s, a, nullptr, &r.outcome);
        record.actions.push_back(r);
    }
    return record;
}

// A position inside a turn (07.11.2026): who has moved and where, attacks,
// the ball played, a line-up, a free ball under a player - written and read
// back identically, with the same legal actions, along random play.
static void TestPositionInsideTurn() {
    int inside = 0, under = 0, lineups = 0, failures = 0;
    for (uint64_t seed = 1; seed <= 40; ++seed) {
        GameState s = NewMatch(seed);
        uint64_t r = seed * 2654435761u + 1;
        for (int step = 0; step < 2000 && s.phase != Phase::Finished; ++step) {
            const std::vector<Action> actions = LegalActions(s);
            if (actions.empty()) break;
            r = r * 6364136223846793005ull + 1442695040888963407ull;
            const Action a = actions[(r >> 33) % actions.size()];
            const std::vector<Outcome> outcomes = OutcomesUnchecked(s, a);
            r = r * 6364136223846793005ull + 1442695040888963407ull;
            ApplyUnchecked(s, a, outcomes[(r >> 33) % outcomes.size()]);
            if (s.phase != Phase::Play && s.phase != Phase::Lineup) continue;
            const std::string text = FormatPosition(s);
            GameState back;
            if (!ParsePosition(text, &back) || FormatPosition(back) != text ||
                LegalActions(back).size() != LegalActions(s).size()) {
                ++failures;
                continue;
            }
            if (text.find(" m:") != std::string::npos) ++inside;
            if (s.phase == Phase::Lineup) ++lineups;
            if (s.ball.IsFree() && PlayerAt(s, s.ball.pos) != kNoPlayer) ++under;
        }
    }
    CHECK(failures == 0);
    CHECK(inside > 1000);
    CHECK(lineups > 0);
    GameState s;
    // A free ball under a player is read (07.11.2026).
    CHECK(ParsePosition("b 46 0-0 e11 e10/d10 i8 d9/c9 j10/i10 h11/i12 e12/d11 c13/b12 @j10,+1,0,1 t:Bp", &s));
    // Inside a turn: W1 moved to g12, the ball played.
    CHECK(ParsePosition("w 10 0-0 f11 d11 h11 f7 f12 i12 c12 f16 @W2 t:W m:W1g12 f:p", &s));
    CHECK(s.players[0].pos == (Cell{6, 8}) && s.players[0].turnStart == (Cell{5, 9}) && s.players[0].moved);
    CHECK(s.ballPlayed && !s.committed && s.ball.pos == s.players[1].pos);
    CHECK(!ParsePosition("w 10 0-0 f11 d11 h11 f7 f12 i12 c12 f16 @W2 t:W m:W1f12,W2f12", &s));   // two players in f12
    (void)under;
}

static void TestGameRecord() {
    for (uint64_t seed = 1; seed <= 20; ++seed) {
        GameRecord record = RandomGame(seed, 3000);
        GameState end1, end2;
        CHECK(ReplayRecord(record, &end1));
        std::string text = WriteRecord(record);
        GameRecord read;
        std::string error;
        bool ok = ReadRecord(text, &read, &error);
        CHECK(ok);
        if (!ok) {
            std::printf("  %s\n", error.c_str());
            continue;
        }
        CHECK(ReplayRecord(read, &end2));
        CHECK(FormatPosition(end1) == FormatPosition(end2) || end1.phase != Phase::Play);
        CHECK(end1.score[0] == end2.score[0] && end1.score[1] == end2.score[1] && end1.halfTurns == end2.halfTurns);
        CHECK(WriteRecord(read) == text);
    }

    // Outcomes may be omitted: they are sampled while reading.
    GameRecord read;
    std::string error;
    CHECK(ReadRecord("[Seed \"5\"]\nL. W 1f10\nL. B --\n1. W 1f9 >f5\n2. B --\n", &read, &error));
    CHECK(read.actions.size() == 7 && read.actions[4].action.type == ActionType::Pass);
    CHECK(!ReadRecord("L. B --\n", &read, &error));   // White lines up first
}

static void TestShotEndsTurn() {
    auto shooter = [](int halfTurns) {
        GameState s = Scenario(kWhite);
        s.halfTurns = halfTurns;
        Put(s, 0, {5, 3});
        GiveBall(s, 0);
        return s;
    };
    Outcome o;
    EventLog log;

    // Parried: the shot ends the turn, the opponent moves next, the clock runs.
    GameState s = shooter(20);
    o.kind = OutcomeKind::Parried;
    o.cell = {3, 3};
    CHECK(ApplyOutcome(s, Action::Shot(), o) == ActionError::None);
    CHECK(s.phase == Phase::Play && s.teamToMove == kBlack && s.halfTurns == 21);
    CHECK(s.ball.IsFree() && s.ball.pos == (Cell{3, 3}));
    CHECK(!s.players[1].moved && s.players[0].turnStart == (Cell{5, 3}));

    // Caught: restart, the keeper's team lines up first; the turn counts.
    s = shooter(20);
    o = Outcome{};
    o.kind = OutcomeKind::Caught;
    CHECK(ApplyOutcome(s, Action::Shot(), o) == ActionError::None);
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kBlack && s.halfTurns == 21);

    // Goal.
    s = shooter(20);
    o.kind = OutcomeKind::Goal;
    CHECK(ApplyOutcome(s, Action::Shot(), o) == ActionError::None);
    CHECK(s.score[kWhite] == 1 && s.phase == Phase::Lineup && s.teamToMove == kBlack && s.halfTurns == 21);

    // A goal in the last turn of the half: half-time, Black lines up first.
    s = shooter(kHalfTurnsPerHalf - 1);
    s.teamToMove = kWhite;
    log.clear();
    CHECK(ApplyOutcome(s, Action::Shot(), o, &log) == ActionError::None);
    CHECK(HasEvent(log, EventType::HalfTime) && s.halfTurns == kHalfTurnsPerHalf && s.teamToMove == kBlack);

    // A save in the last turn of the match ends it.
    s = shooter(kHalfTurnsPerMatch - 1);
    o.kind = OutcomeKind::Caught;
    log.clear();
    CHECK(ApplyOutcome(s, Action::Shot(), o, &log) == ActionError::None);
    CHECK(HasEvent(log, EventType::MatchOver) && s.phase == Phase::Finished);

    // The ball knocked into the goal mouth by a tackle: the keeper takes it, the turn counts.
    s = Scenario(kBlack);
    s.halfTurns = 30;
    Put(s, 0, {5, 18});
    GiveBall(s, 0);
    Put(s, 4, {5, 17});
    o = Outcome{};
    o.kind = OutcomeKind::KnockedAway;
    o.cell = {5, 19};
    log.clear();
    CHECK(ApplyOutcome(s, Action::Attack(4), o, &log) == ActionError::None);
    CHECK(HasEvent(log, EventType::KeeperTookBall));
    CHECK(s.phase == Phase::Lineup && s.teamToMove == kWhite && s.halfTurns == 31);
}

// The formulas for hypothetical cells agree with the actions.
static void TestFormulas() {
    Rng r(77);
    for (int i = 0; i < 300; ++i) {
        GameState s = Scenario(kBlack);
        Cell h{1 + r.Index(9), 2 + r.Index(17)};
        Put(s, 0, h);
        GiveBall(s, 0);
        Cell a{h.x - 1 + r.Index(3), h.y - 1 + r.Index(3)};
        if (a == h) continue;
        Cell prev{a.x - 1 + r.Index(3), a.y - 1 + r.Index(3)};
        Put(s, 4, a, InField(prev) ? prev : a);
        const bool allowed = AttackDirectionAllowed(a, h, s.players[4].Velocity(), s.players[4].Displacement());
        CHECK(allowed == (GetAttackKind(s, 4) == AttackKind::Tackle));
        if (!allowed) continue;
        std::vector<Outcome> outs = Outcomes(s, Action::Attack(4));
        double won = 0, away = 0;
        for (const Outcome& o : outs) {
            if (o.kind == OutcomeKind::Won) won += o.probability;
            if (o.kind == OutcomeKind::KnockedAway) away += o.probability;
        }
        CHECK(std::fabs(won - TackleChance(h, a, kBlack)) < 1e-12);
        CHECK(std::fabs(away - kKnockAwayChance) < 1e-12);
    }
    // A shot without blockers in the zone.
    GameState s = Scenario(kWhite);
    Put(s, 1, {5, 6});
    GiveBall(s, 1);
    CHECK(std::fabs(ShotProbability(s, 1) - OpenShotProbability({5, 6}, kWhite)) < 1e-12);
    Put(s, 5, {5, 3});
    CHECK(std::fabs(ShotProbability(s, 1) - OpenShotProbability({5, 6}, kWhite) * BlockFactor(0)) < 1e-12);
    CHECK(std::fabs(InterceptionWeight(0) - 0.6) < 1e-12);

    // Parried shots land in the parry cells, each equally likely.
    for (int team : {kWhite, kBlack}) {
        GameState t = Scenario(team);
        const int shooter = FirstPlayerOf(team);
        Put(t, shooter, {5, team == kWhite ? 5 : 15});
        GiveBall(t, shooter);
        Cell cells[kParryCells];
        ParryCells(team, cells);
        int parried = 0;
        double first = -1;
        for (const Outcome& o : Outcomes(t, Action::Shot())) {
            if (o.kind != OutcomeKind::Parried) continue;
            ++parried;
            CHECK(Contains(std::vector<Cell>(cells, cells + kParryCells), o.cell));
            if (first < 0) first = o.probability;
            CHECK(std::fabs(o.probability - first) < 1e-15);
        }
        CHECK(parried == kParryCells);
    }
}

static void TestAttackTeamAcrossFreeBall() {
    GameState s = Scenario(kBlack);
    Put(s, 0, {5, 10});
    Put(s, 4, {5, 11});
    GiveBall(s, 0);
    bool checked = false;
    for (const Outcome& o : Outcomes(s, Action::Attack(4))) {
        if (o.kind != OutcomeKind::KnockedAway) continue;
        GameState t = s;
        CHECK(ApplyOutcome(t, Action::Attack(4), o) == ActionError::None);
        if (!t.ball.IsFree()) continue;
        CHECK(t.ball.lastTouchTeam == kBlack);
        CHECK(t.ball.attackTeam == kWhite);
        GameState read;
        CHECK(ParsePosition(FormatPosition(t), &read));
        CHECK(read.ball.attackTeam == kWhite);
        CHECK(read.ball.lastTouchTeam == kBlack);
        checked = true;
        break;
    }
    CHECK(checked);
}

int main() {
    TestDeadlock();
    TestDeadlockPenalty();
    TestPoke();
    TestAttackDirection();
    TestAttackRestrictsMove();
    TestKeeper();
    TestBallOut();
    TestTurnFlow();
    TestMatchLength();
    TestDeterminism();
    TestRandomPlayInvariants();
    TestOutcomes();
    TestNotation();
    TestPositionInsideTurn();
    TestGameRecord();
    TestShotEndsTurn();
    TestFormulas();
    TestAttackTeamAcrossFreeBall();
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "ALL OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
