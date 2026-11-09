// Bot tests: the goal search (exact and selective), its parts and the bot.
#include "bot/bot.h"
#include "bot/bot_registry.h"
#include "bot/episode.h"
#include "bot/episode_value.h"
#include "bot/ball_race.h"
#include "bot/hash.h"
#include "bot/goal_bounds.h"
#include "bot/goal_episode.h"
#include "bot/goal_dfs_search.h"
#include "bot/team_moves.h"
#include "bot/pass_pruning.h"
#include "bot/race.h"
#include "bot/reach.h"
#include "bot/value.h"
#include "engine/notation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace soccer;

constexpr int kUnreachableTest = bot::kUnreachable;

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

static GameState Position(const char* text) {
    GameState s;
    std::string error;
    if (!ParsePosition(text, &s, &error)) std::printf("bad position %s: %s\n", text, error.c_str());
    return s;
}

static bool HasEvent(const EventLog& events, EventType type) {
    for (const Event& event : events)
        if (event.type == type) return true;
    return false;
}

static void TestHash() {
    GameState a = Position("w 10 0-0 f10/f11 c8 i8 f4 f12 d13 h13 f15 @W1");
    GameState b = a;
    CHECK(bot::PositionHash(a) == bot::PositionHash(b));
    b.rng.Seed(99);   // the generator does not matter
    CHECK(bot::PositionHash(a) == bot::PositionHash(b));
    Apply(b, Action::Move(1, {2, 13}));
    CHECK(bot::PositionHash(a) != bot::PositionHash(b));
}

static bot::BotOptions TestBotOptions(const char* preset) {
    // A deterministic budget: one team turn deep (the beam) or a fixed number
    // of simulations (the tree), no clock.
    bot::BotOptions o = bot::FindBotPreset(preset)->options;
    o.timeLimit = 0;
    o.threads = 1;
    if (o.search.selectiveTree) o.search.selectiveTreeSimulations = 40;
    else o.search.maxTurnDepth = 1;
    return o;
}

static void TestBotPlays() {
    // Line-up then a few half-turns: every action is legal.
    for (const char* preset : {"goal-sel-run-fb-b8-t120", "goal-sel-tree-fb-b8-t120"}) {
        GameState s = NewMatch(5);
        bot::Bot white(TestBotOptions(preset)), black(TestBotOptions(preset));
        int actions = 0;
        while (s.phase != Phase::Finished && s.halfTurns < 6 && actions < 400) {
            bot::Bot& b = s.teamToMove == kWhite ? white : black;
            ActionError err = Apply(s, b.NextAction(s));
            CHECK(err == ActionError::None);
            if (err != ActionError::None) break;
            ++actions;
        }
        CHECK(s.halfTurns >= 6);
    }
}

static void TestReach() {
    // In an empty field the cells reachable in one turn are exactly the destinations.
    Rng r(11);
    for (int i = 0; i < 300; ++i) {
        // Everybody else far away on the first and last rows.
        GameState s = Position("w 10 0-0 a19 b19 c1 d1 h1 i1 j1 k1 @k19");
        Cell c{r.Index(11), 4 + r.Index(13)};
        Cell v{r.Index(5) - 2, r.Index(5) - 2};
        Cell prev{c.x - v.x, c.y - v.y};
        if (!InField(prev)) continue;
        PlayerState& p = s.players[0];
        p.pos = p.turnStart = c;
        p.prevTurnStart = prev;
        bot::ReachMap m = bot::PlayerReach(s, 0);
        std::vector<Cell> dest = MoveDestinations(s, 0);
        int ones = 0;
        bool ok = m.Turns(c) == 0;
        for (int x = 0; x < kFieldWidth; ++x)
            for (int y = kFirstRow; y <= kLastRow; ++y) {
                Cell q{x, y};
                bool inDest = std::find(dest.begin(), dest.end(), q) != dest.end() && q != c;
                if (m.Turns(q) == 1) ++ones;
                if (inDest && m.Turns(q) != 1) ok = false;
                if (m.Turns(q) == kUnreachableTest) ok = false;   // everything is reachable eventually
            }
        CHECK(ok);
        CHECK(ones == static_cast<int>(dest.size()) - (std::find(dest.begin(), dest.end(), c) != dest.end()));
    }
    // From rest: radius 2k - 1 after k turns.
    bot::ReachMap m = bot::ComputeReach({5, 10}, {0, 0});
    CHECK(m.Turns({6, 11}) == 1 && m.Turns({7, 12}) == 2 && m.Turns({8, 10}) == 2 && m.Turns({9, 10}) == 3);
    CHECK(m.Turns({10, 10}) == 3 && m.Turns({5, 1}) == 5);
}

static void TestForcedPickupCertificate() {
    GameState s = Position("w 10 0-0 f10 g10 a4 j4 a15 d15 h15 j15 @W1");
    const Action exposed = Action::Pass({4, 5}); // e15, next to B2 at d15
    CHECK(Validate(s, exposed) == ActionError::None);
    const std::vector<Outcome> outcomes = Outcomes(s, exposed);
    const auto through = std::find_if(outcomes.begin(), outcomes.end(), [](const Outcome& outcome) {
        return outcome.kind == OutcomeKind::BallThrough;
    });
    CHECK(through != outcomes.end());
    if (through != outcomes.end()) {
        GameState child = s;
        CHECK(ApplyOutcome(child, exposed, *through) == ActionError::None);
        CHECK(child.ball.IsFree() && child.ball.stepPhase == 1);
        CHECK(bot::ProvenOpponentPickupBeforeBallStep(child));
        GameState reply = child;
        CHECK(ApplyOutcome(reply, Action::EndTurn(), Outcome::Certain()) == ActionError::None);
        CHECK(reply.ball.IsFree() && reply.ball.pos == child.ball.pos);
        CHECK(ApplyOutcome(reply, Action::Move(5, child.ball.pos), Outcome::Certain()) == ActionError::None);
        CHECK(reply.ball.holder == 5);
        child.players[5].moved = true; // the direct collector is unavailable
        CHECK(!bot::ProvenOpponentPickupBeforeBallStep(child));
    }
    const Action safe = Action::Pass({5, 9}); // f11, within W1's remaining reach
    const std::vector<Outcome> safeOutcomes = Outcomes(s, safe);
    const auto safeThrough = std::find_if(safeOutcomes.begin(), safeOutcomes.end(), [](const Outcome& outcome) {
        return outcome.kind == OutcomeKind::BallThrough;
    });
    CHECK(safeThrough != safeOutcomes.end());
    if (safeThrough != safeOutcomes.end()) {
        GameState child = s;
        CHECK(ApplyOutcome(child, safe, *safeThrough) == ActionError::None);
        CHECK(!bot::ProvenOpponentPickupBeforeBallStep(child));
    }
    GameState limited = s;
    for (int player = 0; player < kPlayersPerTeam; ++player) limited.players[player].moved = true;
    bot::GoalDfsOptions options;
    options.timeLimit = 0;
    options.completeTurns = true;
    options.groupMovements = true;
    options.maxTurnDepth = 1;
    options.maxMemoryBytes = 4 * 1024 * 1024;
    options.threads = 1;
    bot::GoalDfsSearch search(options);
    const bot::GoalSearchResult result = search.Search(limited);
    CHECK(Validate(limited, result.action) == ActionError::None);
    CHECK(search.Stats().completedTurnDepth == 1);
    CHECK(search.Stats().forcedPickupPrunes > 0);
}

static void TestNoOwnRecoveryCertificate() {
    GameState s = Position("w 10 0-0 f10 a4 a5 a6 a15 d15 h15 j15 @W1");
    const Action pass = Action::Pass({9, 10});  // j10, then out past k10
    const std::vector<Outcome> outcomes = Outcomes(s, pass);
    CHECK(outcomes.size() == 1);
    if (outcomes.size() != 1) return;
    CHECK(outcomes[0].kind == OutcomeKind::BallThrough && outcomes[0].probability == 1);
    GameState child = s;
    CHECK(ApplyOutcome(child, pass, outcomes[0]) == ActionError::None);
    CHECK(bot::ProvenNoOwnRecoveryOnPassiveReply(child));

    // A team-mate can wait on the final in-field cell and stop the ball.
    GameState receiver = s;
    receiver.players[1].pos = {10, 10};
    receiver.players[1].turnStart = receiver.players[1].prevTurnStart = receiver.players[1].pos;
    const std::vector<Outcome> receiveOutcomes = Outcomes(receiver, pass);
    CHECK(receiveOutcomes.size() == 1);
    if (receiveOutcomes.size() == 1) {
        GameState receiveChild = receiver;
        CHECK(ApplyOutcome(receiveChild, pass, receiveOutcomes[0]) == ActionError::None);
        CHECK(!bot::ProvenNoOwnRecoveryOnPassiveReply(receiveChild));
    }

    // A stationary opponent at that cell also ends the current attack.
    GameState opponent = s;
    opponent.players[4].pos = {10, 10};
    opponent.players[4].turnStart = opponent.players[4].prevTurnStart = opponent.players[4].pos;
    const std::vector<Outcome> opponentOutcomes = Outcomes(opponent, pass);
    CHECK(opponentOutcomes.size() == 1);
    if (opponentOutcomes.size() == 1) {
        GameState opponentChild = opponent;
        CHECK(ApplyOutcome(opponentChild, pass, opponentOutcomes[0]) == ActionError::None);
        CHECK(bot::ProvenNoOwnRecoveryOnPassiveReply(opponentChild));
    }

    // The long ball in game2 stops in the field; a runner can collect it later.
    GameState runner = Position("w 10 0-0 g3 h10 a4 j4 a15 d15 h15 j15 @W1");
    const Action longPass = Action::Pass({6, 12});  // g8 in notation
    const std::vector<Outcome> longOutcomes = Outcomes(runner, longPass);
    CHECK(longOutcomes.size() == 1);
    if (longOutcomes.size() == 1) {
        GameState longChild = runner;
        CHECK(ApplyOutcome(longChild, longPass, longOutcomes[0]) == ActionError::None);
        CHECK(!bot::ProvenNoOwnRecoveryOnPassiveReply(longChild));
    }

    GameState limited = s;
    for (int player = 0; player < kPlayersPerTeam; ++player) limited.players[player].moved = true;
    bot::GoalDfsOptions options;
    options.timeLimit = 0;
    options.completeTurns = true;
    options.groupMovements = true;
    options.maxTurnDepth = 1;
    options.maxMemoryBytes = 4 * 1024 * 1024;
    options.threads = 1;
    bot::GoalDfsSearch search(options);
    search.Search(limited);
    CHECK(search.Stats().noRecoveryPrunes > 0);
}

static void TestNextStepPickupCertificate() {
    GameState s = Position("w 10 0-0 f5 a4 b4 c4 g12 a15 b15 c15 @W1");
    const Action pass = Action::Pass({5, 11});  // f9, then f12 on the opponent's EndTurn
    const std::vector<Outcome> outcomes = Outcomes(s, pass);
    CHECK(outcomes.size() == 1);
    if (outcomes.size() != 1) return;
    CHECK(outcomes[0].kind == OutcomeKind::BallThrough && outcomes[0].probability == 1);
    GameState child = s;
    CHECK(ApplyOutcome(child, pass, outcomes[0]) == ActionError::None);
    CHECK(child.ball.IsFree() && child.ball.pos == (Cell{5, 11}));
    CHECK(!bot::ProvenOpponentPickupBeforeBallStep(child));
    CHECK(!bot::ProvenNoOwnRecoveryOnPassiveReply(child));
    CHECK(bot::ProvenOpponentPickupAtNextBallStep(child));

    // Verify the exact blocking and ball rules on the proposed opponent reply.
    GameState reply = child;
    CHECK(ApplyOutcome(reply, Action::EndTurn(), Outcome::Certain()) == ActionError::None);
    CHECK(Validate(reply, Action::Move(4, {5, 8})) == ActionError::None);
    CHECK(ApplyOutcome(reply, Action::Move(4, {5, 8}), Outcome::Certain()) == ActionError::None);
    const std::vector<Outcome> step = Outcomes(reply, Action::EndTurn());
    CHECK(step.size() == 1);
    if (step.size() == 1) {
        CHECK(step[0].kind == OutcomeKind::BallThrough && step[0].probability == 1);
        CHECK(ApplyOutcome(reply, Action::EndTurn(), step[0]) == ActionError::None);
        CHECK(reply.ball.holder == 4 && reply.ball.pos == (Cell{5, 8}));
    }

    const bot::GoalEpisode episode{kWhite, bot::GoalEpisodeStatus::OurAttack};
    const bot::TeamMovementList continuations = bot::EnumerateTeamMovements(child, episode);
    bool capturedAfterEveryContinuation = continuations.complete && continuations.states.size() > 1;
    for (const bot::TeamMovementState& continuation : continuations.states) {
        GameState after = continuation.state;
        if (ApplyOutcome(after, Action::EndTurn(), Outcome::Certain()) != ActionError::None ||
            Validate(after, Action::Move(4, {5, 8})) != ActionError::None ||
            ApplyOutcome(after, Action::Move(4, {5, 8}), Outcome::Certain()) != ActionError::None) {
            capturedAfterEveryContinuation = false;
            break;
        }
        const std::vector<Outcome> landing = Outcomes(after, Action::EndTurn());
        if (landing.size() != 1 || landing[0].kind != OutcomeKind::BallThrough ||
            landing[0].probability != 1 ||
            ApplyOutcome(after, Action::EndTurn(), landing[0]) != ActionError::None ||
            after.ball.holder != 4) {
            capturedAfterEveryContinuation = false;
            break;
        }
    }
    CHECK(capturedAfterEveryContinuation);

    // Our receiver can reach the landing cell before that reply and block it.
    GameState receiver = Position("w 10 0-0 f5 f13 b4 c4 g12 a15 b15 c15 @W1");
    const std::vector<Outcome> receiveOutcomes = Outcomes(receiver, pass);
    CHECK(receiveOutcomes.size() == 1);
    if (receiveOutcomes.size() == 1) {
        GameState receiveChild = receiver;
        CHECK(ApplyOutcome(receiveChild, pass, receiveOutcomes[0]) == ActionError::None);
        CHECK(!bot::ProvenOpponentPickupAtNextBallStep(receiveChild));
        CHECK(Validate(receiveChild, Action::Move(1, {5, 8})) == ActionError::None);
        CHECK(ApplyOutcome(receiveChild, Action::Move(1, {5, 8}), Outcome::Certain()) == ActionError::None);
        CHECK(ApplyOutcome(receiveChild, Action::EndTurn(), Outcome::Certain()) == ActionError::None);
        const std::vector<Outcome> receiveStep = Outcomes(receiveChild, Action::EndTurn());
        CHECK(receiveStep.size() == 1);
        if (receiveStep.size() == 1) {
            CHECK(ApplyOutcome(receiveChild, Action::EndTurn(), receiveStep[0]) == ActionError::None);
            CHECK(receiveChild.ball.holder == 1);
        }
    }

    GameState limited = s;
    for (int player = 0; player < kPlayersPerTeam; ++player) limited.players[player].moved = true;
    bot::GoalDfsOptions options;
    options.timeLimit = 0;
    options.completeTurns = true;
    options.groupMovements = true;
    options.maxTurnDepth = 1;
    options.maxMemoryBytes = 4 * 1024 * 1024;
    options.threads = 1;
    bot::GoalDfsSearch search(options);
    search.Search(limited);
    CHECK(search.Stats().nextStepPickupPrunes > 0);
}

static void TestFairness() {
    // Decisions depend on the open position only, never on the random generator
    // (which would tell the future dice).
    const char* positions[] = {
        "w 30 0-0 f12 d11 j14 a5 f13 e14 h15 f18 @W1",
        "b 40 0-0 f15 a10 b10 c10 c17 d18 h18 k12 @W1",
        "w 58 0-0 g6 e7 f6 g5 f15 g14 f13 g15 @h9,0,0,1 t:W",
    };
    for (const char* text : positions) {
        GameState a = Position(text), b = a;
        a.rng.Seed(1);
        b.rng.Seed(987654321);
        bot::Bot botA(TestBotOptions("goal-sel-run-fb-b8-t120")), botB(TestBotOptions("goal-sel-run-fb-b8-t120"));
        Action x = botA.NextAction(a), y = botB.NextAction(b);
        CHECK(x.type == y.type && x.player == y.player && x.target == y.target);
    }
}

// The position seen from the other side: the field turned by 180 degrees, the teams swapped.
static GameState MirrorState(const GameState& s) {
    auto f = [](Cell c) { return Cell{kFieldWidth - 1 - c.x, kGridHeight - 1 - c.y}; };
    GameState m = s;
    for (int k = 0; k < kNumPlayers; ++k) {
        const int j = (k + kPlayersPerTeam) % kNumPlayers;
        m.players[j] = s.players[k];
        m.players[j].pos = f(s.players[k].pos);
        m.players[j].turnStart = f(s.players[k].turnStart);
        m.players[j].prevTurnStart = f(s.players[k].prevTurnStart);
    }
    m.teamToMove = Opponent(s.teamToMove);
    m.ball.pos = f(s.ball.pos);
    m.ball.vel = {-s.ball.vel.x, -s.ball.vel.y};
    m.ball.dec = {-s.ball.dec.x, -s.ball.dec.y};
    if (!s.ball.IsFree()) m.ball.holder = (s.ball.holder + kPlayersPerTeam) % kNumPlayers;
    m.ball.lastTouchTeam = Opponent(s.ball.lastTouchTeam);
    m.ball.attackTeam = Opponent(s.ball.attackTeam);
    m.score[0] = s.score[1];
    m.score[1] = s.score[0];
    return m;
}

// Both teams are valued alike: the static estimate of the mirrored position
// (the field turned, the teams swapped) is the same for the mirrored attacking
// team, as it stands and after EndTurn, also one ply into the team turn. The
// search itself is not yet mirror-symmetric: the beam's ties are broken in the
// engine's order of actions (docs/bot_design.md, open questions).
static void TestMirrorSymmetry() {
    Rng r(5);
    const bot::GoalDfsSearch estimate(bot::FindBotPreset("goal-sel-run-fb-b8-t120")->options.search);
    const auto same = [&](const GameState& t, const GameState& u) {
        for (bool afterEndTurn : {false, true})
            CHECK(std::fabs(estimate.SelectiveEstimate(t, t.ball.attackTeam, afterEndTurn) -
                            estimate.SelectiveEstimate(u, u.ball.attackTeam, afterEndTurn)) < 1e-9);
    };
    for (int i = 0; i < 24; ++i) {
        GameState s = NewMatch(i);
        s.phase = Phase::Play;
        s.halfTurns = 10;
        s.teamToMove = r.Index(2);
        bool used[kFieldWidth][kGridHeight] = {};
        for (int k = 0; k < kNumPlayers; ++k) {
            Cell c;
            do c = {r.Index(kFieldWidth), 1 + r.Index(kGridHeight - 2)}; while (used[c.x][c.y]);
            used[c.x][c.y] = true;
            PlayerState& p = s.players[k];
            p.pos = p.turnStart = p.prevTurnStart = c;
        }
        const int h = r.Index(kNumPlayers);
        s.ball.holder = h;
        s.ball.pos = s.players[h].pos;
        s.ball.lastTouchTeam = s.ball.attackTeam = TeamOf(h);
        same(s, MirrorState(s));
        // One certain action of the team to move, then the mirrored one.
        for (const Action& a : LegalActions(s)) {
            const std::vector<Outcome> outs = OutcomesUnchecked(s, a);
            if (outs.size() != 1 || a.type == ActionType::EndTurn) continue;
            GameState t = s;
            ApplyUnchecked(t, a, outs.front());
            same(t, MirrorState(t));
            break;
        }
    }
}

// The collection of bots: unique names, found by name, every bot can be made.
static void TestBotRegistry() {
    const std::vector<bot::BotPreset>& presets = bot::BotPresets();
    CHECK(presets.size() >= 5);
    for (std::size_t i = 0; i < presets.size(); ++i) {
        CHECK(!presets[i].name.empty() && presets[i].name.find(' ') == std::string::npos);
        CHECK(bot::FindBotPreset(presets[i].name) == &presets[i]);
        for (std::size_t j = 0; j < i; ++j) CHECK(presets[i].name != presets[j].name);
        CHECK(bot::MakeBot(presets[i]) != nullptr);
    }
    CHECK(bot::FindBotPreset(bot::DefaultBotName()) != nullptr);
    CHECK(bot::FindBotPreset("no such bot") == nullptr);
}

// The attack episode by the race to the goal (7.67): game7's 2 v 1 of Black,
// the crowded position after 3.W, legal policy moves for the outsiders.
static void TestAttackEpisode() {
    GameState s;
    CHECK(ParsePosition("b 29 1-0 f10 e2/e4 g11/g12 e11/e12 d3/c5 b5/b7 e14/f16 e10/f11 @B2 t:B", &s));
    bot::AttackEpisode e = bot::AnalyzeAttackEpisode(s);
    const int w1 = 0, w2 = 1, w3 = 2, w4 = 3, b1 = 4, b2 = 5, b3 = 6, b4 = 7;
    CHECK(e.kind == bot::AttackEpisodeKind::TwoVsOne);
    CHECK(e.team == kBlack && e.carrier == b2);
    CHECK(e.attackers == ((1u << b1) | (1u << b2)));
    CHECK(e.defenders == (1u << w2));
    CHECK(e.outside == ((1u << w1) | (1u << w3) | (1u << w4) | (1u << b3) | (1u << b4)));
    // Black to move: its outsiders follow the play, legally, in order.
    GameState t = s;
    for (const Action& a : bot::OutsiderMoves(s, e)) {
        CHECK(a.type == ActionType::Move && (e.outside >> a.player & 1u) && TeamOf(a.player) == kBlack);
        CHECK(Validate(t, a) == ActionError::None);
        ApplyUnchecked(t, a, Outcome::Certain());
    }
    // After Black's turn White's outsiders run back towards their goal.
    ApplyUnchecked(t, Action::EndTurn(), OutcomesUnchecked(t, Action::EndTurn()).front());
    e = bot::AnalyzeAttackEpisode(t);
    GameState u = t;
    for (const Action& a : bot::OutsiderMoves(t, e)) {
        CHECK(Validate(u, a) == ActionError::None);
        CHECK(Abs(a.target.y - AttackedGoalRow(kBlack)) <= Abs(u.players[a.player].pos.y - AttackedGoalRow(kBlack)));
        ApplyUnchecked(u, a, Outcome::Certain());
    }
    // game7 after 3.W: nobody is outside.
    CHECK(ParsePosition("b 3 0-0 g12/f11 f13/d11 h13/h11 g7/f7 f12 i11/i12 c12 f17/f16 @W4 t:W", &s));
    e = bot::AnalyzeAttackEpisode(s);
    CHECK(e.kind == bot::AttackEpisodeKind::General && e.outside == 0);
}

// The full set of candidates (docs/bot_design.md, 7.17): the race by alpha-beta
// gives exactly the plain minimax; the possession table; the planner's value of
// keeping the ball after it was played; passes into space among the candidates;
// B* with all of it deterministic, legal, mirror-symmetric, and the bot plays.
// The race for a free ball (race.h): the fast and the plain search agree, the
// mirrored race is the same with the colours swapped.
// The attack episode of a free ball by the race solver, and the episode
// table (02.11.2026): g7 30.B after Black's pass >c3, B2 certainly takes the
// ball on c2 - a 2 v 1 (B1, B2 against W2) that the coarse race calls contested.
static void TestEpisodeTable() {
    GameState s;
    CHECK(ParsePosition("w 30 1-0 f10 e2/e4 g11/g12 e11/e12 f3/d3 b3/b5 c12/e14 c8/e10 @c3,0,-1,0 t:Bp", &s));
    const int w2 = 1, b1 = 4, b2 = 5;
    CHECK(bot::AnalyzeAttackEpisode(s).kind == bot::AttackEpisodeKind::None);
    bot::AttackEpisode e = bot::AnalyzeAttackEpisode(s, false, true);
    CHECK(e.kind == bot::AttackEpisodeKind::TwoVsOne);
    CHECK(e.team == kBlack && e.carrier == b2 && e.carrierPly == 2);
    CHECK(e.attackers == ((1u << b1) | (1u << b2)) && e.defenders == (1u << w2));
    // The taken state: the table's value does not fall with more turns and
    // stays within the best open shot the attackers can reach.
    const bot::BallRaceResult r = bot::SolveBallRace(s);
    CHECK(r.kind == bot::BallRaceKind::Taken && r.player == b2);
    const GameState t = bot::BallRaceTakenState(s, r);
    double previous = 0;
    for (int turns = 1; turns <= 5; ++turns) {
        bot::EpisodeValueOptions o;
        o.turns = turns;
        const bot::EpisodeValueResult v = bot::EpisodeValue(t, e.attackers, e.defenders, o);
        CHECK(v.complete);
        CHECK(v.value >= previous - 1e-12);
        CHECK(v.value >= 0 && v.value <= 1);
        previous = v.value;
    }
    CHECK(previous > 0.4 && previous < 0.5);   // 0.4382 (search d4-d5 of the whole game: 0.55)
    // No turn of the attack within the horizon: nothing.
    bot::EpisodeValueOptions one;
    one.turns = 1;
    CHECK(bot::EpisodeValue(t, e.attackers, e.defenders, one).value == 0);
}

static void TestRace() {
    const char* races[] = {
        "b 148 1-1 h10/i10 j6 h2/i4 g3/g5 i8/h10 k10 d7/d9 f3/h4 @g8,0,+4,0 t:Wp",
        "b 148 1-1 h10/h9 j6 h2/i4 g3/g5 i8/h10 k10 d7/d9 f3/h4 @g8,0,+4,0 t:Wp",
        "w 84 1-0 h10/j8 j13/j11 f7/h6 i12/i10 i8/i7 k13/k11 g14/g12 g16 @i13,0,0,1 t:Wp",
        "w 82 1-0 j8/k7 j11/i10 h6/i6 i10/i8 i7/j7 k11/j10 g12/g11 g16/g17 @f6,-2,-4,1 t:Wp",
    };
    for (const char* text : races) {
        GameState s = Position(text);
        bot::RaceOptions fast{6, false}, plain{6, true};
        bot::RaceResult a = bot::Race(s, fast), b = bot::Race(s, plain);
        CHECK(std::fabs(a.pWhite - b.pWhite) < 1e-9 && a.winner == b.winner && a.pickup == b.pickup);
        CHECK(a.pickupHalfTurn == b.pickupHalfTurn && a.firstMove[0] == b.firstMove[0] && a.firstMove[1] == b.firstMove[1]);
        if (a.winner >= 0) CHECK(!a.taken.ball.IsFree() && TeamOf(a.taken.ball.holder) == a.winner);
        // Mirrored: the same race with the colours swapped.
        bot::RaceResult m = bot::Race(MirrorState(s), fast);
        CHECK(std::fabs(m.pWhite - (1 - a.pWhite)) < 1e-9 && (a.winner < 0 ? m.winner < 0 : m.winner == 1 - a.winner));
    }
    // The last one: W3 takes the ball at once.
    {
        bot::RaceResult r = bot::Race(Position(races[3]), bot::RaceOptions{8, false, 0.01, 3000});
        CHECK(r.winner == kWhite && r.pickupHalfTurn == 0);
    }

}

// The possession value by the ball's cell (docs/bot_design.md, 7.19): the table
// derived from the mover row is consistent; the compiled one is higher near the
// opponent's goal than at the own goal.
static void TestPossessionTable() {
    const bot::ValueTable& t = bot::ValueTable::PossessionDefault();
    const int mover = bot::ValueTable::Kind(bot::BallOwner::Mover), other = bot::ValueTable::Kind(bot::BallOwner::Other);
    for (int x = 0; x < kFieldWidth; ++x)
        for (int y = 1; y < kGridHeight - 1; ++y) {
            const Cell c{x, y}, r{kFieldWidth - 1 - x, kGridHeight - 1 - y};
            CHECK(std::fabs(t.Get(other, c) + t.Get(mover, r)) < 1e-12);
            CHECK(std::fabs(t.Get(mover, c) - t.Get(mover, Cell{kFieldWidth - 1 - x, y})) < 1e-9);   // pooled with the mirror
        }
    Cell att, own, centre{kCenterX, kCenterY};
    ParseCell("f17", &att);   // White's frame: near Black's goal
    ParseCell("f3", &own);
    CHECK(t.Of(bot::BallOwner::Mover, att, kWhite) > t.Of(bot::BallOwner::Mover, centre, kWhite));
    CHECK(t.Of(bot::BallOwner::Mover, centre, kWhite) > t.Of(bot::BallOwner::Mover, own, kWhite));
    CHECK(std::fabs(t.Of(bot::BallOwner::Mover, att, kWhite) - t.Of(bot::BallOwner::Mover, bot::ValueTable::Frame(att, kBlack), kBlack)) < 1e-12);
}

static void TestImmediateShotUpper() {
    const char* positions[] = {
        "w 30 0-0 f12 d12 h12 a6 f14 c14 i14 k15 @W1",
        "b 30 0-0 f14 c14 i14 k15 f12 d12 h12 a6 @B1",
    };
    for (const char* position : positions) {
        const GameState s = Position(position);
        const int team = s.teamToMove;
        const int holder = s.ball.holder;
        const double upper = bot::ImmediateShotUpper(s, team);
        CHECK(upper >= 0 && upper <= 1);
        CHECK(bot::ImmediateShotUpper(s, Opponent(team)) == 0);
        CHECK(ShotProbability(s, holder) <= upper + 1e-12);

        // Team-mates may move first and unblock a shot or the holder's route.
        // Every resulting legal shot must still be under the relaxed bound.
        for (int teammate = FirstPlayerOf(team); teammate < FirstPlayerOf(team) + kPlayersPerTeam; ++teammate) {
            if (teammate == holder) continue;
            for (Cell support : MoveDestinations(s, teammate)) {
                GameState withSupport = s;
                if (ApplyOutcome(withSupport, Action::Move(teammate, support), Outcome::Certain()) != ActionError::None)
                    continue;
                CHECK(ShotProbability(withSupport, holder) <= upper + 1e-12);
                for (Cell destination : MoveDestinations(withSupport, holder)) {
                    GameState afterMove = withSupport;
                    if (ApplyOutcome(afterMove, Action::Move(holder, destination), Outcome::Certain()) !=
                        ActionError::None)
                        continue;
                    CHECK(ShotProbability(afterMove, holder) <= upper + 1e-12);
                }
            }
        }

        GameState played = s;
        played.ballPlayed = true;
        CHECK(bot::ImmediateShotUpper(played, team) == 0);

        GameState freeBall = s;
        freeBall.ball.holder = kNoPlayer;
        CHECK(bot::ImmediateShotUpper(freeBall, team) == 1);
        freeBall.ballPlayed = true;
        CHECK(bot::ImmediateShotUpper(freeBall, team) == 0);

        GameState opponentBall = s;
        opponentBall.ball.holder = FirstPlayerOf(Opponent(team));
        CHECK(bot::ImmediateShotUpper(opponentBall, team) == 1);
    }
}

static void TestHolderShotChanceThisTurn() {
    const char* positions[] = {
        "w 30 0-0 f12 d12 h12 a6 f14 c14 i14 k15 @W1",
        "b 30 0-0 f14 c14 i14 k15 f12 d12 h12 a6 @B1",
        "w 10 0-0 f10 g10 a4 j4 a15 d15 h15 j15 @W1",
    };
    for (const char* position : positions) {
        const GameState s = Position(position);
        const int holder = s.ball.holder;
        double expected = ShotProbability(s, holder);
        bool sawMove = false;
        for (const Action& action : LegalActions(s)) {
            if (action.type != ActionType::Move || action.player != holder) continue;
            GameState after = s;
            CHECK(ApplyOutcome(after, action, Outcome::Certain()) == ActionError::None);
            CHECK(Validate(after, Action::Shot()) == ActionError::None);
            const double chance = ShotProbability(after, holder);
            expected = std::max(expected, chance);
            CHECK(std::fabs(bot::HolderShotChanceThisTurn(after) - chance) < 1e-12);
            sawMove = true;
        }
        CHECK(sawMove);
        CHECK(expected > ShotProbability(s, holder));
        CHECK(std::fabs(bot::HolderShotChanceThisTurn(s) - expected) < 1e-12);
        CHECK(bot::HolderShotChanceThisTurn(s) <=
              bot::ImmediateShotUpper(s, s.teamToMove) + 1e-12);

        GameState played = s;
        played.ballPlayed = true;
        CHECK(bot::HolderShotChanceThisTurn(played) == 0);
        GameState waiting = s;
        waiting.teamToMove = Opponent(s.teamToMove);
        CHECK(bot::HolderShotChanceThisTurn(waiting) == 0);
        GameState freeBall = s;
        freeBall.ball.holder = kNoPlayer;
        CHECK(bot::HolderShotChanceThisTurn(freeBall) == 0);
    }
}

static void TestGoalEpisodeTransitions() {
    const GameState white = Position("w 30 0-0 f12 d12 h12 a6 f14 c14 i14 k15 @W1");
    const bot::GoalEpisode attack{kWhite, bot::GoalEpisodeStatus::OurAttack};
    bool sawGoal = false, sawCaught = false, sawParry = false;
    for (const Outcome& outcome : Outcomes(white, Action::Shot())) {
        GameState after = white;
        EventLog events;
        CHECK(ApplyOutcome(after, Action::Shot(), outcome, &events) == ActionError::None);
        const bot::GoalEpisode next = bot::AdvanceGoalEpisode(attack, white, after, events);
        if (outcome.kind == OutcomeKind::Goal) {
            sawGoal = true;
            CHECK(next.status == bot::GoalEpisodeStatus::OurGoal);
        } else if (outcome.kind == OutcomeKind::Caught) {
            sawCaught = true;
            CHECK(after.ball.IsFree() && after.phase == Phase::Lineup);
            CHECK(next.status == bot::GoalEpisodeStatus::TheirCounterattack);
        } else if (outcome.kind == OutcomeKind::Parried) {
            sawParry = true;
            CHECK(next.status == bot::GoalEpisodeStatus::OurAttack);
        }
    }
    CHECK(sawGoal && sawCaught && sawParry);

    // A deliberate pass into space may leave the ball physically free across
    // the opponent's turn without ending the original attacking episode.
    const Action freePass = Action::Pass({5, 9});  // f11
    GameState freeAfterPass = white;
    EventLog freePassEvents;
    const std::vector<Outcome> freePassOutcomes = Outcomes(white, freePass);
    CHECK(freePassOutcomes.size() == 1 && freePassOutcomes.front().kind == OutcomeKind::BallThrough);
    CHECK(ApplyOutcome(freeAfterPass, freePass, freePassOutcomes.front(), &freePassEvents) == ActionError::None);
    CHECK(freeAfterPass.ball.IsFree());
    bot::GoalEpisode freeEpisode = bot::AdvanceGoalEpisode(attack, white, freeAfterPass, freePassEvents);
    CHECK(freeEpisode.status == bot::GoalEpisodeStatus::OurAttack);
    GameState blackTurn = freeAfterPass;
    EventLog endEvents;
    CHECK(ApplyOutcome(blackTurn, Action::EndTurn(), Outcomes(freeAfterPass, Action::EndTurn()).front(),
                       &endEvents) == ActionError::None);
    freeEpisode = bot::AdvanceGoalEpisode(freeEpisode, freeAfterPass, blackTurn, endEvents);
    CHECK(blackTurn.teamToMove == kBlack && blackTurn.ball.IsFree());
    CHECK(freeEpisode.status == bot::GoalEpisodeStatus::OurAttack);

    bool sawIntercept = false;
    const Action pass = Action::Pass({5, 5});  // f15, past B1 at f14
    for (const Outcome& outcome : Outcomes(white, pass)) {
        if (outcome.kind != OutcomeKind::Intercepted) continue;
        GameState after = white;
        EventLog events;
        CHECK(ApplyOutcome(after, pass, outcome, &events) == ActionError::None);
        CHECK(bot::AdvanceGoalEpisode(attack, white, after, events).status ==
              bot::GoalEpisodeStatus::TheirCounterattack);
        sawIntercept = true;
    }
    CHECK(sawIntercept);

    // An out ends the current attack. The other team gets exactly one new
    // counterattack only when it is the team that restarts from the centre.
    GameState out = white;
    out.ball.holder = kNoPlayer;
    out.ball.pos = {9, 10};
    out.ball.vel = {3, 0};
    out.ball.dec = {-1, 0};
    out.ball.stepPhase = 0;
    out.ball.lastTouchTeam = kWhite;
    GameState afterOut = out;
    EventLog outEvents;
    CHECK(Apply(afterOut, Action::EndTurn(), &outEvents) == ActionError::None);
    CHECK(HasEvent(outEvents, EventType::BallOut));
    CHECK(bot::AdvanceGoalEpisode(attack, out, afterOut, outEvents).status ==
          bot::GoalEpisodeStatus::TheirCounterattack);
    out.ball.lastTouchTeam = kBlack;
    afterOut = out;
    outEvents.clear();
    CHECK(Apply(afterOut, Action::EndTurn(), &outEvents) == ActionError::None);
    CHECK(HasEvent(outEvents, EventType::BallOut));
    CHECK(bot::AdvanceGoalEpisode(attack, out, afterOut, outEvents).status ==
          bot::GoalEpisodeStatus::NoGoal);
    CHECK(bot::AdvanceGoalEpisode({kWhite, bot::GoalEpisodeStatus::TheirCounterattack},
                                  out, afterOut, outEvents).status == bot::GoalEpisodeStatus::NoGoal);

    const GameState black = Position("b 30 0-0 f12 d12 h12 a6 f14 c14 i14 k15 @B1");
    const bot::GoalEpisode counter{kWhite, bot::GoalEpisodeStatus::TheirCounterattack};
    bool sawCounterGoal = false, sawCounterCaught = false;
    for (const Outcome& outcome : Outcomes(black, Action::Shot())) {
        if (outcome.kind != OutcomeKind::Goal && outcome.kind != OutcomeKind::Caught) continue;
        GameState after = black;
        EventLog events;
        CHECK(ApplyOutcome(after, Action::Shot(), outcome, &events) == ActionError::None);
        const bot::GoalEpisode next = bot::AdvanceGoalEpisode(counter, black, after, events);
        if (outcome.kind == OutcomeKind::Goal) {
            sawCounterGoal = true;
            CHECK(next.status == bot::GoalEpisodeStatus::TheirGoal);
        } else {
            sawCounterCaught = true;
            CHECK(next.status == bot::GoalEpisodeStatus::NoGoal);
        }
    }
    CHECK(sawCounterGoal && sawCounterCaught);

    CHECK(bot::InitialGoalBounds(bot::GoalEpisodeStatus::OurAttack).lower == -1);
    CHECK(bot::InitialGoalBounds(bot::GoalEpisodeStatus::OurAttack).upper == 1);
    CHECK(bot::InitialGoalBounds(bot::GoalEpisodeStatus::TheirCounterattack).upper == 0);
    CHECK(bot::InitialGoalBounds(bot::GoalEpisodeStatus::OurGoal).lower == 1);
}

static void TestGoalDfsBounds() {
    GameState s = Position("w 179 0-0 f19 d11 j14 a5 f13 e14 h15 f4 @W1");
    for (const Action& action : LegalActions(s)) {
        if (action.type != ActionType::Move && action.type != ActionType::EndTurn) continue;
        const std::vector<Outcome> outcomes = OutcomesUnchecked(s, action);
        CHECK(outcomes.size() == 1);
        CHECK(outcomes[0].kind == OutcomeKind::Certain);
        CHECK(outcomes[0].probability == 1);
    }
    GameState lineup = NewMatch(1);
    for (const Action& action : LegalActions(lineup)) {
        if (action.type != ActionType::Place) continue;
        const std::vector<Outcome> outcomes = OutcomesUnchecked(lineup, action);
        CHECK(outcomes.size() == 1);
        CHECK(outcomes[0].kind == OutcomeKind::Certain);
        CHECK(outcomes[0].probability == 1);
        break;
    }
    for (int k = 0; k < kPlayersPerTeam; ++k) s.players[k].moved = true;
    bot::GoalDfsOptions full;
    full.timeLimit = 0;
    full.maxActionDepth = 1;
    full.maxMemoryBytes = 1024 * 1024;
    full.threads = 1;
    bot::GoalDfsSearch search(full);
    const bot::GoalSearchResult a = search.Search(s);
    CHECK(a.moves.size() == LegalActions(s).size());
    CHECK(Validate(s, a.action) == ActionError::None);
    CHECK(a.action.type == ActionType::Shot);
    CHECK(search.Stats().completedActionDepth == 1);
    CHECK(a.estimatedMemoryMB <= 1);
    const double p = ShotProbability(s, s.ball.holder);
    for (const bot::GoalRootMove& move : a.moves) {
        CHECK(move.bounds.lower <= move.bounds.upper);
        if (move.action.type == ActionType::Shot) {
            CHECK(std::fabs(move.bounds.lower - p) < 1e-9);
            CHECK(std::fabs(move.bounds.upper - p) < 1e-9);
        }
    }

    bot::GoalDfsOptions shortRun = full;
    shortRun.expansions = 1;
    bot::GoalDfsSearch interrupted(shortRun);
    const bot::GoalSearchResult b = interrupted.Search(s);
    CHECK(b.expansions == 1);
    CHECK(b.moves.size() == a.moves.size());
    for (std::size_t i = 0; i < a.moves.size(); ++i) {
        CHECK(b.moves[i].bounds.lower <= a.moves[i].bounds.lower + 1e-12);
        CHECK(b.moves[i].bounds.upper >= a.moves[i].bounds.upper - 1e-12);
    }

    bot::GoalDfsOptions parallelOptions = full;
    parallelOptions.threads = 2;
    bot::GoalDfsSearch parallel(parallelOptions);
    const bot::GoalSearchResult c = parallel.Search(s);
    CHECK(parallel.Stats().completedActionDepth == 1);
    CHECK(c.estimatedMemoryMB <= 1);
    CHECK(c.moves.size() == a.moves.size());
    for (std::size_t i = 0; i < a.moves.size(); ++i) {
        CHECK(std::fabs(c.moves[i].bounds.lower - a.moves[i].bounds.lower) < 1e-12);
        CHECK(std::fabs(c.moves[i].bounds.upper - a.moves[i].bounds.upper) < 1e-12);
    }

    parallelOptions.expansions = 1;
    bot::GoalDfsSearch parallelInterrupted(parallelOptions);
    const bot::GoalSearchResult d = parallelInterrupted.Search(s);
    CHECK(d.expansions <= 1);
    CHECK(d.moves.size() == a.moves.size());
    auto countedExpansions = [](const bot::GoalDfsStats& stats) {
        return stats.moveExpansions + stats.passExpansions + stats.shotExpansions +
               stats.attackExpansions + stats.endTurnExpansions + stats.placeExpansions;
    };
    CHECK(a.expansions == countedExpansions(search.Stats()));
    CHECK(b.expansions == countedExpansions(interrupted.Stats()));
    CHECK(c.expansions == countedExpansions(parallel.Stats()));
    CHECK(d.expansions == countedExpansions(parallelInterrupted.Stats()));
    for (std::size_t i = 0; i < a.moves.size(); ++i) {
        CHECK(d.moves[i].bounds.lower <= a.moves[i].bounds.lower + 1e-12);
        CHECK(d.moves[i].bounds.upper >= a.moves[i].bounds.upper - 1e-12);
    }

    // A state-level shot bound applies to the expectation, not to a rare
    // unvisited goal outcome. Deepening must not widen the shot interval.
    GameState far = Position("w 179 0-0 f3 d11 j14 a5 f13 e14 h15 f4 @W1");
    for (int k = 1; k < kNumPlayers; ++k) far.players[k].moved = true;
    bot::GoalDfsOptions farOptions = full;
    farOptions.maxActionDepth = 2;
    bot::GoalDfsSearch farSearch(farOptions);
    const bot::GoalSearchResult farResult = farSearch.Search(far);
    CHECK(farSearch.Stats().completedActionDepth == 2);
    const double farProbability = ShotProbability(far, far.ball.holder);
    bool foundFarShot = false;
    for (const bot::GoalRootMove& move : farResult.moves) {
        if (move.action.type != ActionType::Shot) continue;
        foundFarShot = true;
        CHECK(std::fabs(move.bounds.lower - farProbability) < 1e-12);
        CHECK(std::fabs(move.bounds.upper - farProbability) < 1e-12);
    }
    CHECK(foundFarShot);
}

static void TestGoalTeamDfsTurnDepth() {
    GameState s = Position("w 179 0-0 f3 d11 j14 a5 f13 e14 h15 f4 @W1");
    for (int k = 1; k < kNumPlayers; ++k) s.players[k].moved = true;
    const double directShot = ShotProbability(s, s.ball.holder);
    Action betterMove;
    double movedShot = directShot;
    bool foundMove = false;
    for (const Action& action : LegalActions(s)) {
        if (action.type != ActionType::Move || action.player != s.ball.holder) continue;
        GameState child = s;
        ApplyUnchecked(child, action, Outcome::Certain());
        const double p = ShotProbability(child, child.ball.holder);
        if (p > movedShot) {
            betterMove = action;
            movedShot = p;
            foundMove = true;
        }
    }
    CHECK(foundMove);

    bot::GoalDfsOptions actionOptions;
    actionOptions.timeLimit = 0;
    actionOptions.maxActionDepth = 1;
    actionOptions.maxMemoryBytes = 1024 * 1024;
    actionOptions.threads = 1;
    bot::GoalDfsSearch actionSearch(actionOptions);
    const bot::GoalSearchResult actions = actionSearch.Search(s);
    CHECK(actionSearch.Stats().completedActionDepth == 1);

    bot::GoalDfsOptions turnOptions = actionOptions;
    turnOptions.completeTurns = true;
    turnOptions.maxTurnDepth = 1;
    bot::GoalDfsSearch turnSearch(turnOptions);
    const bot::GoalSearchResult turns = turnSearch.Search(s);
    CHECK(turnSearch.Stats().completedTurnDepth == 1);
    CHECK(turns.moves.size() == actions.moves.size());
    CHECK(turns.value.lower >= movedShot - 1e-12);
    CHECK(turns.value.lower > directShot);
    CHECK(Validate(s, turns.action) == ActionError::None);
    for (std::size_t i = 0; i < turns.moves.size(); ++i) {
        CHECK(turns.moves[i].bounds.lower <= turns.moves[i].bounds.upper);
        if (turns.moves[i].action.type == ActionType::Move &&
            turns.moves[i].action.player == betterMove.player &&
            turns.moves[i].action.target == betterMove.target) {
            CHECK(actions.moves[i].bounds.lower < movedShot);
            CHECK(turns.moves[i].bounds.lower >= movedShot - 1e-12);
        }
    }
}

// At the last half-turn, a pass or EndTurn cannot lead to a goal before the
// match ends. Enumerate every legal move order directly from the engine and
// compare its best shot with the grouped search, including every root action.
static double ExactLastTurnValue(const GameState& state, const Action& action) {
    if (action.type == ActionType::Shot)
        return ShotProbability(state, state.ball.holder);
    if (action.type != ActionType::Move) return 0;
    GameState child = state;
    ApplyUnchecked(child, action, Outcome::Certain());
    double best = 0;
    for (const Action& next : LegalActions(child))
        best = std::max(best, ExactLastTurnValue(child, next));
    return best;
}

static void TestGoalTeamMovesAgainstExhaustiveLastTurn() {
    const char* positions[] = {
        "w 179 0-0 f3 d11 j14 a5 f13 e14 h15 f4 @W1",
        "w 179 0-0 f3 g3 j14 a5 f13 e14 h15 f4 @W1",
    };
    for (int position = 0; position < 2; ++position) {
        GameState state = Position(positions[position]);
        for (int k = 2; k < kNumPlayers; ++k) state.players[k].moved = true;
        if (position == 0) state.players[1].moved = true;

        bot::GoalDfsOptions options;
        options.timeLimit = 0;
        options.maxTurnDepth = 1;
        options.maxMemoryBytes = 16 * 1024 * 1024;
        options.completeTurns = true;
        options.groupMovements = true;
        options.threads = 1;
        bot::GoalDfsSearch search(options);
        const bot::GoalSearchResult result = search.Search(state);
        CHECK(search.Stats().completedTurnDepth == 1);
        CHECK(!result.memoryLimited);
        CHECK(result.moves.size() == LegalActions(state).size());
        const bot::GoalDfsStats expansionStats = search.Stats();
        CHECK(result.expansions == expansionStats.moveExpansions + expansionStats.passExpansions +
                                   expansionStats.shotExpansions + expansionStats.attackExpansions +
                                   expansionStats.endTurnExpansions + expansionStats.placeExpansions);

        double exactBest = 0;
        double chosenExact = -1;
        for (const bot::GoalRootMove& move : result.moves) {
            const double exact = ExactLastTurnValue(state, move.action);
            exactBest = std::max(exactBest, exact);
            CHECK(move.bounds.lower <= exact + 1e-12);
            CHECK(move.bounds.upper >= exact - 1e-12);
            if (move.action.type == result.action.type &&
                move.action.player == result.action.player &&
                move.action.target == result.action.target) chosenExact = exact;
        }
        CHECK(chosenExact >= 0);
        CHECK(std::fabs(chosenExact - exactBest) < 1e-12);
        CHECK(result.rootBounds.lower <= exactBest + 1e-12);
        CHECK(result.rootBounds.upper >= exactBest - 1e-12);
        std::printf("last-turn oracle %d: best %.12f, chosen %.12f, proven %d\n",
                    position, exactBest, chosenExact, result.proven ? 1 : 0);
        for (int width : {2, 8, 32}) {
            bot::GoalDfsOptions approximateOptions = options;
            approximateOptions.selectiveWidth = width;
            bot::GoalDfsSearch approximateSearch(approximateOptions);
            const bot::GoalSearchResult approximate = approximateSearch.Search(state);
            const double actual = ExactLastTurnValue(state, approximate.action);
            CHECK(Validate(state, approximate.action) == ActionError::None);
            CHECK(!approximate.proven);
            CHECK(approximateSearch.Stats().completedTurnDepth == 1);
            CHECK(actual <= exactBest + 1e-12);
            std::printf("selective last-turn %d width %d: chosen %.12f, regret %.12f\n",
                        position, width, actual, exactBest - actual);
        }
        if (position == 0) {
            bot::GoalDfsOptions parallelOptions = options;
            parallelOptions.threads = 2;
            bot::GoalDfsSearch parallelSearch(parallelOptions);
            const bot::GoalSearchResult parallel = parallelSearch.Search(state);
            CHECK(parallelSearch.Stats().completedTurnDepth == 1);
            CHECK(parallel.moves.size() == result.moves.size());
            for (const bot::GoalRootMove& move : parallel.moves) {
                const double exact = ExactLastTurnValue(state, move.action);
                CHECK(move.bounds.lower <= exact + 1e-12);
                CHECK(move.bounds.upper >= exact - 1e-12);
            }
            const double parallelChosen = ExactLastTurnValue(state, parallel.action);
            CHECK(std::fabs(parallelChosen - exactBest) < 1e-12);
            std::printf("two-thread oracle: best %.12f, chosen %.12f, proven %d\n",
                        exactBest, parallelChosen, parallel.proven ? 1 : 0);

            bot::GoalDfsOptions interruptedOptions = options;
            interruptedOptions.expansions = 1;
            bot::GoalDfsSearch interruptedSearch(interruptedOptions);
            const bot::GoalSearchResult interrupted = interruptedSearch.Search(state);
            const double interruptedExact = ExactLastTurnValue(state, interrupted.action);
            CHECK(!interrupted.proven);
            CHECK(interruptedExact <= exactBest + 1e-12);
            std::printf("one-expansion oracle: best %.12f, chosen %.12f, proven %d\n",
                        exactBest, interruptedExact, interrupted.proven ? 1 : 0);
        }
    }
}

static void TestTeamMovementEnumeration() {
    GameState s = Position("w 10 0-0 f10 g10 a4 j4 a15 d15 h15 j15 @W1");
    s.players[2].moved = true;
    s.players[3].moved = true;
    const Action unblock = Action::Move(1, {7, 10});
    const Action freed = Action::Move(0, {6, 10});
    CHECK(Validate(s, freed) != ActionError::None);
    CHECK(Validate(s, unblock) == ActionError::None);
    GameState afterUnblock = s;
    ApplyUnchecked(afterUnblock, unblock, Outcome::Certain());
    CHECK(Validate(afterUnblock, freed) == ActionError::None);

    const bot::GoalEpisode episode{s.ball.attackTeam, bot::GoalEpisodeStatus::OurAttack};
    const bot::TeamMovementList list = bot::EnumerateTeamMovements(s, episode);
    CHECK(list.complete);
    CHECK(list.states.size() > 1);
    CHECK(list.duplicates > 0);
    bool foundFreed = false;
    for (const bot::TeamMovementState& entry : list.states) {
        GameState replay = s;
        bot::GoalEpisode replayEpisode = episode;
        for (int step = 0; step < entry.count; ++step) {
            CHECK(Validate(replay, entry.moves[step]) == ActionError::None);
            EventLog events;
            GameState before = replay;
            ApplyUnchecked(replay, entry.moves[step], Outcome::Certain(), &events);
            replayEpisode = bot::AdvanceGoalEpisode(replayEpisode, before, replay, events);
        }
        CHECK(bot::SameSearchState(replay, entry.state));
        CHECK(replayEpisode.team == entry.episode.team);
        CHECK(replayEpisode.status == entry.episode.status);
        if (entry.count == 2 && entry.moves[0].player == unblock.player &&
            entry.moves[0].target == unblock.target && entry.moves[1].player == freed.player &&
            entry.moves[1].target == freed.target) foundFreed = true;
    }
    CHECK(foundFreed);

    // A pass can precede the remaining team moves. The search starts a fresh
    // movement closure from each pass outcome.
    const Action earlyPass = Action::Pass({5, 9});
    CHECK(Validate(s, earlyPass) == ActionError::None);
    GameState afterEarlyPass = s;
    EventLog passEvents;
    const std::vector<Outcome> earlyOutcomes = Outcomes(s, earlyPass);
    const auto through = std::find_if(earlyOutcomes.begin(), earlyOutcomes.end(),
                                      [](const Outcome& outcome) {
                                          return outcome.kind == OutcomeKind::BallThrough;
                                      });
    CHECK(through != earlyOutcomes.end());
    if (through != earlyOutcomes.end()) {
        CHECK(ApplyOutcome(afterEarlyPass, earlyPass, *through, &passEvents) == ActionError::None);
        CHECK(afterEarlyPass.turnId == s.turnId);
        CHECK(Validate(afterEarlyPass, unblock) == ActionError::None);
        const bot::GoalEpisode afterPassEpisode =
            bot::AdvanceGoalEpisode(episode, s, afterEarlyPass, passEvents);
        const bot::TeamMovementList afterPassMoves =
            bot::EnumerateTeamMovements(afterEarlyPass, afterPassEpisode);
        bool foundMoveAfterPass = false;
        for (const bot::TeamMovementState& entry : afterPassMoves.states)
            if (entry.count == 1 && entry.moves[0].player == unblock.player &&
                entry.moves[0].target == unblock.target) foundMoveAfterPass = true;
        CHECK(afterPassMoves.complete);
        CHECK(foundMoveAfterPass);
    }

    // The holder can also pass from the end of an ordered movement sequence.
    GameState afterBothMoves = s;
    ApplyUnchecked(afterBothMoves, unblock, Outcome::Certain());
    ApplyUnchecked(afterBothMoves, freed, Outcome::Certain());
    CHECK(afterBothMoves.ball.pos == freed.target);
    const Action latePass = Action::Pass({6, 9});
    CHECK(Validate(afterBothMoves, latePass) == ActionError::None);
    bool foundLatePassSource = false;
    for (const bot::TeamMovementState& entry : list.states)
        if (bot::SameSearchState(entry.state, afterBothMoves) &&
            Validate(entry.state, latePass) == ActionError::None) foundLatePassSource = true;
    CHECK(foundLatePassSource);

    std::vector<bot::TeamMovementState> reference;
    std::function<void(const GameState&, bot::GoalEpisode)> visit =
        [&](const GameState& state, bot::GoalEpisode currentEpisode) {
            bool known = false;
            for (const bot::TeamMovementState& previous : reference)
                if (previous.episode.team == currentEpisode.team &&
                    previous.episode.status == currentEpisode.status &&
                    bot::SameSearchState(previous.state, state)) known = true;
            if (!known) {
                bot::TeamMovementState entry;
                entry.state = state;
                entry.episode = currentEpisode;
                reference.push_back(entry);
            }
            for (const Action& action : LegalActions(state)) {
                if (action.type != ActionType::Move) continue;
                GameState child = state;
                EventLog events;
                ApplyUnchecked(child, action, Outcome::Certain(), &events);
                visit(child, bot::AdvanceGoalEpisode(currentEpisode, state, child, events));
            }
        };
    visit(s, episode);
    // The reference has unique states. Equal sizes plus inclusion below also
    // prove that the enumerated list has no duplicates.
    CHECK(reference.size() == list.states.size());
    for (const bot::TeamMovementState& expected : reference) {
        bool found = false;
        for (const bot::TeamMovementState& entry : list.states)
            if (entry.episode.team == expected.episode.team &&
                entry.episode.status == expected.episode.status &&
                bot::SameSearchState(entry.state, expected.state)) found = true;
        CHECK(found);
    }
    const bot::TeamMovementList capped = bot::EnumerateTeamMovements(s, episode, 2);
    CHECK(!capped.complete);
    CHECK(capped.states.size() == 2);

    // The grouped turn search must preserve the same rigorous root bounds as
    // the ordinary action tree when a complete turn is affordable.
    s.ballPlayed = true;
    bot::GoalDfsOptions options;
    options.timeLimit = 0;
    options.maxTurnDepth = 1;
    options.maxMemoryBytes = 2 * 1024 * 1024;
    options.completeTurns = true;
    options.threads = 1;
    bot::GoalDfsSearch ordinary(options);
    const bot::GoalSearchResult plain = ordinary.Search(s);
    options.groupMovements = true;
    bot::GoalDfsSearch grouped(options);
    const bot::GoalSearchResult merged = grouped.Search(s);
    CHECK(ordinary.Stats().completedTurnDepth == 1);
    CHECK(grouped.Stats().completedTurnDepth == 1);
    CHECK(merged.moves.size() == plain.moves.size());
    CHECK(merged.rootBounds.lower <= plain.rootBounds.upper);
    CHECK(plain.rootBounds.lower <= merged.rootBounds.upper);
    for (std::size_t i = 0; i < merged.moves.size(); ++i) {
        CHECK(merged.moves[i].bounds.lower <= plain.moves[i].bounds.upper);
        CHECK(plain.moves[i].bounds.lower <= merged.moves[i].bounds.upper);
    }

    bot::GoalDfsOptions selectiveOptions = options;
    selectiveOptions.selectiveWidth = 2;
    selectiveOptions.expansions = 100000;
    bot::GoalDfsSearch selective(selectiveOptions);
    const bot::GoalSearchResult approximate = selective.Search(s);
    CHECK(Validate(s, approximate.action) == ActionError::None);
    CHECK(!approximate.proven);
    CHECK(selective.Stats().selectiveMovementDrops > 0);
    CHECK(selective.Stats().selectiveActionDrops > 0);
    CHECK(selective.Stats().completedTurnDepth == 1);
    selectiveOptions.maxTurnDepth = 2;
    selectiveOptions.expansions = approximate.expansions + 1;
    bot::GoalDfsSearch interruptedSelective(selectiveOptions);
    const bot::GoalSearchResult retained = interruptedSelective.Search(s);
    CHECK(interruptedSelective.Stats().completedTurnDepth == 1);
    CHECK(retained.action.type == approximate.action.type);
    CHECK(retained.action.player == approximate.action.player);
    CHECK(retained.action.target == approximate.action.target);
    CHECK(std::fabs(retained.value.lower - approximate.value.lower) < 1e-12);
}

// The best-first selective tree (bot_design.md, 7.68): deterministic for a
// fixed number of simulations and independent of the generator, a legal plan
// whose value is that of a root candidate, the obvious shot, the tree kept.
static void TestSelectiveTree() {
    const bot::BotPreset* preset = bot::FindBotPreset("goal-sel-tree-cat-b8-t90");
    CHECK(preset != nullptr);
    if (!preset) return;
    bot::GoalDfsOptions g = preset->options.search;
    CHECK(g.selectiveTree);
    g.selectiveTreeBallCategories = false;   // the plain candidates; the categories below
    g.timeLimit = 0;
    g.selectiveTreeSimulations = 40;
    g.selectiveTreeReuse = false;
    for (const char* text : {"b 29 1-0 f10 e2/e4 g11/g12 e11/e12 d3/c5 b5/b7 e14/f16 e10/f11 @B2 t:B",
                             "b 3 0-0 g12/f11 f13/d11 i13/i11 g7/f7 f12 h11/i12 c12 f16 @W4 t:W"}) {
        GameState s;
        CHECK(ParsePosition(text, &s));
        bot::GoalDfsSearch a(g), b(g);
        const bot::GoalSearchResult ra = a.Search(s);
        GameState other = s;
        other.rng = Rng(12345);
        const bot::GoalSearchResult rb = b.Search(other);
        CHECK(ra.turnPlan.size() == rb.turnPlan.size());
        for (std::size_t i = 0; i < ra.turnPlan.size() && i < rb.turnPlan.size(); ++i)
            CHECK(ra.turnPlan[i].type == rb.turnPlan[i].type && ra.turnPlan[i].player == rb.turnPlan[i].player &&
                  ra.turnPlan[i].target == rb.turnPlan[i].target);
        CHECK(ra.value.lower == rb.value.lower);
        CHECK(!ra.turnPlan.empty() && ra.action.type == ra.turnPlan.front().type);
        // The plan is legal: certain actions in order, the last may be random.
        GameState t = s;
        for (std::size_t i = 0; i < ra.turnPlan.size(); ++i) {
            CHECK(Validate(t, ra.turnPlan[i]) == ActionError::None);
            const std::vector<Outcome> outs = OutcomesUnchecked(t, ra.turnPlan[i]);
            CHECK(outs.size() == 1 || i + 1 == ra.turnPlan.size());
            ApplyUnchecked(t, ra.turnPlan[i], outs.front());
        }
        bool found = false;
        for (const bot::GoalRootMove& m : ra.moves)
            if (m.opened && m.bounds.lower == ra.value.lower) found = true;
        CHECK(found);
        CHECK(ra.value.lower >= -1 && ra.value.lower <= 1);
    }
    // W1 alone in front of the goal: a goal is nearly certain (a shot now, or
    // one step closer and a shot next turn - the beam tree prefers that too).
    {
        GameState s = Position("w 30 0-0 f17 a10 b10 c10 k10 k11 k12 k13 @W1");
        bot::GoalDfsSearch search(g);
        const bot::GoalSearchResult r = search.Search(s);
        CHECK(r.value.lower > 0.9);
    }
    // Transpositions: one node per state (the same turn made by one player's
    // move and then the rest, or all at once); without them the same search
    // stays legal and deterministic.
    {
        GameState s;
        CHECK(ParsePosition("b 3 0-0 e12/f11 d13/d11 f13/h11 e8/f7 f12 h13/i12 c12 f16 @W4 t:W", &s));
        bot::GoalDfsOptions k = g;
        k.selectiveTreeSimulations = 120;
        bot::GoalDfsSearch with(k);
        const bot::GoalSearchResult r = with.Search(s);
        const std::size_t at = r.note.find(" transpositions,");
        CHECK(at != std::string::npos);
        if (at != std::string::npos) {
            const std::size_t open = r.note.rfind('(', at);
            CHECK(std::atol(r.note.c_str() + open + 1) > 0);
        }
        k.selectiveTreeTranspositions = false;
        bot::GoalDfsSearch a(k), b(k);
        const bot::GoalSearchResult ra = a.Search(s), rb = b.Search(s);
        CHECK(ra.value.lower == rb.value.lower && ra.turnPlan.size() == rb.turnPlan.size());
        CHECK(ra.note.find("(0 transpositions,") != std::string::npos);
        GameState t = s;
        for (std::size_t i = 0; i < ra.turnPlan.size(); ++i) {
            CHECK(Validate(t, ra.turnPlan[i]) == ActionError::None);
            ApplyUnchecked(t, ra.turnPlan[i], OutcomesUnchecked(t, ra.turnPlan[i]).front());
        }
    }
    // The ball's categories (7.69): candidates added, the plan legal.
    {
        const bot::BotPreset* cat = bot::FindBotPreset("goal-sel-tree-cat-b8-t90");
        CHECK(cat != nullptr && cat->options.search.selectiveTreeBallCategories);
        if (cat) {
            bot::GoalDfsOptions k = cat->options.search;
            k.timeLimit = 0;
            k.selectiveTreeSimulations = 30;
            GameState s;
            CHECK(ParsePosition("w 4 0-0 e12/f11 d13/d11 f13/h11 e8/f7 e11/f12 h13 c12 f16 @W4 t:W", &s));
            bot::GoalDfsSearch search(k);
            const bot::GoalSearchResult r = search.Search(s);
            CHECK(search.Stats().selectiveCategoryEdges > 0);
            GameState t = s;
            for (std::size_t i = 0; i < r.turnPlan.size(); ++i) {
                CHECK(Validate(t, r.turnPlan[i]) == ActionError::None);
                ApplyUnchecked(t, r.turnPlan[i], OutcomesUnchecked(t, r.turnPlan[i]).front());
            }
        }
    }
    // The tree is kept: the same position again continues the same tree.
    {
        bot::GoalDfsOptions k = g;
        k.selectiveTreeReuse = true;
        GameState s;
        CHECK(ParsePosition("b 29 1-0 f10 e2/e4 g11/g12 e11/e12 d3/c5 b5/b7 e14/f16 e10/f11 @B2 t:B", &s));
        bot::GoalDfsSearch search(k);
        search.Search(s);
        const bot::GoalSearchResult r = search.Search(s);
        CHECK(r.note.find("(40 reused)") != std::string::npos);
    }
}

// The free ball by the race from the state as it is (bot_design.md, 7.71):
// tree_match game 1 after 13.W >e18, Black to move, the ball rolling on e18.
// The race gives the ball to Black (B3 in c18); the step preview played Black's
// End Turn without its move and gave it to White (0.97 for White).
static void TestRaceDefence() {
    const bot::BotPreset* ep = bot::FindBotPreset("goal-sel-run-ep-b8-t90");
    const bot::BotPreset* fb = bot::FindBotPreset("goal-sel-run-fb-b8-t90");
    CHECK(ep && fb && fb->options.search.selectiveRaceDefence);
    if (!ep || !fb) return;
    GameState s;
    CHECK(ParsePosition("b 13 0-0 j16 h18 h13 e14 a7 i15/j13 f14/f13 g16/g15 @e18,-2,0,0 t:Wp", &s));
    bot::GoalDfsSearch a(ep->options.search), b(fb->options.search);
    CHECK(a.SelectiveEstimate(s, kWhite, false) > 0.9);
    CHECK(b.SelectiveEstimate(s, kWhite, false) < 0.1);
}

int main() {
    TestHash();
    TestBotPlays();
    TestReach();
    TestForcedPickupCertificate();
    TestNoOwnRecoveryCertificate();
    TestNextStepPickupCertificate();
    TestFairness();
    TestMirrorSymmetry();
    TestBotRegistry();
    TestAttackEpisode();
    TestRace();
    TestEpisodeTable();
    TestPossessionTable();
    TestImmediateShotUpper();
    TestHolderShotChanceThisTurn();
    TestGoalEpisodeTransitions();
    TestGoalDfsBounds();
    TestGoalTeamDfsTurnDepth();
    TestGoalTeamMovesAgainstExhaustiveLastTurn();
    TestTeamMovementEnumeration();
    TestSelectiveTree();
    TestRaceDefence();
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "ALL OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
