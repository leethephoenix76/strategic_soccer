// Command-line tool for the bot (docs/architecture.md, "ssbot"):
//   ssbot bots                                       the collection of bots (bot/bot_registry.h)
//   ssbot selfplay [--white P] [--black P] [--halfturns N] [--maxactions N] [--seed S] [--out game.ssn]
//                  [--start-position POSITION] [--episode-turns N] [--trace decisions.csv]
//                  [--time S] [--wtime S] [--btime S] [--threads N] [--silent]
//                                                   a match between two players: a bot of the collection by
//                                                   name or random; --time/--threads override its settings
//   ssbot search "<position>" --bot NAME [--time S] [--turn-depth N] [--width W] [--root-as-node] [--tree-* ...] [--race-defence]
//                                                   one search of a selective bot: its note, plan and value
//   ssbot estimate "<position>" --bot NAME [--race-defence]
//                                                   the static estimate (the leaf value) of a selective bot
//   ssbot estimate "<position>" ... also prints the leaf as the search values it and the position
//   ssbot episode-value "<position>" [--turn-depth N] [--expansions N] [--floor P]
//                                                   the episode table: minimax over the episode's participants
//   ssbot table-bench <dump> [--turn-depth N] [--expansions N]
//                                                   recompute episode tables dumped by search --table-dump
//   (search: --table-dump FILE; goal-team-moves: --frozen MASK, --shot-only; --then: --show-outcomes;
//    attack-episode: --episode-race)
//   ssbot goal-static "<position>"                   diagnostic components of the static estimate
//   ssbot attack-episode "<position>"                the attack episode by the race to the goal (7.67)
//   ssbot goal-dfs | goal-team-dfs | goal-team-moves "<position>" [--time S] [--expansions N]
//                  [--memory-mb M] [--action-depth N | --turn-depth N] [--threads N]
//                                                   the exact search (by actions, team turns, distinct
//                                                   move-reachable states) with its root intervals
//   ssbot team-moves "<position>"                    all distinct states reached by legal move sequences
//   ssbot reach-front "<position>" [--turn-depth N]  reach waves, first arrivals and borders as JSON
//   ssbot reach-pass "<position>"                   pass targets: reach claim, immediate outcomes and ball race
//   ssbot route-static FILE [--no-loss]              route static on the reach map (id<TAB>position lines, 07.11)
//   ssbot ball-plans "<position>" [--moves "a b ..."] [--verbose] [--no-replies]
//                                                   ball plans of the holder's team and their participants
//                                                   (bot/ball_plan.h): how far they cut the movement states
//   ssbot episode-table "<position>" --attackers MASK --defenders MASK [--front] [--lead] [--turn-depth N]
//                                                   the episode table over given participants
//   ssbot route-tables "<position>" [--turn-depth R] [--lead]  the episode table over the best routes' players
//   ssbot ball-routes "<position>" [--turn-depth H]   best shot by routes with at most k interfering opponents
//   ssbot episode "<position>" [--bot NAME] [--time S] [--turn-depth N] [--frozen MASK] [--possession W]
//                                                   the danger episode by -pkr- over its participants only, keeping the ball
//   ssbot delivery "<position>" [PASSES]             routes of the ball to the danger the map finds (1 v 0,
//                                                   2 v 1) by dribble and at most PASSES passes (default 2)
//   ssbot run-border "<position>" RUNNER DEFENDER    border of a run, one runner against one defender
//                                                   (bot/run_border.h, fixed procedure), e.g. B2 W4
//   ssbot ball-chains "<position>" [--turn-depth H] [--verbose]  chains of the ball over 1..H rounds
//   ssbot episode-moves "<position>" [--turn-depth N]   the half-turns of the attack episode valued by its table
//   ssbot plan-residual "<position>" --moves "a b ... end" [--bot NAME]
//                                                   the half-turn with the plan's outside players moved by the
//                                                   residual principle (each alone, best by the leaf value)
//   ssbot replay <game.ssn>                          checks a recorded game, prints the final position
//   ssbot positions <game.ssn>                       prints each recorded team-turn start position
//   ssbot stats <game.ssn>...                        statistics of recorded games per team and per player
//   ssbot ball-race "<position>" | <game.ssn>... [--verbose]
//                                                   who reaches a free ball first (bot/ball_race.h): one
//                                                   position in detail, or every free ball of the games
//   ssbot timing <game.ssn>... [--bots a,b] [--budgets 2.5,5,...] [--every N] [--max M] [--threads T] [--out f.csv]
//                                                   a bot's decision against the time per search
// Positions and games use the notation of docs/notation.md.
#include "bot/ball_plan.h"
#include "bot/ball_race.h"
#include "bot/bot.h"
#include "bot/bot_registry.h"
#include "bot/episode.h"
#include "bot/episode_value.h"
#include "bot/hash.h"
#include "bot/geometric_static.h"
#include "bot/goal_dfs_search.h"
#include "bot/reach_front.h"
#include "bot/possession_field.h"
#include "bot/route_static.h"
#include "bot/run_border.h"
#include "bot/team_moves.h"
#include "engine/notation.h"
#include "engine/rules.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <bitset>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace soccer;

namespace {

struct Options {
    int halfTurns = kHalfTurnsPerMatch;
    int maxActions = 20000;
    uint64_t seed = 1;
    std::string white = "goal-sel-run-fb-b8-t120";   // a bot of the collection | random
    std::string black = "goal-sel-run-fb-b8-t120";
    std::string out;
    std::string startPosition;
    std::string trace;
    int episodeTurns = -1;  // relative half-turn limit from a supplied position
    std::string arg;        // position or file
    double time = -1;       // --time: seconds per team turn (search: per search), < 0: the bot's own
    int threads = -1;       // --threads, < 0: the bot's own
    double whiteTime = -1, blackTime = -1;   // --wtime / --btime: seconds per team turn of one side
    bool quiet = false;                      // --silent: no progress line (for logs)
    std::string botName;                     // --bot: a bot of the collection (search, estimate)
    std::string then;                        // --then "a b ...": actions played on the position first
                                             // (search, estimate; a state inside a turn)
    // Limits of the search (the exact commands; search, selfplay: when given).
    long expansions = 0;
    std::size_t memoryBytes = 0;
    int actionDepth = 0, turnDepth = 0;
    int staticCacheBits = -1;
    int previewTop = -1;        // --preview-top, < 0: the bot's own   // --static-cache-bits, < 0: the bot's own
    // Overrides of the best-first selective tree (7.68) for ssbot search; < 0: the bot's own.
    int treeWidth = -1;
    int width = -1;            // --width W: ssbot search, the beam width (selectiveWidth) instead of the bot's own
    int lastOutcome = -1;      // --outcome K: the last --then action takes its K-th outcome (as --show-outcomes lists them)
    bool partnerCategories = false, stateDiversity = false;   // --partner-categories, --state-diversity (search)
    int ballFirstExtra = 0;    // --ball-first-extra N: ssbot search, + N ball-first decisions in the beam
    int pickupDiversity = -1;  // --pickup-diversity N: ssbot search, selectivePickupDiversity
    bool reachExtension = false; // --reach-extension: ssbot search, the episode extension also by the reach map (07.11)
    std::string diagPlan;       // --diag-plan "...": ssbot search, trace this decision through the inner node at the root (07.11)
    double possession = -1;     // --possession W: ssbot search, the possession value (scale "who scores next", 07.11)
    bool lossCheck = false;      // --loss-check: ssbot search, with --main-outcome a lost ball the reach map flags is searched (07.11)
    bool reachPrune = false;   // --reach-prune: ssbot search, passes the reach map gives to the opponent are not generated (07.11)
    bool mainOutcome = false;  // --main-outcome: ssbot search, passes and tackles searched along the main outcome only
    bool playerDiversity = false;   // --player-diversity: ssbot search, the best move of each player in the movement beam
    bool noBreakaway = false;  // --no-breakaway: ssbot search/estimate, without the 1 v 0 / 2 v 1 lower bounds in the leaves
    int refineStates = 0;      // --refine-states N: ssbot search, N movement states ranked by a short search
    bool lineage = false;      // --lineage: ssbot search, lineage diversity in the movement beam
    int moveWidth = 0;         // --move-width W: ssbot search, the movement beam per ply
    bool ballFirst = false;    // --ball-first: ssbot search, ball-first decisions are their own kind in the beam
    bool rootAsNode = false;   // --root-as-node: ssbot search, the beam root as an inner node (whole team decisions)
    double treeC = -1, treeTemp = -1, treeWidenBase = -1, treeWidenExp = -1, treeShare = -1;
    long treeSims = -1;
    bool profileTree = false;
    bool treeNoTranspositions = false;
    int treeCategories = -1;   // -1: the bot's own
    bool raceDefence = false;  // search / estimate: the free ball by the race as it is (7.71)
    bool showOutcomes = false; // --show-outcomes: print the outcomes of the --then actions
    bool profileRoot = false;  // --profile-root: search prints each root decision's seconds per iteration
    bool rootMoves = false;    // --root-moves: search prints the root decisions it searched
    bool rootSplit = false;    // --root-split: with --threads N, split the root instead of the ranking
    bool episodeRace = false;  // --episode-race: attack-episode of a free ball by the race solver
    double floor = 0;          // --floor P: episode-value wants only a value above P
    unsigned frozen = 0;       // --frozen MASK: the exact commands never move these players (bit k - player k)
    bool shotOnly = false;     // --shot-only: the exact commands value the episode's best goal probability
    std::string tableDump;     // --table-dump FILE: search appends every computed episode table
    std::string planMoves;     // --moves "a b ...": ball-plans checks the plan these actions play
    bool verbose = false;      // --verbose: ball-plans lists every plan
    bool noReplies = false;    // --no-replies: ball-plans does not count the reply states
    bool leadPasses = false;   // --lead: episode tables with passes ahead
    unsigned attackersMask = 0, defendersMask = 0;   // --attackers / --defenders MASK: episode-table
    bool front = false;        // --front: episode-table, the other opponents hold the moving front
};

std::string ReadFile(const std::string& path, bool* ok) {
    std::ifstream f(path);
    std::stringstream buffer;
    buffer << f.rdbuf();
    *ok = static_cast<bool>(f);
    return buffer.str();
}

bool ParseOptions(int argc, char** argv, int first, Options& o) {
    for (int i = first; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--expansions") o.expansions = std::atol(next("--expansions"));
        else if (a == "--memory-mb") o.memoryBytes = std::strtoull(next("--memory-mb"), nullptr, 10) * 1024u * 1024u;
        else if (a == "--action-depth") o.actionDepth = std::atoi(next("--action-depth"));
        else if (a == "--turn-depth") o.turnDepth = std::atoi(next("--turn-depth"));
        else if (a == "--static-cache-bits") o.staticCacheBits = std::atoi(next("--static-cache-bits"));
        else if (a == "--preview-top") o.previewTop = std::atoi(next("--preview-top"));
        else if (a == "--bot") o.botName = next("--bot");
        else if (a == "--then") o.then = next("--then");
        else if (a == "--outcome") o.lastOutcome = std::atoi(next("--outcome"));
        else if (a == "--tree-width") o.treeWidth = std::atoi(next("--tree-width"));
        else if (a == "--width") o.width = std::atoi(next("--width"));
        else if (a == "--root-as-node") o.rootAsNode = true;
        else if (a == "--ball-first") o.ballFirst = true;
        else if (a == "--ball-first-extra") o.ballFirstExtra = std::atoi(next("--ball-first-extra"));
        else if (a == "--lineage") o.lineage = true;
        else if (a == "--no-breakaway") o.noBreakaway = true;
        else if (a == "--pickup-diversity") o.pickupDiversity = std::atoi(next("--pickup-diversity"));
        else if (a == "--player-diversity") o.playerDiversity = true;
        else if (a == "--main-outcome") o.mainOutcome = true;
        else if (a == "--reach-prune") o.reachPrune = true;
        else if (a == "--reach-extension") o.reachExtension = true;
        else if (a == "--loss-check") o.lossCheck = true;
        else if (a == "--possession") o.possession = std::atof(next("--possession"));
        else if (a == "--diag-plan") o.diagPlan = next("--diag-plan");
        else if (a == "--refine-states") o.refineStates = std::atoi(next("--refine-states"));
        else if (a == "--move-width") o.moveWidth = std::atoi(next("--move-width"));
        else if (a == "--partner-categories") o.partnerCategories = true;
        else if (a == "--state-diversity") o.stateDiversity = true;
        else if (a == "--tree-c") o.treeC = std::atof(next("--tree-c"));
        else if (a == "--tree-temp") o.treeTemp = std::atof(next("--tree-temp"));
        else if (a == "--tree-widen") {
            o.treeWidenBase = std::atof(next("--tree-widen"));
            o.treeWidenExp = std::atof(next("--tree-widen"));
        }
        else if (a == "--tree-share") o.treeShare = std::atof(next("--tree-share"));
        else if (a == "--tree-sims") o.treeSims = std::atol(next("--tree-sims"));
        else if (a == "--profile-tree") o.profileTree = true;
        else if (a == "--tree-no-transpositions") o.treeNoTranspositions = true;
        else if (a == "--tree-categories") o.treeCategories = 1;
        else if (a == "--tree-no-categories") o.treeCategories = 0;
        else if (a == "--race-defence") o.raceDefence = true;
        else if (a == "--show-outcomes") o.showOutcomes = true;
        else if (a == "--profile-root") o.profileRoot = true;
        else if (a == "--root-split") o.rootSplit = true;
        else if (a == "--root-moves") o.rootMoves = true;
        else if (a == "--episode-race") o.episodeRace = true;
        else if (a == "--floor") o.floor = std::atof(next("--floor"));
        else if (a == "--frozen") o.frozen = static_cast<unsigned>(std::strtoul(next("--frozen"), nullptr, 0));
        else if (a == "--shot-only") o.shotOnly = true;
        else if (a == "--table-dump") o.tableDump = next("--table-dump");
        else if (a == "--moves") o.planMoves = next("--moves");
        else if (a == "--verbose") o.verbose = true;
        else if (a == "--no-replies") o.noReplies = true;
        else if (a == "--lead") o.leadPasses = true;
        else if (a == "--front") o.front = true;
        else if (a == "--attackers") o.attackersMask = static_cast<unsigned>(std::strtoul(next("--attackers"), nullptr, 0));
        else if (a == "--defenders") o.defendersMask = static_cast<unsigned>(std::strtoul(next("--defenders"), nullptr, 0));
        else if (a == "--halfturns") o.halfTurns = std::atoi(next("--halfturns"));
        else if (a == "--episode-turns") o.episodeTurns = std::atoi(next("--episode-turns"));
        else if (a == "--start-position") o.startPosition = next("--start-position");
        else if (a == "--trace") o.trace = next("--trace");
        else if (a == "--time") o.time = std::atof(next("--time"));
        else if (a == "--threads") o.threads = std::atoi(next("--threads"));
        else if (a == "--silent") o.quiet = true;
        else if (a == "--wtime") o.whiteTime = std::atof(next("--wtime"));
        else if (a == "--btime") o.blackTime = std::atof(next("--btime"));
        else if (a == "--maxactions") o.maxActions = std::atoi(next("--maxactions"));
        else if (a == "--seed") o.seed = std::strtoull(next("--seed"), nullptr, 10);
        else if (a == "--white") o.white = next("--white");
        else if (a == "--black") o.black = next("--black");
        else if (a == "--out") o.out = next("--out");
        else if (!a.empty() && a[0] != '-' && o.arg.empty()) o.arg = a;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

std::string ActionText(const GameState& s, const Action& a) {
    return a.type == ActionType::EndTurn ? "end" : FormatAction(s, a);
}

bool ReadPosition(const Options& o, GameState* s) {
    std::string error;
    if (!ParsePosition(o.arg, s, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return false;
    }
    // --then: actions in notation ("end" ends the turn), each with its
    // untouched outcome (the first one otherwise).
    std::size_t at = 0;
    while (at < o.then.size()) {
        const std::size_t space = o.then.find(' ', at);
        const std::string text = o.then.substr(at, space == std::string::npos ? std::string::npos : space - at);
        at = space == std::string::npos ? o.then.size() : space + 1;
        if (text.empty()) continue;
        Action a;
        if (text == "end") a = Action::EndTurn();
        else if (!ParseAction(*s, text, &a)) {
            std::fprintf(stderr, "--then: cannot play %s\n", text.c_str());
            return false;
        }
        const std::vector<Outcome> outcomes = OutcomesUnchecked(*s, a);
        if (o.showOutcomes)
            for (const Outcome& x : outcomes) {
                GameState c = *s; ApplyUnchecked(c, a, x);
                std::fprintf(stderr, "outcome of %s: kind %d p %.4f -> %s\n", text.c_str(), static_cast<int>(x.kind), x.probability, FormatPosition(c).c_str());
            }
        Outcome chosen = outcomes.front();
        for (const Outcome& x : outcomes)
            if (x.kind == OutcomeKind::BallThrough) chosen = x;
        if (o.lastOutcome >= 0 && at >= o.then.size()) {   // --outcome K: the last action's K-th outcome
            if (o.lastOutcome >= static_cast<int>(outcomes.size())) {
                std::fprintf(stderr, "--outcome %d: %s has %d outcomes\n", o.lastOutcome, text.c_str(),
                             static_cast<int>(outcomes.size()));
                return false;
            }
            chosen = outcomes[o.lastOutcome];
        }
        ApplyUnchecked(*s, a, chosen);
    }
    return true;
}

// The limits given on the command line, over a bot's search options.
void ApplyLimits(const Options& o, bot::GoalDfsOptions* g) {
    if (o.expansions > 0) g->expansions = o.expansions;
    if (o.memoryBytes > 0) g->maxMemoryBytes = o.memoryBytes;
    if (o.actionDepth > 0) g->maxActionDepth = o.actionDepth;
    if (o.turnDepth > 0) g->maxTurnDepth = o.turnDepth;
    if (o.staticCacheBits >= 0) g->selectiveStaticCacheBits = o.staticCacheBits;
    if (o.previewTop >= 0) g->selectiveEndTurnPreviewTop = o.previewTop;
}

void PrintRootMoves(const GameState& s, const bot::GoalSearchResult& r) {
    std::printf("chosen: %s\n", ActionText(s, r.action).c_str());
    for (std::size_t i = 0; i < r.moves.size() && i < 24; ++i) {
        const bot::GoalRootMove& move = r.moves[i];
        std::printf("  [%.6f, %.6f] %c %s\n", move.bounds.lower, move.bounds.upper,
                    move.opened ? '*' : ' ', ActionText(s, move.action).c_str());
    }
    if (r.moves.size() > 24) std::printf("  ... %zu more legal actions\n", r.moves.size() - 24);
}

// The exact search (selectiveWidth 0) of a position.
int ExactCommand(const Options& o, bool completeTurns, bool groupMovements) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    bot::GoalDfsOptions g;
    if (o.time >= 0) g.timeLimit = o.time;
    if (o.threads >= 0) g.threads = o.threads;
    ApplyLimits(o, &g);
    g.completeTurns = completeTurns;
    g.groupMovements = groupMovements;
    g.frozenPlayers = o.frozen;
    if (o.frozen && !groupMovements) std::fprintf(stderr, "--frozen applies to goal-team-moves only\n");
    bot::SetEpisodeShotOnly(o.shotOnly);
    bot::GoalDfsSearch search(g);
    const bot::GoalSearchResult r = search.Search(s);
    std::printf("%s\nroot [%.6f, %.6f], %zu legal actions\n", r.note.c_str(),
                r.rootBounds.lower, r.rootBounds.upper, r.moves.size());
    const bot::GoalDfsStats stats = search.Stats();
    std::printf("expansion mix: move %ld, pass %ld, shot %ld, attack %ld, end-turn %ld "
                "(ball step %ld), place %ld; outcomes processed %ld\n",
                stats.moveExpansions, stats.passExpansions, stats.shotExpansions,
                stats.attackExpansions, stats.endTurnExpansions, stats.ballStepExpansions,
                stats.placeExpansions, stats.outcomesProcessed);
    PrintRootMoves(s, r);
    return 0;
}

const bot::BotPreset* SelectiveBot(const Options& o, const char* command) {
    const bot::BotPreset* preset = bot::FindBotPreset(o.botName);
    if (!preset || preset->options.search.selectiveWidth <= 0) {
        std::fprintf(stderr, "%s: --bot must name a selective bot of the collection (ssbot bots)\n", command);
        return nullptr;
    }
    return preset;
}

// ssbot search "<position>" --bot NAME [--time S] [--turn-depth N] [--tree-width W] [--tree-c C]
//     [--tree-temp T] [--tree-widen B E] [--tree-share F] [--tree-sims N] [--tree-no-transpositions]
//     [--tree-categories | --tree-no-categories] [--race-defence]
// One search of a selective bot of the collection (goal-sel-*) on a position:
// its note, the chosen plan and its value; the --tree-* options override the
// best-first tree's constants (docs/bot_design.md, 7.68).
int SearchCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const bot::BotPreset* preset = SelectiveBot(o, "search");
    if (!preset) return 2;
    bot::GoalDfsOptions g = preset->options.search;
    g.timeLimit = o.time >= 0 ? o.time : preset->options.timeLimit;
    if (o.threads >= 0) g.threads = o.threads;
    ApplyLimits(o, &g);
    if (o.width > 0) g.selectiveWidth = o.width;
    if (o.rootAsNode) g.selectiveRootAsNode = true;
    if (o.ballFirst) g.selectiveBallFirstKinds = true;
    if (o.moveWidth > 0) g.selectiveMovementWidth = o.moveWidth;
    if (o.lineage) g.selectiveLineageDiversity = true;
    if (o.pickupDiversity >= 0) g.selectivePickupDiversity = o.pickupDiversity;
    if (o.playerDiversity) g.selectivePlayerDiversity = true;
    if (o.mainOutcome) g.selectiveMainOutcome = true;
    if (o.reachPrune) g.selectiveReachPassPrune = true;
    if (o.reachExtension) g.selectiveReachExtension = true;
    if (o.lossCheck) g.selectiveLossReachCheck = true;
    if (o.possession >= 0) g.selectivePossessionValue = o.possession;
    if (o.frozen) {
        // The episode's participants only (08.11.2026): the others stand;
        // the attack episode's own outsiders' rule is off.
        g.frozenPlayers = o.frozen;
        g.selectiveAttackEpisode = false;
        g.selectiveEpisodeExits = false;
    }
    if (!o.diagPlan.empty()) {
        // The decision's moves up to its first ball action (or the end of the turn).
        bot::NodeDiagTarget target;
        target.active = true;
        target.root = s;
        GameState t = s;
        std::stringstream words(o.diagPlan);
        std::string w;
        target.action = Action::EndTurn();
        while (words >> w) {
            Action a;
            if (w == "end") break;
            if (!ParseAction(t, w, &a)) { std::fprintf(stderr, "--diag-plan: cannot play %s\n", w.c_str()); return 1; }
            if (a.type != ActionType::Move) { target.action = a; break; }
            ApplyUnchecked(t, a, Outcome::Certain());
            target.prefixes.push_back(t);
        }
        bot::SetNodeDiag(target);
    }
    if (o.noBreakaway) { g.selectiveBreakawayBound = false; g.selectiveTwoVsOneBound = false; }
    if (o.refineStates > 0) g.selectiveRefineStates = o.refineStates;
    if (o.ballFirstExtra > 0) g.selectiveBallFirstExtra = o.ballFirstExtra;
    if (o.partnerCategories) g.selectivePartnerCategories = true;
    if (o.stateDiversity) g.selectiveStateDiversity = true;
    if (o.treeWidth >= 0) g.selectiveTreeWidth = o.treeWidth;
    if (o.treeC >= 0) g.selectiveTreeCPuct = o.treeC;
    if (o.treeTemp > 0) g.selectiveTreeTemperature = o.treeTemp;
    if (o.treeWidenBase > 0) g.selectiveTreeWidenBase = o.treeWidenBase;
    if (o.treeWidenExp >= 0) g.selectiveTreeWidenExponent = o.treeWidenExp;
    if (o.treeShare >= 0) g.selectiveTreeFinalShare = o.treeShare;
    if (o.treeNoTranspositions) g.selectiveTreeTranspositions = false;
    if (o.treeCategories >= 0) g.selectiveTreeBallCategories = o.treeCategories > 0;
    if (o.raceDefence) g.selectiveRaceDefence = true;
    g.tableDumpPath = o.tableDump;
    g.profileRoot = o.profileRoot;
    g.selectiveRootSplit = o.rootSplit;
    if (o.treeSims > 0) {
        g.selectiveTreeSimulations = o.treeSims;
        if (o.time < 0) g.timeLimit = 0;
    }
    if (o.profileTree && !g.selectiveTree) {
        std::fprintf(stderr, "--profile-tree requires a tree bot\n");
        return 2;
    }
    g.profileTree = o.profileTree;
    bot::GoalDfsSearch search(g);
    const bot::GoalSearchResult r = search.Search(s);
    GameState t = s;
    std::string plan;
    for (const Action& a : r.turnPlan) {
        plan += (plan.empty() ? "" : " ") + ActionText(t, a);
        const std::vector<Outcome> outs = OutcomesUnchecked(t, a);
        ApplyUnchecked(t, a, outs.front());
    }
    if (o.rootMoves)
        for (const bot::GoalRootMove& move : r.moves)
            if (move.opened)
                std::printf("  root [%.4f, %.4f] %s\n", move.bounds.lower, move.bounds.upper, ActionText(s, move.action).c_str());
    // value: the attacking team's; eval: White's (positive - White is better).
    const double eval = s.ball.attackTeam == kWhite ? r.value.lower : -r.value.lower;
    std::printf("%s\n%s: plan %s, value %.4f, eval %+.4f, depth %d, %.1f s\n", r.note.c_str(), o.botName.c_str(),
                plan.c_str(), r.value.lower, eval, search.Stats().completedTurnDepth, r.seconds);
    return 0;
}

// ssbot estimate "<position>" --bot NAME [--race-defence]
// The static estimate of a selective bot of the collection (its leaf value) for
// the position's attacking team: as it stands and after EndTurn.
int EstimateCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const bot::BotPreset* preset = SelectiveBot(o, "estimate");
    if (!preset) return 2;
    bot::GoalDfsOptions g = preset->options.search;
    if (o.raceDefence) g.selectiveRaceDefence = true;
    if (o.possession >= 0) g.selectivePossessionValue = o.possession;
    bot::GoalDfsSearch search(g);
    std::printf("static %.6f, after EndTurn %.6f (for %s)\n",
                search.SelectiveEstimate(s, s.ball.attackTeam, false),
                search.SelectiveEstimate(s, s.ball.attackTeam, true),
                s.ball.attackTeam == kWhite ? "White" : "Black");
    std::printf("position %s\n", FormatPosition(s).c_str());
    std::printf("leaf %.6f, leaf after EndTurn %.6f\n",
                search.SelectiveLeafEstimate(s, s.ball.attackTeam, false),
                search.SelectiveLeafEstimate(s, s.ball.attackTeam, true));
    return 0;
}

int GoalStaticCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const bot::GoalStaticSummary value = bot::EvaluateGoalStatic(s);
    std::printf("attack %.9f direct-pass %.9f lead-pass %.9f keep %.9f counter %.9f\n",
                value.attackThreat, value.directPassThreat, value.leadPassThreat,
                value.keepChance, value.counterThreat);
    return 0;
}

// The attack episode by the race to the goal (bot/episode.h, bot_design.md 7.67).
int AttackEpisodeCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const bot::AttackEpisode e = bot::AnalyzeAttackEpisode(s, true, o.episodeRace);
    std::printf("episode %s", bot::AttackEpisodeName(e.kind));
    if (e.carrier != kNoPlayer) std::printf(", carrier %s (ball at ply %d, best shooting cell at ply %d)", PlayerName(e.carrier).c_str(), e.carrierPly, e.endPly);
    std::printf("\n");
    for (int k = 0; k < kNumPlayers; ++k)
        std::printf("  %s %-8s %s\n", PlayerName(k).c_str(),
                    e.attackers >> k & 1u ? "attack" : e.defenders >> k & 1u ? "defence" : "outside",
                    e.reason[k].c_str());
    GameState t = s;
    std::printf("outsiders of the team to move:");
    for (const Action& a : bot::OutsiderMoves(s, e)) {
        std::printf(" %s", FormatAction(t, a).c_str());
        ApplyUnchecked(t, a, Outcome::Certain());
    }
    std::printf("\n");
    return 0;
}

// ssbot episode-value "<position>" [--turn-depth N] [--expansions N]
// The attack episode's value by the minimax over its participants (episode_value.h);
// a free ball taken by the race solver is valued from the taken state.
int EpisodeValueCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    if (s.phase == Phase::Play && s.ball.IsFree()) {
        const bot::BallRaceResult r = bot::SolveBallRace(s);
        if (r.kind != bot::BallRaceKind::Taken) { std::printf("free ball not taken by the race solver\n"); return 0; }
        s = bot::BallRaceTakenState(s, r);
        std::printf("taken: %s\n", FormatPosition(s).c_str());
    }
    const bot::AttackEpisode e = bot::AnalyzeAttackEpisode(s, false, true);
    std::printf("episode %s, attackers", bot::AttackEpisodeName(e.kind));
    for (int k = 0; k < kNumPlayers; ++k) if (e.attackers >> k & 1u) std::printf(" %s", PlayerName(k).c_str());
    std::printf(", defenders");
    for (int k = 0; k < kNumPlayers; ++k) if (e.defenders >> k & 1u) std::printf(" %s", PlayerName(k).c_str());
    std::printf("\n");
    const int maxTurns = o.turnDepth > 0 ? o.turnDepth : 6;
    for (int turns = 1; turns <= maxTurns; ++turns) {
        bot::EpisodeValueOptions eo;
        eo.turns = turns;
        if (o.expansions > 0) eo.nodeLimit = o.expansions;
        if (o.floor > 0) eo.floor = o.floor;
        const auto start = std::chrono::steady_clock::now();
        const bot::EpisodeValueResult r = bot::EpisodeValue(s, e.attackers, e.defenders, eo);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double eval = e.team == kWhite ? r.value : -r.value;
        std::printf("turns %d: value %.4f (eval %+.4f), nodes %ld, movement states %ld, %s, %.2f s\n", turns, r.value,
                    eval, r.nodes, r.movementStates, r.complete ? "complete" : "STOPPED", sec);
        std::fflush(stdout);
        if (!r.complete) break;
    }
    return 0;
}

// ssbot episode-moves "<position>" [--turn-depth N]
// The team to move's half-turns in its attack episode (participants only, the
// others standing), each valued by the episode table after it: moves of the
// participants, then hold, shot or any legal pass - to a partner's feet or
// into space (08.11.2026) - by all its outcomes; the table follows a free
// ball (lead passes on). The best ones are listed.
int EpisodeMovesCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const bot::AttackEpisode e = bot::AnalyzeAttackEpisode(s, false, true);
    if (e.kind == bot::AttackEpisodeKind::None || e.team != s.teamToMove || s.ball.IsFree()) {
        std::printf("no attack episode of the team to move with the ball\n");
        return 0;
    }
    const int turns = o.turnDepth > 0 ? o.turnDepth : 5;
    const unsigned frozen = ~(e.attackers | e.defenders) & ((1u << kNumPlayers) - 1);
    const bot::GoalEpisode ge{s.ball.attackTeam, bot::GoalEpisodeStatus::OurAttack};
    const bot::TeamMovementList list = bot::EnumerateTeamMovements(s, ge, 0, {}, frozen);
    struct Entry { double value; std::string text; };
    std::vector<Entry> entries;
    bot::EpisodeValueOptions eo;
    eo.turns = turns;
    eo.leadPasses = true;   // a free ball goes on in the table (08.11.2026)
    // After the end of the turn, over its outcomes: a ball held by an attacker
    // or free is valued by the table, a ball the defence has is worth 0.
    auto after = [&](const GameState& u) {
        double v = 0;
        for (const Outcome& o : OutcomesUnchecked(u, Action::EndTurn())) {
            GameState t = u;
            ApplyUnchecked(t, Action::EndTurn(), o);
            if (t.phase != Phase::Play) continue;
            if (!t.ball.IsFree() && !((e.attackers >> t.ball.holder) & 1u)) continue;
            v += o.probability * bot::EpisodeValue(t, e.attackers, e.defenders, eo).value;
        }
        return v;
    };
    long pruned = 0;
    for (const bot::TeamMovementState& st : list.states) {
        if (st.state.ball.IsFree() || TeamOf(st.state.ball.holder) != s.teamToMove) continue;
        GameState replay = s;
        std::string moves;
        for (int j = 0; j < st.count; ++j) {
            moves += ActionText(replay, st.moves[j]) + " ";
            ApplyUnchecked(replay, st.moves[j], Outcome::Certain());
        }
        const int h = st.state.ball.holder;
        entries.push_back({after(st.state), moves + "end"});
        entries.push_back({ShotProbability(st.state, h), moves + "#"});
        // Every legal pass - to a partner's feet or into space (08.11.2026) -
        // over all its outcomes; not those the reach map gives to the opponent first.
        const bot::ReachFront front = bot::ComputeReachFront(st.state);
        for (const Action& pass : LegalActions(st.state)) {
            if (pass.type != ActionType::Pass) continue;
            if (PlayerAt(st.state, pass.target) == kNoPlayer && front.stop != bot::ReachFrontStop::InvalidStart &&
                bot::ReachPassTaker(st.state, front, pass) == Opponent(s.teamToMove)) { ++pruned; continue; }
            double v = 0;
            for (const Outcome& out : OutcomesUnchecked(st.state, pass)) {
                GameState t = st.state;
                ApplyUnchecked(t, pass, out);
                if (!t.ball.IsFree() && !((e.attackers >> t.ball.holder) & 1u)) continue;   // the defence has it
                v += out.probability * after(t);
            }
            entries.push_back({v, moves + ActionText(st.state, pass) + " end"});
        }
    }
    if (pruned) std::printf("%ld passes the reach map gives to the opponent first: not tried\n", pruned);
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.value > b.value; });
    std::printf("%zu half-turns of the episode (%s), table %d turns after them\n", entries.size(),
                bot::AttackEpisodeName(e.kind), turns);
    for (std::size_t i = 0; i < entries.size() && i < 15; ++i)
        std::printf("  %.4f  %s\n", entries[i].value, entries[i].text.c_str());
    return 0;
}

std::string Mask(unsigned bits);
int Popcount(unsigned bits);

// ssbot ball-chains "<position>" [--turn-depth H] [--verbose]
// Chains of the ball over 1..H rounds (bot/ball_plan.h) and their participants:
// how the number of opponents who can interfere grows with the depth.
int BallChainsCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const int maxRounds = o.turnDepth > 0 ? o.turnDepth : 4;
    std::printf("rounds  chains  shots   opponents 0/1/2/3/4 (all chains)   sets   best open shot with <= k opponents (k=0..4)\n");
    for (int rounds = 1; rounds <= maxRounds; ++rounds) {
        const std::vector<bot::BallChain> chains = bot::AnalyzeBallChains(s, rounds);
        if (chains.empty()) { std::printf("no chains: the team to move does not hold the ball\n"); return 0; }
        long hist[5] = {0, 0, 0, 0, 0}, shots = 0;
        double best[5] = {0, 0, 0, 0, 0};
        std::vector<unsigned> sets;
        for (const bot::BallChain& c : chains) {
            const int n = Popcount(c.opponent);
            ++hist[n];
            const unsigned set = c.own | c.opponent;
            if (std::find(sets.begin(), sets.end(), set) == sets.end()) sets.push_back(set);
            if (c.shot) {
                ++shots;
                for (int k = n; k <= 4; ++k) best[k] = std::max(best[k], c.shotUpper);
            }
        }
        std::printf("%6d  %6zu  %5ld   %5ld %5ld %5ld %5ld %5ld   %6zu   %.2f %.2f %.2f %.2f %.2f\n", rounds, chains.size(),
                    shots, hist[0], hist[1], hist[2], hist[3], hist[4], sets.size(), best[0], best[1], best[2], best[3],
                    best[4]);
        if (o.verbose && rounds == maxRounds) {
            std::vector<const bot::BallChain*> sorted;
            for (const bot::BallChain& c : chains) sorted.push_back(&c);
            std::stable_sort(sorted.begin(), sorted.end(), [](const bot::BallChain* a, const bot::BallChain* b) {
                const int na = Popcount(a->opponent), nb = Popcount(b->opponent);
                if (na != nb) return na < nb;
                return a->shotUpper > b->shotUpper;
            });
            for (std::size_t i = 0; i < sorted.size() && i < 40; ++i)
                std::printf("    %-40s own %-12s opp %-14s%s\n", sorted[i]->text.c_str(), Mask(sorted[i]->own).c_str(),
                            Mask(sorted[i]->opponent).c_str(),
                            sorted[i]->shot ? (" shot <= " + std::to_string(sorted[i]->shotUpper).substr(0, 4)).c_str() : "");
        }
    }
    return 0;
}

// ssbot ball-routes "<position>" [--turn-depth H]
// Routes of the ball with a moving front (bot/ball_plan.h): for every number k
// of opponents, the best open shot by the end of our half-turn r on a route on
// which at most k opponents (the named ones) can interfere.
int BallRoutesCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const int rounds = o.turnDepth > 0 ? o.turnDepth : 5;
    const auto t0 = std::chrono::steady_clock::now();
    const auto best = bot::BallRoutes(s, rounds);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const int firstOpp = FirstPlayerOf(Opponent(s.teamToMove));
    std::printf("best shot (blockers of the set at worst) by our half-turn r with at most k opponents able to interfere (%.2f s)\n", sec);
    for (int r = 0; r < rounds; ++r) {
        std::printf("r%d:", r + 1);
        for (int k = 0; k <= 4; ++k) {
            const bot::BallRouteBest* b = nullptr;
            int mask = 0;
            for (int m = 0; m < 16; ++m)
                if (Popcount(static_cast<unsigned>(m)) <= k && (!b || best[r][m].shot > b->shot)) { b = &best[r][m]; mask = m; }
            std::string who;
            for (int i = 0; i < 4; ++i)
                if (mask >> i & 1) who += (who.empty() ? "" : ",") + PlayerName(firstOpp + i);
            if (b->shot > 0)
                std::printf("  k%d %.2f %s %s(%s)", k, b->shot, PlayerName(b->carrier).c_str(), CellName(b->cell).c_str(),
                            who.empty() ? "-" : who.c_str());
            else
                std::printf("  k%d -", k);
        }
        std::printf("\n");
    }
    // The routes of the last round's bests.
    for (int k = 0; k <= 4; ++k) {
        const bot::BallRouteBest* b = nullptr;
        for (int m = 0; m < 16; ++m)
            if (Popcount(static_cast<unsigned>(m)) <= k && (!b || best[rounds - 1][m].shot > b->shot)) b = &best[rounds - 1][m];
        if (b && b->shot > 0) std::printf("route k%d: %s\n", k, b->route.c_str());
    }
    return 0;
}

// ssbot route-tables "<position>" [--turn-depth R] [--expansions N]
// For the best ball routes (ball-routes) with at most k interfering opponents,
// by our half-turn r: the episode table (episode_value.h) over the route's
// carriers and those opponents, 2r+1 team turns; the other opponents stand
// but hold the moving front: the ball may not go where they can be by then.
int RouteTablesCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const int rounds = o.turnDepth > 0 ? o.turnDepth : 3;
    const auto best = bot::BallRoutes(s, rounds);
    const int firstOpp = FirstPlayerOf(Opponent(s.teamToMove));
    std::vector<std::string> done;
    std::printf("half-turn k  route bound  carriers / opponents        table (turns)  nodes  time\n");
    for (int r = 0; r < rounds; ++r)
        for (int k = 0; k <= 4; ++k) {
            const bot::BallRouteBest* b = nullptr;
            int mask = 0;
            for (int m = 0; m < 16; ++m)
                if (Popcount(static_cast<unsigned>(m)) <= k && (!b || best[r][m].shot > b->shot)) { b = &best[r][m]; mask = m; }
            if (!b || b->shot <= 0) continue;
            unsigned defenders = 0;
            for (int i = 0; i < 4; ++i)
                if (mask >> i & 1) defenders |= 1u << (firstOpp + i);
            // At most two attackers (the holder and one partner of the route: 2 v k is
            // enough, user's decision 02.11.2026): each pair is a restriction of the
            // attack, the best of them is the value (a lower bound of the route's set).
            const unsigned holderBit = 1u << s.ball.holder;
            const unsigned carriers = b->own | holderBit;
            std::vector<unsigned> pairs;
            for (int q = 0; q < kNumPlayers; ++q)
                if ((carriers >> q & 1u) && q != s.ball.holder) pairs.push_back(holderBit | (1u << q));
            if (pairs.empty()) pairs.push_back(holderBit);
            const int turns = 2 * r + 1;
            double bestValue = -1;
            unsigned bestAttackers = 0;
            bool allComplete = true;
            long states = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (unsigned attackers : pairs) {
                const std::string key = std::to_string(attackers) + "/" + std::to_string(defenders) + "/" + std::to_string(turns);
                if (std::find(done.begin(), done.end(), key) != done.end()) continue;
                done.push_back(key);
                bot::EpisodeValueOptions eo;
                eo.turns = turns;
                eo.nodeLimit = o.expansions > 0 ? o.expansions : 3000000;
                eo.frontOpponents = (0xFu << firstOpp) & ~defenders;
                eo.leadPasses = o.leadPasses;
                const bot::EpisodeValueResult v = bot::EpisodeValue(s, attackers, defenders, eo);
                states += v.movementStates;
                allComplete = allComplete && v.complete;
                if (v.value > bestValue) { bestValue = v.value; bestAttackers = attackers; }
            }
            if (bestValue < 0) continue;
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (allComplete)
                std::printf("%9d %d  %11.3f  %-12s / %-12s  %.3f (%d)  %ld  %.2f s\n", r + 1, k, b->shot,
                            Mask(bestAttackers).c_str(), Mask(defenders).c_str(), bestValue, turns, states, sec);
            else   // a stopped table's value is not a value
                std::printf("%9d %d  %11.3f  %-12s / %-12s  -     (%d) STOPPED  %ld  %.2f s\n", r + 1, k, b->shot,
                            Mask(bestAttackers).c_str(), Mask(defenders).c_str(), turns, states, sec);
            std::printf("            route: %s\n", b->route.c_str());
        }
    return 0;
}

// ssbot episode-table "<position>" --attackers MASK --defenders MASK [--front] [--lead] [--turn-depth N]
// The episode table over the given participants (bit k: player k), by
// iterative turns; --front: the other opponents hold the moving front;
// --lead: passes ahead.
int EpisodeTableCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const int maxTurns = o.turnDepth > 0 ? o.turnDepth : 5;
    const unsigned opponents = 0xFu << FirstPlayerOf(Opponent(s.teamToMove));
    for (int turns = 1; turns <= maxTurns; turns += 2) {
        bot::EpisodeValueOptions eo;
        eo.turns = turns;
        eo.nodeLimit = o.expansions > 0 ? o.expansions : 20000000;
        eo.leadPasses = o.leadPasses;
        if (o.front) eo.frontOpponents = opponents & ~o.defendersMask;
        const auto t0 = std::chrono::steady_clock::now();
        const bot::EpisodeValueResult v = bot::EpisodeValue(s, o.attackersMask | (1u << s.ball.holder), o.defendersMask, eo);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("turns %d: value %.4f, nodes %ld (free ball %ld), movement states %ld, passes ahead %ld of %ld (race lost %ld), %s, %.2f s\n",
                    turns, v.value, v.nodes, v.freeNodes, v.movementStates, v.leadTried, v.leadCandidates, v.leadRaceLost,
                    v.complete ? "complete" : "STOPPED", sec);
        std::fflush(stdout);
        if (!v.complete) break;
    }
    return 0;
}

// ssbot table-bench <dump file> [--turn-depth N] [--expansions N]: recomputes the dumped
// episode tables (ssbot search --table-dump), checks the values, reports the time.
int TableBenchCommand(const Options& o) {
    std::ifstream in(o.arg);
    std::string line;
    long count = 0, mismatches = 0, states = 0, incomplete = 0;
    double total = 0, sum = 0;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::size_t at = 0;
        while (true) {
            const std::size_t bar = line.find('|', at);
            f.push_back(line.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
            if (bar == std::string::npos) break;
            at = bar + 1;
        }
        if (f.size() < 7) continue;
        GameState s;
        if (f[0].size() != 2 * sizeof(GameState)) { std::fprintf(stderr, "bad state\n"); continue; }
        unsigned char* raw = reinterpret_cast<unsigned char*>(&s);
        for (std::size_t i = 0; i < sizeof(GameState); ++i) raw[i] = static_cast<unsigned char>(std::stoi(f[0].substr(2 * i, 2), nullptr, 16));
        bot::EpisodeValueOptions eo;
        eo.turns = o.turnDepth > 0 ? o.turnDepth : 4;
        eo.nodeLimit = o.expansions > 0 ? o.expansions : 150000;
        eo.floor = std::atof(f[3].c_str());
        const auto start = std::chrono::steady_clock::now();
        const bot::EpisodeValueResult r = bot::EpisodeValue(s, std::strtoul(f[1].c_str(), nullptr, 10),
                                                            std::strtoul(f[2].c_str(), nullptr, 10), eo);
        total += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        ++count;
        states += r.movementStates;
        if (!r.complete) ++incomplete;
        const double old = std::atof(f[4].c_str());
        const bool oldComplete = f[5] == "1";
        // Only values above the floor are exact.
        const double floor = eo.floor;
        if (r.complete && oldComplete) {
            const bool a = r.value > floor + 1e-12, b = old > floor + 1e-12;
            if ((a || b) && std::abs(r.value - old) > 1e-9) {
                ++mismatches;
                if (mismatches <= 5) std::printf("mismatch: %s old %.9f new %.9f\n", f[0].c_str(), old, r.value);
            }
        }
        if (r.complete && r.value > floor) sum += r.value;
    }
    std::printf("%ld tables, %.3f s (%.2f ms each), %ld movement states, %ld incomplete, %ld mismatches, sum %.6f\n",
                count, total, count ? 1000 * total / count : 0.0, states, incomplete, mismatches, sum);
    return 0;
}

int TeamMovesCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const bot::GoalEpisode episode{s.ball.attackTeam, bot::GoalEpisodeStatus::OurAttack};
    const bot::TeamMovementList list = bot::EnumerateTeamMovements(s, episode);
    for (std::size_t i = 0; i < list.states.size(); ++i) {
        const bot::TeamMovementState& entry = list.states[i];
        GameState replay = s;
        std::printf("%zu:", i);
        for (int j = 0; j < entry.count; ++j) {
            std::printf(" %s", ActionText(replay, entry.moves[j]).c_str());
            ApplyUnchecked(replay, entry.moves[j], Outcome::Certain());
        }
        std::printf(" -> %s\n", FormatPosition(entry.state).c_str());
    }
    std::printf("%zu distinct states, %zu legal movement transitions, %zu duplicate states\n",
                list.states.size(), list.transitions, list.duplicates);
    return 0;
}

int ReachFrontCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const bot::ReachFront front = bot::ComputeReachFront(s, o.turnDepth > 0 ? o.turnDepth : kHalfTurnsPerMatch);
    if (front.stop == bot::ReachFrontStop::InvalidStart) {
        std::fprintf(stderr, "reach-front: expected the start of a play turn with eight distinct player cells\n");
        return 1;
    }
    const char* stop = front.stop == bot::ReachFrontStop::Full ? "full" :
        front.stop == bot::ReachFrontStop::Stable ? "stable" : "horizon";
    std::printf("{\"startsWhite\":%s,\"stop\":\"%s\",\"players\":[",
                s.teamToMove == kWhite ? "true" : "false", stop);
    for (int i = 0; i < kNumPlayers; ++i) {
        const PlayerState& p = s.players[i];
        const Cell v = p.Velocity();
        const char team = TeamOf(i) == kWhite ? 'W' : 'B';
        std::printf("%s{\"id\":\"%c%d\",\"team\":\"%c\",\"x\":%d,\"y\":%d,\"vx\":%d,\"vy\":%d}",
                    i ? "," : "", team, i % kPlayersPerTeam + 1, team, p.pos.x, p.pos.y, v.x, v.y);
    }
    std::printf("],\"steps\":[");
    for (std::size_t h = 0; h < front.frames.size(); ++h) {
        const bot::ReachFrontFrame& frame = front.frames[h];
        int occupied = 0;
        std::printf("%s{\"phase\":%zu,\"a\":[", h ? "," : "", h);
        for (int i = 0; i < bot::kReachFrontCells; ++i) {
            const unsigned mask = frame.players[i];
            occupied += mask != 0;
            std::printf("%s%u", i ? "," : "", mask);
        }
        std::printf("],\"first\":[");
        for (int i = 0; i < bot::kReachFrontCells; ++i)
            std::printf("%s%d", i ? "," : "", frame.players[i] ? front.firstArrival[i] : -1);
        std::printf("],\"occupied\":%d,\"border\":[", occupied);
        const auto borders = bot::ReachFrontBorders(frame);
        for (std::size_t i = 0; i < borders.size(); ++i)
            std::printf("%s[%d,%d]", i ? "," : "", borders[i][0], borders[i][1]);
        std::printf("]}");
    }
    std::printf("]}\n");
    return 0;
}

int ReachPassCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const auto start = std::chrono::steady_clock::now();
    const bot::ReachFront front = bot::ComputeReachFront(s);
    if (front.stop == bot::ReachFrontStop::InvalidStart) {
        std::fprintf(stderr, "reach-pass: expected the start of a play turn\n");
        return 1;
    }
    const double mapMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("map_ms,%.6f\n", mapMs);
    std::printf("target,claim,own_first,opponent_first,path_claim,forecast,path_slot,path_cell,through,own_intercept,opponent_intercept,deflect,race,race_team,race_ms\n");
    for (const Action& action : LegalActions(s)) {
        if (action.type != ActionType::Pass) continue;
        const int index = bot::ReachFrontIndex(action.target);
        const unsigned mask = front.frames.back().players[index];
        const unsigned own = s.teamToMove == kWhite ? 15u : 240u;
        const bool ownClaim = (mask & own) != 0;
        int ownFirst = -1, opponentFirst = -1;
        for (std::size_t h = 0; h < front.frames.size(); ++h) {
            const unsigned at = front.frames[h].players[index];
            if (ownFirst < 0 && (at & own)) ownFirst = static_cast<int>(h);
            if (opponentFirst < 0 && (at & (255u ^ own))) opponentFirst = static_cast<int>(h);
        }
        double through = 0, ownIntercept = 0, opponentIntercept = 0, deflect = 0;
        for (const Outcome& outcome : OutcomesUnchecked(s, action)) {
            if (outcome.kind == OutcomeKind::BallThrough) through += outcome.probability;
            else if (outcome.kind == OutcomeKind::Intercepted) {
                if (TeamOf(outcome.player) == s.teamToMove) ownIntercept += outcome.probability;
                else opponentIntercept += outcome.probability;
            } else if (outcome.kind == OutcomeKind::Deflected) deflect += outcome.probability;
        }
        GameState after = s;
        ApplyUnchecked(after, action, Outcome{OutcomeKind::BallThrough, kNoPlayer, {}, through});
        const auto raceStart = std::chrono::steady_clock::now();
        const bot::BallRaceResult race = bot::SolveBallRace(after);
        const double raceMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - raceStart).count();
        const int forecastTeam = bot::ReachPassFirstTeam(s, front, race.windows);
        const char* forecast = forecastTeam < 0 ? "none" :
            forecastTeam == s.teamToMove ? "own" : "opponent";
        int pathSlot = -1;
        Cell pathCell{};
        const char* pathClaim = "none";
        for (const bot::BallWindow& window : race.windows) {
            const int firstSlot = window.arrival < 0 ? 0 : (window.arrival + 1) / 2;
            if (window.arrival >= 0) {
                const int standingFrame = (window.arrival + 1) / 2;
                if (standingFrame < static_cast<int>(front.frames.size())) {
                    const unsigned at = front.frames[standingFrame].players[bot::ReachFrontIndex(window.cell)];
                    if (at) {
                        pathSlot = standingFrame - 1;
                        pathCell = window.cell;
                        pathClaim = at & own ? "own" : "opponent";
                    }
                }
            }
            if (pathSlot >= 0) break;
            const int lastSlot = window.openEnd ? static_cast<int>(front.frames.size()) - 2 : window.lastMoveSlot;
            for (int slot = firstSlot; slot <= lastSlot &&
                                      slot + 1 < static_cast<int>(front.frames.size()); ++slot) {
                const unsigned at = front.frames[slot + 1].players[bot::ReachFrontIndex(window.cell)];
                if (!at) continue;
                pathSlot = slot;
                pathCell = window.cell;
                pathClaim = at & own ? "own" : "opponent";
                break;
            }
            if (pathSlot >= 0) break;
        }
        const char* raceKind = race.kind == bot::BallRaceKind::Taken ? "taken" :
            race.kind == bot::BallRaceKind::Restart ? "restart" :
            race.kind == bot::BallRaceKind::Contested ? "contested" :
            after.phase == Phase::Lineup ? "restart" : "held";
        int raceTeam = race.team;
        if (race.kind == bot::BallRaceKind::NotFree && after.ball.holder != kNoPlayer)
            raceTeam = TeamOf(after.ball.holder);
        else if (race.kind == bot::BallRaceKind::NotFree && after.phase == Phase::Lineup)
            raceTeam = after.teamToMove;
        std::printf("%s,%s,%d,%d,%s,%s,%d,%s,%.9f,%.9f,%.9f,%.9f,%s,%d,%.6f\n",
                    CellName(action.target).c_str(),
                    ownClaim ? "own" : mask ? "opponent" : "none", ownFirst, opponentFirst,
                    pathClaim, forecast, pathSlot,
                    pathSlot >= 0 ? CellName(pathCell).c_str() : "-",
                    through, ownIntercept, opponentIntercept, deflect, raceKind, raceTeam, raceMs);
    }
    return 0;
}

std::string Mask(unsigned bits) {
    std::string t;
    for (int k = 0; k < kNumPlayers; ++k)
        if (bits >> k & 1u) t += (t.empty() ? "" : ",") + PlayerName(k);
    return t.empty() ? "-" : t;
}

int Popcount(unsigned bits) { int n = 0; for (; bits; bits &= bits - 1) ++n; return n; }

int BallPlansCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<bot::PlanParticipants> plans = bot::AnalyzeBallPlans(s, !o.noReplies);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (plans.empty()) {
        std::printf("no plans: the team to move does not hold the ball\n");
        return 0;
    }
    const int holder = s.ball.holder;
    const char* kinds[] = {"hold", "shot", "pass feet", "pass space"};
    struct Sum {
        long plans = 0, ownFull = 0, ownPlan = 0;
        double replyFull = 0, replyPlan = 0, roundFull = 0, roundPlan = 0;
        long replies = 0;
        long ownHist[4] = {0, 0, 0, 0}, oppHist[5] = {0, 0, 0, 0, 0};
        double closure = 0;
    } sums[5];
    for (const bot::PlanParticipants& p : plans) {
        for (Sum* sum : {&sums[static_cast<int>(p.plan.kind)], &sums[4]}) {
            ++sum->plans;
            sum->ownFull += p.ownFull;
            sum->ownPlan += p.ownPlan;
            ++sum->ownHist[std::min(3, Popcount(p.own & ~(1u << holder)))];
            ++sum->oppHist[std::min(4, Popcount(p.opponent))];
            sum->closure += Popcount(p.closure);
            if (p.replyFull > 0) {
                ++sum->replies;
                sum->replyFull += p.replyFull;
                sum->replyPlan += p.replyPlan;
                sum->roundFull += double(p.ownFull) * p.replyFull;
                sum->roundPlan += double(p.ownPlan) * p.replyPlan;
            }
        }
        if (o.verbose)
            std::printf("%-22s own %-14s opp %-14s closure %-10s states %ld/%ld reply %ld/%ld\n",
                        bot::PlanName(s, p.plan).c_str(), Mask(p.own).c_str(), Mask(p.opponent).c_str(),
                        Mask(p.closure).c_str(), p.ownPlan, p.ownFull, p.replyPlan, p.replyFull);
    }
    std::printf("%zu plans in %.1f s (holder %s)\n", plans.size(), seconds, PlayerName(holder).c_str());
    std::printf("%-10s %6s  partners 0/1/2/3      opponents 0/1/2/3/4     closure  own states plan/full        reply states plan/full     round\n", "kind", "plans");
    for (int i = 0; i < 5; ++i) {
        const Sum& m = sums[i];
        if (!m.plans) continue;
        std::printf("%-10s %6ld  %4ld %4ld %4ld %4ld   %4ld %4ld %4ld %4ld %4ld   %5.2f  %9ld/%-10ld %6.4f",
                    i < 4 ? kinds[i] : "all", m.plans, m.ownHist[0], m.ownHist[1], m.ownHist[2], m.ownHist[3],
                    m.oppHist[0], m.oppHist[1], m.oppHist[2], m.oppHist[3], m.oppHist[4], m.closure / m.plans,
                    m.ownPlan, m.ownFull, m.ownFull ? double(m.ownPlan) / m.ownFull : 0.0);
        if (m.replies)
            std::printf("  %9.0f/%-9.0f %6.4f  %8.2e", m.replyPlan, m.replyFull, m.replyPlan / m.replyFull,
                        m.roundPlan / m.roundFull);
        std::printf("\n");
    }
    if (!o.planMoves.empty()) {
        std::vector<Action> actions;
        GameState replay = s;
        std::size_t at = 0;
        while (at < o.planMoves.size()) {
            const std::size_t space = o.planMoves.find(' ', at);
            const std::string text = o.planMoves.substr(at, space == std::string::npos ? std::string::npos : space - at);
            at = space == std::string::npos ? o.planMoves.size() : space + 1;
            if (text.empty()) continue;
            Action a;
            if (text == "end") a = Action::EndTurn();
            else if (!ParseAction(replay, text, &a)) {
                std::fprintf(stderr, "--moves: cannot play %s\n", text.c_str());
                return 1;
            }
            actions.push_back(a);
            if (a.type != ActionType::EndTurn) {
                const std::vector<Outcome> outcomes = OutcomesUnchecked(replay, a);
                Outcome chosen = outcomes.front();
                for (const Outcome& x : outcomes)
                    if (x.kind == OutcomeKind::BallThrough) chosen = x;
                ApplyUnchecked(replay, a, chosen);
            }
        }
        bot::BallPlan plan;
        if (!bot::PlanOfActions(s, actions, &plan)) return 1;
        const bot::PlanParticipants* found = nullptr;
        for (const bot::PlanParticipants& p : plans)
            if (p.plan.kind == plan.kind && p.plan.from == plan.from && p.plan.target == plan.target &&
                p.plan.receiver == plan.receiver)
                found = &p;
        std::printf("moves \"%s\": plan %s\n", o.planMoves.c_str(), bot::PlanName(s, plan).c_str());
        if (!found) {
            std::printf("  the plan is not among the analysed plans\n");
            return 0;
        }
        std::printf("  own %s, opponents %s, closure %s; states %ld/%ld, reply %ld/%ld\n", Mask(found->own).c_str(),
                    Mask(found->opponent).c_str(), Mask(found->closure).c_str(), found->ownPlan, found->ownFull,
                    found->replyPlan, found->replyFull);
        for (int k = 0; k < kNumPlayers; ++k)
            if (!found->why[k].empty()) std::printf("  %s: %s\n", PlayerName(k).c_str(), found->why[k].c_str());
        // Which players the moves move, before and after the ball action.
        bool ballPlayed = false;
        for (const Action& a : actions) {
            if (a.type == ActionType::Pass || a.type == ActionType::Shot) ballPlayed = true;
            if (a.type != ActionType::Move) continue;
            const unsigned bit = 1u << a.player;
            const char* role = (found->own & bit) ? "participant" : (found->closure & bit) ? "closure" : "outside";
            std::printf("  move %s%s: %s\n", PlayerName(a.player).c_str(), ballPlayed ? " (after the ball)" : "", role);
        }
    }
    return 0;
}

// ssbot plan-residual "<position>" --moves "a b ... end" [--bot NAME]
// The residual principle on a half-turn (bot/ball_plan.h): keep the moves of
// the plan's participants and the ball action of the given half-turn; every
// outside player of the team to move then chooses his own best move alone
// (staying included; not onto a free ball) by the bot's leaf value after End
// Turn, the other outside players standing. Passes are taken untouched.
// Prints the new half-turn and the leaf values of both.
int PlanResidualCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    Options withBot = o;
    if (withBot.botName.empty()) withBot.botName = bot::DefaultBotName();
    const bot::BotPreset* preset = SelectiveBot(withBot, "plan-residual");
    if (!preset) return 2;
    bot::GoalDfsSearch search(preset->options.search);
    const int us = s.teamToMove;
    const int episodeTeam = s.ball.attackTeam;
    const bool maximize = episodeTeam == us;
    auto leaf = [&](const GameState& t) { return search.SelectiveLeafEstimate(t, episodeTeam, true); };
    auto play = [](GameState& t, const Action& a) {
        const std::vector<Outcome> outcomes = OutcomesUnchecked(t, a);
        Outcome chosen = outcomes.front();
        for (const Outcome& x : outcomes)
            if (x.kind == OutcomeKind::BallThrough) chosen = x;
        ApplyUnchecked(t, a, chosen);
    };

    // The given half-turn.
    std::vector<Action> actions;
    GameState old = s;
    {
        std::size_t at = 0;
        while (at < o.planMoves.size()) {
            const std::size_t space = o.planMoves.find(' ', at);
            const std::string text = o.planMoves.substr(at, space == std::string::npos ? std::string::npos : space - at);
            at = space == std::string::npos ? o.planMoves.size() : space + 1;
            if (text.empty() || text == "end") continue;
            Action a;
            if (!ParseAction(old, text, &a)) {
                std::fprintf(stderr, "--moves: cannot play %s\n", text.c_str());
                return 1;
            }
            actions.push_back(a);
            play(old, a);
        }
    }
    bot::BallPlan plan;
    if (!bot::PlanOfActions(s, actions, &plan)) {
        std::printf("no plan: the team to move does not hold the ball\n");
        return 0;
    }
    unsigned participants = 0;
    for (const bot::PlanParticipants& p : bot::AnalyzeBallPlans(s, false))
        if (p.plan.kind == plan.kind && p.plan.from == plan.from && p.plan.target == plan.target &&
            p.plan.receiver == plan.receiver)
            participants = p.own;
    if (!participants) participants = 1u << s.ball.holder;

    // Participants' moves and the ball action, in the given order.
    GameState t = s;
    std::vector<std::string> text;
    bool shot = false;
    for (const Action& a : actions) {
        if (a.type == ActionType::Move && !(participants >> a.player & 1u)) continue;
        if (a.type == ActionType::Shot) { shot = true; break; }   // last: outside players move before it
        if (Validate(t, a) != ActionError::None) {
            std::printf("participant action %s is not legal without the outside moves\n", ActionText(t, a).c_str());
            return 1;
        }
        text.push_back(ActionText(t, a));
        play(t, a);
    }
    // Outside players: each alone, best by the leaf value; then played in order of gain.
    struct Choice { int player; Cell cell; double gain; };
    std::vector<Choice> choices;
    const double base = leaf(t);
    for (int k = FirstPlayerOf(us); k < FirstPlayerOf(us) + kPlayersPerTeam; ++k) {
        if ((participants >> k & 1u) || t.players[k].moved) continue;
        Choice best{k, t.players[k].pos, 0};
        for (Cell c : MoveDestinations(t, k)) {
            if (c == t.players[k].pos || (t.ball.IsFree() && c == t.ball.pos)) continue;
            GameState u = t;
            ApplyUnchecked(u, Action::Move(k, c), Outcome::Certain());
            const double v = leaf(u) - base;
            const double gain = maximize ? v : -v;
            if (gain > best.gain + 1e-12) best = {k, c, gain};
        }
        std::printf("  %s outside: best %s, gain %+.4f\n", PlayerName(k).c_str(),
                    best.cell == t.players[k].pos ? "stay" : CellName(best.cell).c_str(), best.gain);
        if (best.cell != t.players[k].pos) choices.push_back(best);
    }
    std::sort(choices.begin(), choices.end(), [](const Choice& a, const Choice& b) { return a.gain > b.gain; });
    for (const Choice& c : choices) {
        const Action a = Action::Move(c.player, c.cell);
        if (Validate(t, a) != ActionError::None) {
            std::printf("  %s: %s blocked after the other choices, stays\n", PlayerName(c.player).c_str(), CellName(c.cell).c_str());
            continue;
        }
        text.push_back(ActionText(t, a));
        play(t, a);
    }
    if (shot) { text.push_back("#"); }
    std::string line;
    for (const std::string& x : text) line += x + " ";
    line += "end";
    const double oldLeaf = leaf(old), newLeaf = leaf(t);
    std::printf("plan %s, participants %s\n", bot::PlanName(s, plan).c_str(), Mask(participants).c_str());
    std::printf("old: %s  leaf %.4f\nnew: %s  leaf %.4f%s\n", o.planMoves.c_str(), oldLeaf, line.c_str(), newLeaf,
                shot ? "  (shot plans: leaf of the position before the shot)" : "");
    return 0;
}

// One side of a self-play match: a bot of the collection by name
// (bot/bot_registry.h) - --time, --threads and the search limits, when given,
// override its settings - or random.
bot::BotOptions PresetOptions(const std::string& kind, const Options& o) {
    const bot::BotPreset* p = bot::FindBotPreset(kind);
    bot::BotOptions r = p ? p->options : bot::BotOptions();
    if (o.time >= 0) r.timeLimit = o.time;
    if (o.threads >= 0) r.threads = o.threads;
    ApplyLimits(o, &r.search);
    return r;
}

class Side {
public:
    Side(const std::string& kind, const bot::BotOptions& options, uint64_t seed) : kind_(kind), random_(seed) {
        if (kind_ != "random") bot_.reset(new bot::Bot(options));
    }

    Action Next(const GameState& s, double& seconds) {
        if (!bot_) {
            if (s.phase == Phase::Lineup) return bot::LineupAction(s);
            std::vector<Action> acts = LegalActions(s);
            Action a = acts.back();
            if (acts.size() > 1 && random_.Uniform() < 0.8) a = acts[random_.Index(static_cast<int>(acts.size()) - 1)];
            return a;
        }
        auto start = std::chrono::steady_clock::now();
        Action a = bot_->NextAction(s);
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return a;
    }

    bool IsBot() const { return bot_ != nullptr; }
    const bot::GoalSearchResult& LastSearch() const { return bot_ ? bot_->LastSearch() : empty_; }
    bot::GoalDfsStats LastSearchStats() const { return bot_ ? bot_->LastSearchStats() : bot::GoalDfsStats{}; }

private:
    std::string kind_;
    std::unique_ptr<bot::Bot> bot_;
    bot::GoalSearchResult empty_;
    Rng random_;
};

int SelfPlay(const Options& o) {
    if (o.episodeTurns < -1) {
        std::fprintf(stderr, "--episode-turns must be nonnegative\n");
        return 2;
    }
    for (const std::string& k : {o.white, o.black})
        if (k != "random" && !bot::FindBotPreset(k)) {
            std::string names;
            for (const bot::BotPreset& p : bot::BotPresets()) names += p.name + ", ";
            std::fprintf(stderr, "unknown player %s (%srandom; ssbot bots lists the bots)\n", k.c_str(), names.c_str());
            return 2;
        }
    GameRecord record;
    char date[16];
    std::time_t now = std::time(nullptr);
    std::strftime(date, sizeof date, "%Y.%m.%d", std::localtime(&now));
    bot::BotOptions wp = PresetOptions(o.white, o), bp = PresetOptions(o.black, o);
    if (o.whiteTime >= 0) wp.timeLimit = o.whiteTime;
    if (o.blackTime >= 0) bp.timeLimit = o.blackTime;
    Side white(o.white, wp, o.seed * 7919 + 1), black(o.black, bp, o.seed * 7919 + 2);
    // The player's name; with the time per team turn when it was set (name@60s),
    // so that the statistics tell the variants apart.
    auto tag = [&](const std::string& kind, double sideTime, const bot::BotOptions& po) {
        std::string name = kind;
        if (bot::FindBotPreset(kind) && (sideTime >= 0 || o.time >= 0)) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "@%gs", po.timeLimit);
            name += buf;
        }
        return name;
    };
    record.SetTag("Event", "selfplay");
    record.SetTag("Date", date);
    record.SetTag("White", tag(o.white, o.whiteTime, wp));
    record.SetTag("Black", tag(o.black, o.blackTime, bp));
    record.SetTag("Seed", std::to_string(o.seed));

    GameState s;
    if (!o.startPosition.empty()) {
        std::string error;
        if (!ParsePosition(o.startPosition, &s, &error)) {
            std::fprintf(stderr, "invalid start position: %s\n", error.c_str());
            return 2;
        }
        s.rng.Seed(o.seed);
        record.SetTag("Position", FormatPosition(s));
    } else {
        s = NewMatch(o.seed);
    }
    const int endHalfTurn = o.episodeTurns >= 0
        ? std::min(kHalfTurnsPerMatch, s.halfTurns + o.episodeTurns) : o.halfTurns;
    std::ofstream trace;
    if (!o.trace.empty()) {
        trace.open(o.trace);
        if (!trace) {
            std::fprintf(stderr, "cannot open trace %s\n", o.trace.c_str());
            return 2;
        }
        trace << "action_index,half_turn,turn_id,team,action,root_legal,root_drops,root_drop_fraction,"
                 "seconds,completed_turn_depth,"
                 "max_depth,positions,expansions,move_expansions,pass_expansions,"
                 "shot_expansions,attack_expansions,end_turn_expansions,outcomes,"
                 "movement_drops,action_drops,cache_hits,cache_entries\n";
    }
    auto start = std::chrono::steady_clock::now();
    double thinking[2] = {0, 0};
    // (A guard on the number of actions as well, against a bot that never ends its turn.)
    for (int actions = 0; s.phase != Phase::Finished && s.halfTurns < endHalfTurn && actions < o.maxActions; ++actions) {
        const int team = s.teamToMove;
        Side& side = team == kWhite ? white : black;
        const double previousThinking = thinking[team];
        Action a = side.Next(s, thinking[team]);
        const int turnId = s.turnId;
        const std::string actionText = ActionText(s, a);
        const bot::GoalSearchResult& result = side.LastSearch();
        const bot::GoalDfsStats stats = side.LastSearchStats();
        const int width = (team == kWhite ? wp : bp).search.selectiveWidth;
        const long rootLegal = trace ? static_cast<long>(LegalActions(s).size()) : 0;
        const long rootDrops = side.IsBot() && width > 0
            ? std::min(stats.selectiveActionDrops, std::max(0L, rootLegal - width)) : 0;
        RecordedAction r;
        r.action = a;
        int before = s.halfTurns;
        if (Apply(s, a, nullptr, &r.outcome) != ActionError::None) {
            std::fprintf(stderr, "illegal action from the bot: %s (type %d) in %s\n", ActionText(s, a).c_str(),
                         static_cast<int>(a.type), s.phase == Phase::Play ? FormatPosition(s).c_str() : "line-up");
            return 1;
        }
        record.actions.push_back(r);
        if (trace) {
            // A terminal action may complete many cached iterative depths without
            // exploring a deeper half-turn. Record only the depth actually reached.
            const int completedDepth = side.IsBot()
                ? std::min(stats.completedTurnDepth, result.maxDepth) : 0;
            trace << actions << ',' << before << ',' << turnId << ',' << (team == kWhite ? 'W' : 'B')
                  << ',' << actionText << ',' << rootLegal << ',' << rootDrops << ','
                  << (rootLegal ? static_cast<double>(rootDrops) / rootLegal : 0) << ','
                  << thinking[team] - previousThinking << ','
                  << completedDepth << ','
                  << (side.IsBot() ? result.maxDepth : 0) << ','
                  << (side.IsBot() ? result.positions : 0) << ','
                  << (side.IsBot() ? result.expansions : 0) << ','
                  << stats.moveExpansions << ',' << stats.passExpansions << ','
                  << stats.shotExpansions << ',' << stats.attackExpansions << ','
                  << stats.endTurnExpansions << ',' << stats.outcomesProcessed << ','
                  << stats.selectiveMovementDrops << ',' << stats.selectiveActionDrops << ','
                  << stats.cacheHits << ',' << stats.cacheEntries << '\n';
            trace.flush();
        }
        if (s.halfTurns != before && !o.quiet)
            std::printf("\rhalf-turn %d, score %d-%d, %.0f s", s.halfTurns, s.score[0], s.score[1],
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()),
                std::fflush(stdout);
    }
    record.SetTag("Result", s.phase == Phase::Finished ? std::to_string(s.score[0]) + "-" + std::to_string(s.score[1]) : "*");
    if (o.episodeTurns >= 0)
        record.SetTag("EpisodeScore", std::to_string(s.score[0]) + "-" + std::to_string(s.score[1]));
    std::string text = WriteRecord(record);
    std::printf("%sresult %s %s - %s, half-turns %d, thinking W %.1f s, B %.1f s, %.0f min\n", o.quiet ? "" : "\n",
                record.Tag("Result").c_str(), record.Tag("White").c_str(), record.Tag("Black").c_str(), s.halfTurns,
                thinking[0], thinking[1],
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() / 60);
    if (!o.out.empty()) {
        std::ofstream f(o.out);
        f << text;
        std::printf("written %s\n", o.out.c_str());
    } else {
        std::printf("%s", text.c_str());
    }
    return 0;
}

// Per-team statistics of recorded games.
struct TeamStats {
    int goals = 0, shots = 0, passes = 0, backPasses = 0, tacklesSuffered = 0, ballsLost = 0;
    int keeperPenalties = 0, turnsWithBall = 0, turnsOnEdge = 0;
    double progress = 0;   // sum over turns with the ball: 0 at the own goal line .. 1 at the opponent's
    double xg = 0;         // expected goals: the sum of the goal probabilities of the shots
};

int Stats(int argc, char** argv) {
    TeamStats st[2];
    int games = 0;
    // Per player (the tags White / Black: a bot of the collection or "human").
    struct PlayerStats {
        TeamStats t;
        int games = 0, wins = 0, draws = 0, losses = 0, goalsAgainst = 0;
        double xgAgainst = 0;
    };
    std::map<std::string, PlayerStats> players;
    for (int i = 2; i < argc; ++i) {
        std::ifstream f(argv[i]);
        std::stringstream buffer;
        buffer << f.rdbuf();
        GameRecord record;
        std::string error;
        if (!ReadRecord(buffer.str(), &record, &error)) {
            std::fprintf(stderr, "%s: %s\n", argv[i], error.c_str());
            continue;
        }
        ++games;
        const TeamStats before[2] = {st[0], st[1]};
        GameState s = RecordStart(record);
        const int startScore[2] = {s.score[0], s.score[1]};
        for (const RecordedAction& r : record.actions) {
            const int team = s.teamToMove;
            const bool play = s.phase == Phase::Play;
            if (play && r.action.type == ActionType::EndTurn && !s.ball.IsFree() && TeamOf(s.ball.holder) == team) {
                TeamStats& t = st[team];
                ++t.turnsWithBall;
                Cell b = s.ball.pos;
                if (b.x == 0 || b.x == kFieldWidth - 1) ++t.turnsOnEdge;
                double toGoal = team == kWhite ? b.y - kFirstRow : kLastRow - b.y;
                t.progress += 1 - toGoal / double(kLastRow - kFirstRow);
            }
            if (play && r.action.type == ActionType::Pass) {
                ++st[team].passes;
                Cell from = s.ball.pos, to = r.action.target;
                bool back = team == kWhite ? to.y > from.y : to.y < from.y;
                if (back) ++st[team].backPasses;
            }
            if (play && r.action.type == ActionType::Shot) {
                ++st[team].shots;
                for (const Outcome& out : OutcomesUnchecked(s, r.action))
                    if (out.kind == OutcomeKind::Goal) st[team].xg += out.probability;
            }
            if (play && r.action.type == ActionType::Attack && !s.ball.IsFree()) ++st[Opponent(team)].tacklesSuffered;
            const int holderTeam = s.ball.IsFree() ? -1 : TeamOf(s.ball.holder);
            EventLog events;
            ApplyOutcome(s, r.action, r.outcome, &events);
            for (const Event& e : events) {
                if (e.type == EventType::Goal) ++st[e.team].goals;
                if (e.type == EventType::BackPassPenalty) ++st[Opponent(e.team)].keeperPenalties;
            }
            if (holderTeam >= 0 && !s.ball.IsFree() && TeamOf(s.ball.holder) != holderTeam && s.phase == Phase::Play)
                ++st[holderTeam].ballsLost;
        }
        for (int team = 0; team < 2; ++team) {
            PlayerStats& p = players[record.Tag(team == kWhite ? "White" : "Black")];
            TeamStats d = st[team];
            const TeamStats& b0 = before[team];
            d.goals -= b0.goals, d.shots -= b0.shots, d.passes -= b0.passes, d.backPasses -= b0.backPasses;
            d.tacklesSuffered -= b0.tacklesSuffered, d.ballsLost -= b0.ballsLost, d.keeperPenalties -= b0.keeperPenalties;
            d.turnsWithBall -= b0.turnsWithBall, d.turnsOnEdge -= b0.turnsOnEdge, d.progress -= b0.progress;
            d.xg -= b0.xg;
            p.t.xg += d.xg;
            p.xgAgainst += st[Opponent(team)].xg - before[Opponent(team)].xg;
            p.t.goals += d.goals, p.t.shots += d.shots, p.t.passes += d.passes, p.t.backPasses += d.backPasses;
            p.t.tacklesSuffered += d.tacklesSuffered, p.t.ballsLost += d.ballsLost, p.t.keeperPenalties += d.keeperPenalties;
            p.t.turnsWithBall += d.turnsWithBall, p.t.turnsOnEdge += d.turnsOnEdge, p.t.progress += d.progress;
            ++p.games;
            const int us = s.score[team] - startScore[team];
            const int them = s.score[Opponent(team)] - startScore[Opponent(team)];
            p.goalsAgainst += them;
            if (us > them) ++p.wins;
            else if (us == them) ++p.draws;
            else ++p.losses;
        }
    }
    std::printf("%d games\n%-26s %10s %10s\n", games, "per game", "White", "Black");
    auto row = [&](const char* name, double w, double b) { std::printf("%-26s %10.2f %10.2f\n", name, w, b); };
    double g = games ? games : 1;
    row("goals", st[0].goals / g, st[1].goals / g);
    row("shots", st[0].shots / g, st[1].shots / g);
    row("expected goals (xG)", st[0].xg / g, st[1].xg / g);
    row("passes", st[0].passes / g, st[1].passes / g);
    row("backward passes, %", 100.0 * st[0].backPasses / std::max(1, st[0].passes), 100.0 * st[1].backPasses / std::max(1, st[1].passes));
    row("turns with the ball", st[0].turnsWithBall / g, st[1].turnsWithBall / g);
    row("ball progress, %", 100 * st[0].progress / std::max(1, st[0].turnsWithBall), 100 * st[1].progress / std::max(1, st[1].turnsWithBall));
    row("on the side line, %", 100.0 * st[0].turnsOnEdge / std::max(1, st[0].turnsWithBall), 100.0 * st[1].turnsOnEdge / std::max(1, st[1].turnsWithBall));
    row("tackles suffered", st[0].tacklesSuffered / g, st[1].tacklesSuffered / g);
    row("balls lost in play", st[0].ballsLost / g, st[1].ballsLost / g);
    row("back passes to keeper", st[0].keeperPenalties / g, st[1].keeperPenalties / g);
    if (players.size() > 1) {
        std::printf("\nper player (the tags White / Black; score gained during the record)\n");
        std::printf("%-16s %5s %9s %7s %7s %7s %7s %7s %7s %7s\n", "player", "games", "W-D-L", "goals", "against", "xG",
                    "xG agst", "shots", "passes", "lost");
        for (const auto& e : players) {
            const PlayerStats& p = e.second;
            const double n = p.games ? p.games : 1;
            char wdl[32];
            std::snprintf(wdl, sizeof wdl, "%d-%d-%d", p.wins, p.draws, p.losses);
            std::printf("%-16s %5d %9s %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f %7.2f\n", e.first.c_str(), p.games, wdl,
                        p.t.goals / n, p.goalsAgainst / n, p.t.xg / n, p.xgAgainst / n, p.t.shots / n, p.t.passes / n,
                        p.t.ballsLost / n);
        }
    }
    return 0;
}

int Replay(const Options& o) {
    std::ifstream f(o.arg);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", o.arg.c_str());
        return 1;
    }
    std::stringstream buffer;
    buffer << f.rdbuf();
    GameRecord record;
    std::string error;
    if (!ReadRecord(buffer.str(), &record, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    GameState end;
    ReplayRecord(record, &end);
    std::printf("%zu actions, score %d-%d, half-turn %d\n", record.actions.size(), end.score[0], end.score[1], end.halfTurns);
    if (end.phase == Phase::Play) std::printf("position: %s\n", FormatPosition(end).c_str());
    return 0;
}

int Positions(const Options& o) {
    std::ifstream f(o.arg);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", o.arg.c_str());
        return 1;
    }
    std::stringstream buffer;
    buffer << f.rdbuf();
    GameRecord record;
    std::string error;
    if (!ReadRecord(buffer.str(), &record, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    GameState s = RecordStart(record);
    int printedTurnId = -1;
    auto printTurn = [&](std::size_t actionIndex) {
        if (s.phase != Phase::Play || s.turnId == printedTurnId) return;
        printedTurnId = s.turnId;
        std::printf("%d.%c action=%zu position=%s\n", s.halfTurns + 1,
                    s.teamToMove == kWhite ? 'W' : 'B', actionIndex, FormatPosition(s).c_str());
    };
    for (std::size_t i = 0; i < record.actions.size(); ++i) {
        printTurn(i);
        const RecordedAction& r = record.actions[i];
        if (ApplyOutcome(s, r.action, r.outcome) != ActionError::None) {
            std::fprintf(stderr, "action %zu is not possible\n", i + 1);
            return 1;
        }
    }
    printTurn(record.actions.size());
    return 0;
}

}  // namespace

// Who reaches a free ball first (bot/ball_race.h). A position: the solver's
// bounds for every player. Games: every state with a free ball in play, at the
// start of a team turn and after each action, with a summary.
int BallRaceCommand(int argc, char** argv) {
    std::vector<std::string> args;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
        else args.push_back(argv[i]);
    }
    if (args.empty()) {
        std::fprintf(stderr, "ball-race: a position or game files\n");
        return 2;
    }
    using Clock = std::chrono::steady_clock;
    GameState one;
    if (args.size() == 1 && ParsePosition(args[0], &one)) {
        bot::BallRaceOptions options;
        options.allUppers = true;
        const auto t0 = Clock::now();
        const bot::BallRaceResult r = bot::SolveBallRace(one, options);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::printf("%s%.3f ms\n", r.Describe(one).c_str(), ms);
        return 0;
    }
    int total = 0, taken = 0, restart = 0, contested = 0;
    int contestantHist[kNumPlayers + 1] = {};
    double totalMs = 0, maxMs = 0;
    for (const std::string& file : args) {
        std::ifstream f(file);
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", file.c_str());
            return 1;
        }
        std::stringstream buffer;
        buffer << f.rdbuf();
        GameRecord record;
        std::string error;
        if (!ReadRecord(buffer.str(), &record, &error)) {
            std::fprintf(stderr, "%s: %s\n", file.c_str(), error.c_str());
            return 1;
        }
        GameState s = RecordStart(record);
        std::string previous;
        auto look = [&](std::size_t index, const char* when) {
            if (s.phase != Phase::Play || !s.ball.IsFree() || s.keeperRestartTeam >= 0) return;
            const std::string key = FormatPosition(s) + (s.ballPlayed ? "p" : "") + std::to_string(s.turnId);
            std::string moved;
            for (int k = 0; k < kNumPlayers; ++k) moved += s.players[k].moved ? '1' : '0';
            if (key + moved == previous) return;
            previous = key + moved;
            const auto t0 = Clock::now();
            const bot::BallRaceResult r = bot::SolveBallRace(s);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            totalMs += ms;
            maxMs = std::max(maxMs, ms);
            ++total;
            std::string verdict;
            if (r.kind == bot::BallRaceKind::Taken) {
                ++taken;
                verdict = std::string("taken ") + PlayerName(r.player) + " " + CellName(r.cell) + " " +
                          bot::RaceMomentName(r.moment);
            } else if (r.kind == bot::BallRaceKind::Restart) {
                ++restart;
                verdict = std::string("restart ") + (r.team == kWhite ? "W " : "B ") + (r.keeper ? "keeper " : "out ") +
                          bot::RaceMomentName(r.moment);
            } else {
                ++contested;
                int n = 0;
                verdict = "contested";
                for (int k = 0; k < kNumPlayers; ++k)
                    if (r.contestants >> k & 1) { verdict += " " + PlayerName(k); ++n; }
                ++contestantHist[n];
            }
            std::printf("%s %d.%c %s action=%zu %s | %s\n", file.c_str(), s.halfTurns + 1,
                        s.teamToMove == kWhite ? 'W' : 'B', when, index, verdict.c_str(), FormatPosition(s).c_str());
            if (verbose) std::printf("%s", r.Describe(s).c_str());
        };
        for (std::size_t i = 0; i < record.actions.size(); ++i) {
            look(i, "before");
            const RecordedAction& a = record.actions[i];
            if (ApplyOutcome(s, a.action, a.outcome) != ActionError::None) {
                std::fprintf(stderr, "%s: action %zu is not possible\n", file.c_str(), i + 1);
                return 1;
            }
        }
        look(record.actions.size(), "end");
    }
    std::printf("free-ball states %d: taken %d, restart %d, contested %d; solve %.3f ms mean, %.3f ms max\n",
                total, taken, restart, contested, total ? totalMs / total : 0.0, maxMs);
    std::printf("contestants in contested races:");
    for (int n = 1; n <= kNumPlayers; ++n)
        if (contestantHist[n]) std::printf(" %d players: %d;", n, contestantHist[n]);
    std::printf("\n");
    return 0;
}

// Pass pruning by the reach map (07.11.2026): in every state of the games
// where the team to move holds the ball (start of a turn and inside it), each
// legal pass to an empty cell - the map's first taker against the ball race
// on the untouched branch. Also the map's cost against the race's.
int ReachPruneCheck(int argc, char** argv) {
    std::vector<std::string> files;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
        else files.push_back(argv[i]);
    }
    using Clock = std::chrono::steady_clock;
    // rows: map own / opponent / none; columns: race own taken, opponent taken, restart, contested
    long table[3][4] = {};
    long states = 0, midTurn = 0, passes = 0;
    double mapMs = 0, raceMs = 0, windowMs = 0;
    std::set<std::string> seen;
    for (const std::string& file : files) {
        std::ifstream f(file);
        if (!f) { std::fprintf(stderr, "cannot open %s\n", file.c_str()); return 1; }
        std::stringstream buffer;
        buffer << f.rdbuf();
        GameRecord record;
        std::string error;
        if (!ReadRecord(buffer.str(), &record, &error)) {
            std::fprintf(stderr, "%s: %s\n", file.c_str(), error.c_str());
            continue;
        }
        GameState s = RecordStart(record);
        auto look = [&]() {
            if (s.phase != Phase::Play || s.ball.IsFree() || TeamOf(s.ball.holder) != s.teamToMove ||
                s.ballPlayed || s.keeperRestartTeam >= 0) return;
            std::string key = FormatPosition(s);
            if (!seen.insert(key).second) return;
            bool inside = false;
            for (int k = 0; k < kNumPlayers; ++k) inside = inside || s.players[k].moved;
            ++states;
            if (inside) ++midTurn;
            const auto t0 = Clock::now();
            const bot::ReachFront front = bot::ComputeReachFront(s);
            mapMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            if (front.stop == bot::ReachFrontStop::InvalidStart) return;
            for (const Action& a : LegalActions(s)) {
                if (a.type != ActionType::Pass || PlayerAt(s, a.target) != kNoPlayer) continue;
                ++passes;
                const auto t1 = Clock::now();
                const int taker = bot::ReachPassTaker(s, front, a);
                windowMs += std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
                GameState after = s;
                ApplyUnchecked(after, a, Outcome{OutcomeKind::BallThrough});
                const auto t2 = Clock::now();
                const bot::BallRaceResult r = bot::SolveBallRace(after);
                raceMs += std::chrono::duration<double, std::milli>(Clock::now() - t2).count();
                const int row = taker < 0 ? 2 : taker == s.teamToMove ? 0 : 1;
                int col = 3;
                if (r.kind == bot::BallRaceKind::Taken) col = r.team == s.teamToMove ? 0 : 1;
                else if (r.kind == bot::BallRaceKind::Restart) col = 2;
                ++table[row][col];
                if (verbose && row == 1 && col != 1)
                    std::printf("prune-mismatch %s %d.%c >%s race=%d | %s\n", file.c_str(), s.halfTurns + 1,
                                s.teamToMove == kWhite ? 'W' : 'B', CellName(a.target).c_str(), col,
                                FormatPosition(s).c_str());
            }
        };
        for (std::size_t i = 0; i < record.actions.size(); ++i) {
            look();
            const RecordedAction& a = record.actions[i];
            if (ApplyOutcome(s, a.action, a.outcome) != ActionError::None) {
                std::fprintf(stderr, "%s: action %zu is not possible\n", file.c_str(), i + 1);
                return 1;
            }
        }
    }
    std::printf("states %ld (inside a turn %ld), passes to empty cells %ld\n", states, midTurn, passes);
    const char* rows[] = {"map own", "map opponent", "map none"};
    std::printf("%-14s %10s %10s %10s %10s\n", "", "race own", "race opp", "restart", "contested");
    for (int r = 0; r < 3; ++r)
        std::printf("%-14s %10ld %10ld %10ld %10ld\n", rows[r], table[r][0], table[r][1], table[r][2], table[r][3]);
    std::printf("map %.4f ms per state, pass windows %.4f ms per pass, race %.4f ms per pass\n",
                states ? mapMs / states : 0.0, passes ? windowMs / passes : 0.0, passes ? raceMs / passes : 0.0);
    return 0;
}

// Potential 1 v 0 by the reach map (07.11.2026): in every state of the games
// where a team holds the ball, the map's detector against the proven bound
// (BreakawayShotLowerBound) and the attack episode's 1 v 0; their costs.
int ReachBreakawayCheck(int argc, char** argv) {
    std::vector<std::string> files;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
        else files.push_back(argv[i]);
    }
    using Clock = std::chrono::steady_clock;
    long states = 0, detected = 0, proven = 0, provenDetected = 0, oneVsZero = 0, oneVsZeroProven = 0,
         provenMissedByEpisode = 0, holderToMove = 0;
    double mapMs = 0, boundMs = 0, episodeMs = 0, sumProven = 0;
    long hist[5] = {};   // detected shot: 0, (0,.1], (.1,.3], (.3,.5], > .5
    long histProven[5] = {};
    std::set<std::string> seen;
    for (const std::string& file : files) {
        std::ifstream f(file);
        if (!f) continue;
        std::stringstream buffer;
        buffer << f.rdbuf();
        GameRecord record;
        std::string error;
        if (!ReadRecord(buffer.str(), &record, &error)) continue;
        GameState s = RecordStart(record);
        auto look = [&]() {
            if (s.phase != Phase::Play || s.ball.IsFree() || s.keeperRestartTeam >= 0) return;
            if (!seen.insert(FormatPosition(s)).second) return;
            ++states;
            if (TeamOf(s.ball.holder) == s.teamToMove) ++holderToMove;
            const auto t0 = Clock::now();
            const bot::ReachFront front = bot::ComputeReachFront(s);
            const bot::ReachBreakaway b = bot::ReachHolderBreakaway(s, front);
            const auto t1 = Clock::now();
            const double bound = bot::BreakawayShotLowerBound(s);
            const auto t2 = Clock::now();
            const bot::AttackEpisode e = bot::AnalyzeAttackEpisode(s, false, true);
            const auto t3 = Clock::now();
            mapMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
            boundMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
            episodeMs += std::chrono::duration<double, std::milli>(t3 - t2).count();
            const auto bin = [](double p) { return p <= 0 ? 0 : p <= 0.1 ? 1 : p <= 0.3 ? 2 : p <= 0.5 ? 3 : 4; };
            ++hist[bin(b.shot)];
            if (b.shot > 0) ++detected;
            const bool one = e.kind == bot::AttackEpisodeKind::OneVsZero;
            if (one) ++oneVsZero;
            if (bound > 0) {
                ++proven;
                sumProven += bound;
                ++histProven[bin(b.shot)];
                if (b.shot > 0) ++provenDetected;
                if (one) ++oneVsZeroProven; else ++provenMissedByEpisode;
            }
            if (verbose && (bound > 0 || b.shot > 0.3))
                std::printf("bw %s %d.%c holder=%s map %.3f @%s h%d bound %.3f episode %s | %s\n", file.c_str(),
                            s.halfTurns + 1, s.teamToMove == kWhite ? 'W' : 'B', PlayerName(s.ball.holder).c_str(),
                            b.shot, CellName(b.cell).c_str(), b.halfTurn, bound, one ? "1v0" : "-",
                            FormatPosition(s).c_str());
        };
        for (std::size_t i = 0; i < record.actions.size(); ++i) {
            look();
            const RecordedAction& a = record.actions[i];
            if (ApplyOutcome(s, a.action, a.outcome) != ActionError::None) break;
        }
    }
    std::printf("states with a holder %ld (holder's team to move %ld)\n", states, holderToMove);
    std::printf("map detects a shot cell %ld; proven bound > 0: %ld (mean %.3f), of them detected %ld\n",
                detected, proven, proven ? sumProven / proven : 0.0, provenDetected);
    std::printf("episode 1v0 %ld, of them proven %ld; proven but not 1v0 %ld\n", oneVsZero, oneVsZeroProven,
                provenMissedByEpisode);
    std::printf("map shot bins (0, <=.1, <=.3, <=.5, >.5): all %ld %ld %ld %ld %ld; proven %ld %ld %ld %ld %ld\n",
                hist[0], hist[1], hist[2], hist[3], hist[4], histProven[0], histProven[1], histProven[2],
                histProven[3], histProven[4]);
    std::printf("ms per state: map %.4f, proven bound %.4f, attack episode %.4f\n", states ? mapMs / states : 0.0,
                states ? boundMs / states : 0.0, states ? episodeMs / states : 0.0);
    return 0;
}

// The price of a lost pass (07.11.2026): for every pass in the games' states
// with the ball at the team to move, each outcome where an opponent catches
// it, after the end of the turn - the map's detector for the catcher and the
// proven breakaway bound.
int ReachCounterCheck(int argc, char** argv) {
    std::vector<std::string> files;
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
        else files.push_back(argv[i]);
    }
    long losses = 0;
    double weight = 0;
    long bins[5] = {}, provenBins[5] = {};
    double boundSum[5] = {};
    std::set<std::string> seen, seenLoss;
    for (const std::string& file : files) {
        std::ifstream f(file);
        if (!f) continue;
        std::stringstream buffer;
        buffer << f.rdbuf();
        GameRecord record;
        std::string error;
        if (!ReadRecord(buffer.str(), &record, &error)) continue;
        GameState s = RecordStart(record);
        auto look = [&]() {
            if (s.phase != Phase::Play || s.ball.IsFree() || TeamOf(s.ball.holder) != s.teamToMove ||
                s.ballPlayed || s.keeperRestartTeam >= 0) return;
            if (!seen.insert(FormatPosition(s)).second) return;
            for (const Action& a : LegalActions(s)) {
                if (a.type != ActionType::Pass) continue;
                for (const Outcome& o : OutcomesUnchecked(s, a)) {
                    if (o.kind != OutcomeKind::Intercepted || TeamOf(o.player) == s.teamToMove) continue;
                    GameState t = s;
                    ApplyUnchecked(t, a, o);
                    if (t.phase != Phase::Play || t.ball.IsFree()) continue;
                    const std::vector<Outcome> end = OutcomesUnchecked(t, Action::EndTurn());
                    if (end.size() != 1) continue;
                    ApplyUnchecked(t, Action::EndTurn(), end.front());
                    if (t.phase != Phase::Play || t.ball.IsFree()) continue;
                    if (!seenLoss.insert(FormatPosition(t)).second) continue;
                    ++losses;
                    weight += o.probability;
                    const bot::ReachFront front = bot::ComputeReachFront(t);
                    const bot::ReachBreakaway b = bot::ReachHolderBreakaway(t, front);
                    const double bound = bot::BreakawayShotLowerBound(t);
                    const int bin = b.shot <= 0.1 ? 0 : b.shot <= 0.3 ? 1 : b.shot <= 0.5 ? 2 : b.shot <= 0.9 ? 3 : 4;
                    ++bins[bin];
                    boundSum[bin] += bound;
                    if (bound > 0.1) ++provenBins[bin];
                    if (verbose)
                        std::printf("loss %s %d.%c %s p=%.3f map %.3f @%s h%d bound %.3f | %s\n", file.c_str(),
                                    s.halfTurns + 1, s.teamToMove == kWhite ? 'W' : 'B',
                                    FormatAction(s, a).c_str(), o.probability, b.shot, CellName(b.cell).c_str(),
                                    b.halfTurn, bound, FormatPosition(t).c_str());
                }
            }
        };
        for (std::size_t i = 0; i < record.actions.size(); ++i) {
            look();
            const RecordedAction& a = record.actions[i];
            if (ApplyOutcome(s, a.action, a.outcome) != ActionError::None) break;
        }
    }
    std::printf("loss states %ld (mean catch probability %.3f)\n", losses, losses ? weight / losses : 0.0);
    std::printf("map shot of the catcher: <=.1 / <=.3 / <=.5 / <=.9 / >.9: %ld %ld %ld %ld %ld\n",
                bins[0], bins[1], bins[2], bins[3], bins[4]);
    std::printf("proven bound > .1 in them: %ld %ld %ld %ld %ld; mean bound %.3f %.3f %.3f %.3f %.3f\n",
                provenBins[0], provenBins[1], provenBins[2], provenBins[3], provenBins[4],
                bins[0] ? boundSum[0] / bins[0] : 0.0, bins[1] ? boundSum[1] / bins[1] : 0.0,
                bins[2] ? boundSum[2] / bins[2] : 0.0, bins[3] ? boundSum[3] / bins[3] : 0.0,
                bins[4] ? boundSum[4] / bins[4] : 0.0);
    return 0;
}

// Route static on the reach map (07.11.2026): one line "id<TAB>position" per
// position from a file; a free ball is valued in its race's taken state.
int RouteStaticCommand(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "route-static: a file of id<TAB>position lines [--no-loss]\n"); return 2; }
    std::ifstream in(argv[2]);
    const bool withLoss = !(argc > 3 && std::strcmp(argv[3], "--no-loss") == 0);
    std::string line;
    std::printf("id,team,value,open,best,shooter,cell,frame,defenders,routes,ms,note\n");
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string id = line.substr(0, tab);
        GameState s;
        if (!ParsePosition(line.substr(tab + 1), &s)) { std::printf("%s,,,,,,,,,,,parse\n", id.c_str()); continue; }
        const auto t0 = std::chrono::steady_clock::now();
        const bot::RouteStatic r = bot::ComputeRouteStatic(s, withLoss);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!r.valid) { std::printf("%s,,,,,,,,,,,invalid\n", id.c_str()); continue; }
        std::string note = !r.freeBall ? "held" : r.restart ? std::string("free-restart-") + (r.team == kWhite ? "W" : r.team == kBlack ? "B" : "none")
                         : "free-" + PlayerName(r.taker) + "@" + CellName(r.takeCell) + "/f" + std::to_string(r.takeFrame);
        std::printf("%s,%c,%.4f,%.4f,%.4f,%s,%s,%d,%d,%ld,%.2f,%s\n", id.c_str(), r.team == kWhite ? 'W' : 'B',
                    r.value, r.open, r.best, r.shooter >= 0 ? PlayerName(r.shooter).c_str() : "-",
                    r.shooter >= 0 ? CellName(r.cell).c_str() : "-", r.frame, static_cast<int>(std::bitset<8>(r.defenders).count()),
                    r.routes, ms, note.c_str());
    }
    return 0;
}


// Possession field (07.11.2026, measurement): bot/possession_field.h.
// ssbot episode "<position>" [--bot NAME] [--time S] [--turn-depth N] [--frozen MASK] [--possession W]
// The exact count of a danger episode (08.11.2026, user): the selective
// search of a bot of the collection (default -pkr-) over the episode's
// participants only - the others stand - with the possession value (the bot
// keeps the ball; default W = 0.2). Participants: from the best delivery route
// (delivery: the ball's carriers and receivers, the target, the defenders who
// can be by the route, the overloaded ones), or --frozen MASK (the standing
// players). The line is played on: each decision searched, a random action
// along its main outcome (the pass through, a tackle's likeliest), up to a
// shot, a lost ball, a restart or N half-turns (default 12).
int EpisodeCountCommand(const Options& o) {
    GameState s;
    if (!ReadPosition(o, &s)) return 1;
    if (s.phase != Phase::Play) { std::printf("not in play\n"); return 0; }
    unsigned participants = 0xFFu;
    if (o.frozen) {
        participants = ~o.frozen & 0xFFu;
    } else {
        const bot::Delivery d = bot::ComputeDelivery(s, 0.2, 2);
        std::printf("%s", bot::FormatDelivery(s, d).c_str());
        if (!d.valid || d.routes.empty()) { std::printf("no delivery to a danger: nothing to count\n"); return 0; }
        // All the dangers' routes: the episode keeps every threat (28b: B1's
        // route alone left B2, the other runner of the 2 v 1, standing).
        participants = 0;
        for (const bot::DeliveryRoute& r : d.routes) participants |= r.participants;
    }
    std::string who;
    for (int k = 0; k < kNumPlayers; ++k)
        if (participants >> k & 1u) who += (who.empty() ? "" : ",") + PlayerName(k);
    const std::string name = o.botName.empty() ? "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-b8-t90" : o.botName;
    const bot::BotPreset* preset = bot::FindBotPreset(name);
    if (!preset || preset->options.search.selectiveWidth <= 0) { std::fprintf(stderr, "episode: --bot must name a selective bot\n"); return 2; }
    bot::GoalDfsOptions g = preset->options.search;
    g.timeLimit = o.time >= 0 ? o.time : 30;
    if (o.threads >= 0) g.threads = o.threads;
    g.selectivePossessionValue = o.possession >= 0 ? o.possession : 0.2;
    g.frozenPlayers = ~participants & 0xFFu;
    g.selectiveAttackEpisode = false;
    g.selectiveEpisodeExits = false;
    const int team = s.ball.IsFree() ? s.ball.attackTeam : TeamOf(s.ball.holder);
    std::printf("episode of %s, participants %s, %s, possession W %.2f, %.0f s a decision\n", team == kWhite ? "White" : "Black",
                who.c_str(), name.c_str(), g.selectivePossessionValue, g.timeLimit);
    const int halfTurns = o.turnDepth > 0 ? o.turnDepth : 12;
    const int start = s.halfTurns;
    double chance = 1;   // the main outcomes' probability so far
    for (int guard = 0; guard < 8 * halfTurns && s.phase == Phase::Play && s.halfTurns - start < halfTurns; ++guard) {
        bot::GoalDfsSearch search(g);
        const bot::GoalSearchResult r = search.Search(s);
        std::vector<Action> plan = r.turnPlan;
        if (plan.empty()) plan.push_back(Action::EndTurn());
        const int h = s.halfTurns + 1;
        std::string text;
        bool stop = false;
        for (const Action& a : plan) {
            const std::vector<Outcome> outs = OutcomesUnchecked(s, a);
            std::size_t pick = 0;
            for (std::size_t i = 0; i < outs.size(); ++i) {
                if (outs[i].kind == OutcomeKind::BallThrough) { pick = i; break; }
                if (outs[i].probability > outs[pick].probability) pick = i;
            }
            text += (text.empty() ? "" : " ") + ActionText(s, a);
            if (a.type == ActionType::Shot) {
                char buf[64];
                std::snprintf(buf, sizeof buf, " (shot %.3f)", ShotProbability(s, s.ball.holder));
                text += buf;
                stop = true;
                break;
            }
            if (outs.size() > 1) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "{%.2f}", outs[pick].probability);
                text += buf;
                chance *= outs[pick].probability;
            }
            ApplyUnchecked(s, a, outs[pick]);
            if (outs.size() > 1) break;   // after a random action the bot searches again
        }
        const double v = r.value.lower;
        std::printf("  half-turn %d: %s   value %.3f (the episode team's), depth %d\n", h, text.c_str(), v,
                    search.Stats().completedTurnDepth);
        if (stop) break;
        if (s.phase != Phase::Play) { std::printf("  restart: the episode ends\n"); break; }
        if (!s.ball.IsFree() && TeamOf(s.ball.holder) != team) { std::printf("  the ball is lost\n"); break; }
    }
    std::printf("main outcomes' probability along the line %.3f; position %s\n", chance, FormatPosition(s).c_str());
    return 0;
}

int DeliveryCommand(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "delivery: \"<position>\" [PASSES]\n"); return 2; }
    GameState s;
    std::string error;
    if (!ParsePosition(argv[2], &s, &error)) { std::fprintf(stderr, "position: %s\n", error.c_str()); return 2; }
    const int passes = argc > 3 ? std::atoi(argv[3]) : 2;
    const auto t0 = std::chrono::steady_clock::now();
    const bot::Delivery d = bot::ComputeDelivery(s, 0.2, passes);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%s", bot::FormatDelivery(s, d).c_str());
    std::printf("(%.0f ms)\n", ms);
    return 0;
}

int RunBorderCommand(int argc, char** argv) {
    if (argc < 5) { std::fprintf(stderr, "run-border: \"<position>\" RUNNER DEFENDER (e.g. B2 W4)\n"); return 2; }
    GameState s;
    std::string error;
    if (!ParsePosition(argv[2], &s, &error)) { std::fprintf(stderr, "position: %s\n", error.c_str()); return 2; }
    auto player = [](const std::string& name) {
        if (name.size() != 2 || (name[0] != 'W' && name[0] != 'B') || name[1] < '1' || name[1] > '4') return -1;
        return (name[0] == 'W' ? 0 : kPlayersPerTeam) + (name[1] - '1');
    };
    const int runner = player(argv[3]), defender = player(argv[4]);
    const bot::RunBorderResult r = bot::SolveRunBorder(s, runner, defender);
    if (!r.valid) { std::fprintf(stderr, "run-border: bad players\n"); return 2; }
    std::printf("%s against %s: %d rows left, %d files from the centre (%ld nodes)\n", argv[3], argv[4], r.rowsLeft,
                r.fileOffset, r.nodes);
    for (std::size_t i = 0; i < r.cells.size(); ++i)
        std::printf("%s%c%d %s", i ? "  " : "", r.movers[i] < kPlayersPerTeam ? 'W' : 'B', r.movers[i] % kPlayersPerTeam + 1,
                    CellName(r.cells[i]).c_str());
    std::printf("\n");
    return 0;
}

int PossessionFieldCommand(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "possession-field: a file of id<TAB>position lines [W]\n"); return 2; }
    std::ifstream in(argv[2]);
    const double w = argc > 3 ? std::atof(argv[3]) : 0.0;
    const int passes = argc > 4 ? std::atoi(argv[4]) : 2;
    const double territory = argc > 5 ? std::atof(argv[5]) : 0.0;
    std::string line;
    std::printf("id,team,value,white,holder,first,note,frames,transitions,ms\n");
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string id = line.substr(0, tab);
        GameState s;
        if (!ParsePosition(line.substr(tab + 1), &s)) continue;
        const auto t0 = std::chrono::steady_clock::now();
        const bot::PossessionField r = bot::ComputePossessionField(s, w, passes, territory);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!r.valid) { std::printf("%s,,,,,,invalid,,,\n", id.c_str()); continue; }
        std::printf("%s,%c,%.4f,%.4f,%s,%c,%s,%d,%ld,%.2f\n", id.c_str(), r.team == kWhite ? 'W' : r.team == kBlack ? 'B' : '-',
                    r.value, r.white, r.holder >= 0 ? PlayerName(r.holder).c_str() : "-", r.first,
                    r.restart ? "restart" : r.freeBall ? "free" : "held", r.frames, r.transitions, ms);
    }
    return 0;
}

// Possession analysis (07.11.2026): one JSON line per position.
int PossessionAnalysisCommand(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "possession-analysis: a file of id<TAB>position lines [W] [PASSES]\n"); return 2; }
    std::ifstream in(argv[2]);
    const double w = argc > 3 ? std::atof(argv[3]) : 0.2;
    const int passes = argc > 4 ? std::atoi(argv[4]) : 2;
    std::string line;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        GameState s;
        if (!ParsePosition(line.substr(tab + 1), &s)) continue;
        std::printf("%s\t%s\n", line.substr(0, tab).c_str(), bot::AnalyzePossession(s, w, passes).c_str());
    }
    return 0;
}

// Apply a team's moves (07.11.2026, review): id<TAB>position<TAB>moves lines;
// the moves, then End Turn (the ball's step untouched); prints id<TAB>position.
int ApplyMovesCommand(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "apply-moves: a file of id<TAB>position<TAB>moves lines\n"); return 2; }
    std::ifstream in(argv[2]);
    std::string line;
    while (std::getline(in, line)) {
        const auto a = line.find('\t'), b = line.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        GameState s;
        if (!ParsePosition(line.substr(a + 1, b - a - 1), &s)) { std::fprintf(stderr, "parse\n"); continue; }
        std::istringstream moves(line.substr(b + 1));
        std::string token;
        bool ok = true;
        while (moves >> token) {
            Action act;
            if (!ParseAction(s, token, &act) || Validate(s, act) != ActionError::None) { std::fprintf(stderr, "%s: bad %s\n", line.substr(0, a).c_str(), token.c_str()); ok = false; break; }
            ApplyUnchecked(s, act, Outcome::Certain());
        }
        if (!ok) continue;
        const std::vector<Outcome> outcomes = OutcomesUnchecked(s, Action::EndTurn());
        Outcome through = outcomes.front();
        for (const Outcome& o : outcomes)
            if (o.kind == OutcomeKind::BallThrough) through = o;
        ApplyUnchecked(s, Action::EndTurn(), through);
        std::printf("%s\t%s\n", line.substr(0, a).c_str(), FormatPosition(s).c_str());
    }
    return 0;
}

// Calm moves (07.11.2026, review; passes 08.11.2026): in a calm position the
// team with the ball takes only moves after which no legal tackle threatens
// the holder. Candidates: every team movement; every legal pass (to a
// partner's feet or into space) at its end; every legal pass first, then the
// team's movements (the receiver may carry the ball on: g9_16b `>f7 3e6`).
// A pass is valued over all its outcomes, each by its probability; passes
// the reach map gives to the opponent first are not generated (07.11.2026).
// The candidates are ranked by a cheap key (the expected best open shot on
// cells the team reaches first by its next turn), the first TOP_FIELD of them
// get the calm-move function - the possession field at W = 0.2 with the
// advance tie-break 0.02 (see docs).
int CalmMovesCommand(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "calm-moves: a file of id<TAB>position lines [TOP] [TOP_FIELD]\n"); return 2; }
    std::ifstream in(argv[2]);
    const int top = argc > 3 ? std::atoi(argv[3]) : 12;
    const int topField = argc > 4 ? std::atoi(argv[4]) : 100;
    std::string line;
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        GameState s;
        if (!ParsePosition(line.substr(tab + 1), &s) || s.ball.IsFree() || TeamOf(s.ball.holder) != s.teamToMove) continue;
        const int team = s.teamToMove;
        const unsigned ours = team == kWhite ? 0x0Fu : 0xF0u, theirs = ~ours & 0xFFu;
        bot::GoalEpisode ep;
        ep.team = team;
        const bot::TeamMovementList list = bot::EnumerateTeamMovements(s, ep);
        // A candidate: its outcomes (probability, state before the end of the turn).
        struct Cand { std::string text; std::vector<std::pair<double, GameState>> outs; };
        std::vector<Cand> cands;
        long pruned = 0;
        auto passes = [&](const GameState& u) {
            std::vector<Action> out;
            if (u.ball.IsFree() || TeamOf(u.ball.holder) != team || u.ballPlayed) return out;
            const bot::ReachFront front = bot::ComputeReachFront(u);
            for (const Action& a : LegalActions(u)) {
                if (a.type != ActionType::Pass) continue;
                if (PlayerAt(u, a.target) == kNoPlayer && front.stop != bot::ReachFrontStop::InvalidStart &&
                    bot::ReachPassTaker(u, front, a) == Opponent(team)) { ++pruned; continue; }
                out.push_back(a);
            }
            return out;
        };
        auto withPass = [&](const GameState& u, const Action& pass, const std::string& prefix) {
            Cand c{prefix + (prefix.empty() ? "" : " ") + FormatAction(u, pass), {}};
            for (const Outcome& o : OutcomesUnchecked(u, pass)) {
                GameState v = u;
                ApplyUnchecked(v, pass, o);
                c.outs.push_back({o.probability, v});
            }
            return c;
        };
        for (const auto& m : list.states) {
            std::string text;
            for (int k = 0; k < m.count; ++k) text += (k ? " " : "") + FormatAction(s, m.moves[k]);
            cands.push_back({text, {{1.0, m.state}}});
            for (const Action& pass : passes(m.state)) cands.push_back(withPass(m.state, pass, text));
        }
        for (const Action& pass : passes(s)) {
            const std::vector<Outcome> outs = OutcomesUnchecked(s, pass);
            for (std::size_t main = 0; main < outs.size(); ++main) {
                if (outs[main].kind != OutcomeKind::BallThrough && outs[main].kind != OutcomeKind::Certain) continue;
                GameState v = s;
                ApplyUnchecked(v, pass, outs[main]);
                const bot::TeamMovementList after = bot::EnumerateTeamMovements(v, ep);
                for (const auto& m : after.states) {
                    Cand c{FormatAction(s, pass), {}};
                    for (int k = 0; k < m.count; ++k) c.text += " " + FormatAction(v, m.moves[k]);
                    for (std::size_t i = 0; i < outs.size(); ++i) {
                        if (i == main) { c.outs.push_back({outs[i].probability, m.state}); continue; }
                        GameState w = s;   // another outcome: the others do not move after it
                        ApplyUnchecked(w, pass, outs[i]);
                        c.outs.push_back({outs[i].probability, w});
                    }
                    cands.push_back(c);
                }
            }
        }
        // The end of the turn over its outcomes (a rolling ball's step may be touched).
        auto ended = [&](const GameState& u) {
            std::vector<std::pair<double, GameState>> r;
            for (const Outcome& o : OutcomesUnchecked(u, Action::EndTurn())) {
                GameState t = u;
                ApplyUnchecked(t, Action::EndTurn(), o);
                r.push_back({o.probability, t});
            }
            return r;
        };
        // The other side's legal tackles on our holder in its next turn.
        auto threatened = [&](const GameState& t) {
            if (t.phase != Phase::Play || t.ball.IsFree() || TeamOf(t.ball.holder) != team) return false;
            const Cell ball = t.players[t.ball.holder].pos;
            for (int d = FirstPlayerOf(t.teamToMove); d < FirstPlayerOf(t.teamToMove) + kPlayersPerTeam; ++d) {
                const PlayerState& pl = t.players[d];
                for (const Cell q : MoveDestinations(t, d))
                    if (ChebyshevDistance(q, ball) == 1 &&
                        AttackDirectionAllowed(q, ball, pl.Velocity(), {q.x - pl.turnStart.x, q.y - pl.turnStart.y}) &&
                        TackleChance(ball, q, t.teamToMove) > 0) return true;
            }
            return false;
        };
        // The cheap key: the best open shot on cells our team reaches first by its next turn.
        auto key = [&](const GameState& t) {
            if (t.phase != Phase::Play) return 0.0;
            const bot::ReachFront front = bot::ComputeReachFront(t);
            double s2 = 0;
            for (int i = 0; i < bot::kReachFrontCells; ++i) {
                int f = -1;
                for (int k = 0; k < static_cast<int>(front.frames.size()); ++k)
                    if (front.frames[k].players[i]) { f = k; break; }
                if (f < 0 || f > 2 || !(front.frames[f].players[i] & ours)) continue;
                s2 = std::max(s2, OpenShotProbability({i % kFieldWidth, kFirstRow + i / kFieldWidth}, team));
            }
            return s2;
        };
        struct Row { double field = 0, s2 = 0; std::string moves; GameState after; std::vector<std::pair<double, GameState>> ends; };
        std::vector<Row> rows;
        int unsafe = 0;
        for (const Cand& c : cands) {
            Row r;
            r.moves = c.text.empty() ? "end" : c.text;
            double mainP = -1;
            bool bad = false;
            for (const auto& [p, u] : c.outs)
                for (const auto& [q, t] : ended(u)) {
                    r.ends.push_back({p * q, t});
                    if (p * q > mainP) { mainP = p * q; r.after = t; bad = threatened(t); }
                }
            if (bad) { ++unsafe; continue; }
            for (const auto& [p, t] : r.ends) r.s2 += p * key(t);
            rows.push_back(std::move(r));
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.s2 > b.s2; });
        const int fielded = std::min<int>(topField, rows.size());
        for (int i = 0; i < fielded; ++i) {
            double v = 0;
            for (const auto& [p, t] : rows[i].ends) {
                if (t.phase != Phase::Play) continue;
                const bot::PossessionField pf = bot::ComputePossessionField(t, 0.2, 2, 0.02);
                v += p * (pf.team == team ? pf.value : -pf.value);
            }
            rows[i].field = v;
        }
        std::stable_sort(rows.begin(), rows.begin() + fielded, [](const Row& a, const Row& b) { return a.field > b.field; });
        std::printf("%s: %zu movements, %zu candidates with passes (%ld passes the map gives to the opponent), %d unsafe, "
                    "%zu safe, field for the first %d by the key\n",
                    line.substr(0, tab).c_str(), list.states.size(), cands.size(), pruned, unsafe, rows.size(), fielded);
        (void)theirs;
        for (int i = 0; i < std::min<int>(top, fielded); ++i)
            std::printf("  field %+.3f key %.3f  %s\t%s\n", rows[i].field, rows[i].s2, rows[i].moves.c_str(),
                        FormatPosition(rows[i].after).c_str());
    }
    return 0;
}

// Map cost (07.11.2026, measurement): ComputeReachFront per position, and the
// static for comparison, repeated to time small values.
int ReachTiming(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "reach-timing: a file of id<TAB>position lines\n"); return 2; }
    std::ifstream in(argv[2]);
    std::string line;
    std::printf("id,frames,map_ms,static_ms\n");
    while (std::getline(in, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        GameState s;
        if (!ParsePosition(line.substr(tab + 1), &s)) continue;
        const int reps = 20;
        size_t frames = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; ++r) frames += bot::ComputeReachFront(s).frames.size();
        const double mapMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps;
        double sink = 0;
        t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; ++r) sink += bot::GeometricStaticEstimate(s, bot::GoalEpisode{});
        const double stMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / reps;
        std::printf("%s,%zu,%.4f,%.4f%s\n", line.substr(0, tab).c_str(), frames / reps, mapMs, stMs, sink > 1e9 ? "!" : "");
    }
    return 0;
}

// Possessions in games (07.11.2026, diagnostics): a team's possession runs
// from its gaining the ball (a restart for it, a catch, a tackle, a pickup)
// until the other team holds it, restarts or concedes. Per start kind and
// per player name of the team (human / bot): possessions, goals, shots, turns.
int PossessionStats(int argc, char** argv) {
    struct Tally { long n = 0, goals = 0, shots = 0, halfTurns = 0; };
    std::map<std::string, Tally> table;
    for (int i = 2; i < argc; ++i) {
        std::ifstream f(argv[i]);
        if (!f) continue;
        std::stringstream buffer;
        buffer << f.rdbuf();
        GameRecord record;
        std::string error;
        if (!ReadRecord(buffer.str(), &record, &error)) continue;
        const std::string text = buffer.str();
        auto tag = [&](const char* name) {
            const std::string key = std::string("[") + name + " \"";
            const auto a = text.find(key);
            if (a == std::string::npos) return std::string("?");
            const auto b = text.find('"', a + key.size());
            return text.substr(a + key.size(), b - a - key.size());
        };
        const std::string who[2] = {tag("White") == "human" ? "human" : "bot", tag("Black") == "human" ? "human" : "bot"};
        GameState s = RecordStart(record);
        int owner = -1, startHalf = 0;
        std::string kind;
        Tally current;
        auto close = [&](bool goal) {
            if (owner < 0) return;
            Tally& t = table[who[owner] + " " + kind];
            Tally& all = table[who[owner] + " all"];
            for (Tally* x : {&t, &all}) {
                ++x->n;
                x->goals += goal;
                x->shots += current.shots;
                x->halfTurns += s.halfTurns - startHalf;
            }
            owner = -1;
        };
        auto open = [&](int team, const std::string& k) {
            owner = team;
            kind = k;
            startHalf = s.halfTurns;
            current = Tally();
        };
        for (std::size_t j = 0; j < record.actions.size(); ++j) {
            const RecordedAction& a = record.actions[j];
            const bool wasLineup = s.phase == Phase::Lineup;
            EventLog events;
            if (ApplyOutcome(s, a.action, a.outcome, &events) != ActionError::None) break;
            for (const Event& e : events) {
                if (e.type == EventType::ShotTaken && owner >= 0) ++current.shots;
                if (e.type == EventType::Goal) { close(e.team == owner); }
            }
            if (s.phase == Phase::Play && wasLineup && !s.ball.IsFree()) {
                // a restart: the restarting team has the ball at the centre
                close(false);
                open(TeamOf(s.ball.holder), "restart");
                continue;
            }
            if (s.phase == Phase::Play && !s.ball.IsFree() && TeamOf(s.ball.holder) != owner) {
                const bool fromFree = a.action.type == ActionType::Move || a.action.type == ActionType::EndTurn;
                const std::string k = a.action.type == ActionType::Pass ? "interception"
                                    : a.action.type == ActionType::Attack ? "tackle" : fromFree ? "free ball" : "other";
                close(false);
                open(TeamOf(s.ball.holder), k);
            }
            if (s.phase == Phase::Lineup && owner >= 0 && s.teamToMove != owner) close(false);
        }
        close(false);
    }
    std::printf("%-22s %8s %8s %8s %10s %12s\n", "side / start", "poss", "goals", "P(goal)", "shots/poss", "half-turns");
    for (const auto& [k, t] : table)
        std::printf("%-22s %8ld %8ld %8.3f %10.2f %12.1f\n", k.c_str(), t.n, t.goals, t.n ? double(t.goals) / t.n : 0.0,
                    t.n ? double(t.shots) / t.n : 0.0, t.n ? double(t.halfTurns) / t.n : 0.0);
    return 0;
}

int Timing(int argc, char** argv) {
    std::vector<std::string> files, bots{"goal-sel-run-fb-b8-t120"};
    std::vector<double> budgets{2.5, 5, 10, 20, 40, 80};
    int every = 4, maxPositions = 16, threads = 0;
    std::string out;
    auto split = [](const std::string& text) {
        std::vector<std::string> v;
        std::stringstream ss(text);
        std::string item;
        while (std::getline(ss, item, ',')) if (!item.empty()) v.push_back(item);
        return v;
    };
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--bots") bots = split(next());
        else if (a == "--budgets") {
            budgets.clear();
            for (const std::string& b : split(next())) budgets.push_back(std::atof(b.c_str()));
        } else if (a == "--every") every = std::max(1, std::atoi(next().c_str()));
        else if (a == "--max") maxPositions = std::atoi(next().c_str());
        else if (a == "--threads") threads = std::atoi(next().c_str());
        else if (a == "--out") out = next();
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        } else files.push_back(a);
    }
    for (const std::string& b : bots)
        if (!bot::FindBotPreset(b)) {
            std::fprintf(stderr, "unknown bot %s (ssbot bots)\n", b.c_str());
            return 2;
        }
    std::sort(budgets.begin(), budgets.end());
    if (files.empty() || budgets.empty() || bots.empty()) {
        std::fprintf(stderr, "usage: ssbot timing <game.ssn>... [--bots a,b] [--budgets 2.5,5,...] [--every N] [--max M]\n");
        return 2;
    }

    // The positions.
    std::vector<GameState> positions;
    std::vector<uint64_t> seen;
    for (const std::string& file : files) {
        bool ok = false;
        std::string text = ReadFile(file, &ok);
        GameRecord record;
        std::string error;
        if (!ok || !ReadRecord(text, &record, &error)) {
            std::fprintf(stderr, "%s: %s\n", file.c_str(), ok ? error.c_str() : "can not read");
            continue;
        }
        GameState st = RecordStart(record);
        int lastTurn = -1, count = 0;
        for (const RecordedAction& r : record.actions) {
            if (st.phase == Phase::Play && st.turnId != lastTurn) {
                lastTurn = st.turnId;
                const uint64_t h = bot::PositionHash(st);
                if (count++ % every == 0 && std::find(seen.begin(), seen.end(), h) == seen.end() &&
                    static_cast<int>(positions.size()) < maxPositions) {
                    seen.push_back(h);
                    positions.push_back(st);
                }
            }
            ApplyOutcome(st, r.action, r.outcome, nullptr);
        }
    }
    std::printf("%zu positions, bots:", positions.size());
    for (const std::string& b : bots) std::printf(" %s", b.c_str());
    std::printf(", budgets:");
    for (double b : budgets) std::printf(" %g", b);
    double total = 0;
    for (double b : budgets) total += b;
    std::printf(" s (at most ~%.0f min)\n", total * positions.size() * bots.size() / 60);
    std::FILE* csv = out.empty() ? nullptr : std::fopen(out.c_str(), "w");
    if (csv) std::fprintf(csv, "position,bot,budget,seconds,value,actions,fen\n");

    struct Run {
        std::string actions;
        double value, seconds;
    };
    // [bot][position][budget]
    std::vector<std::vector<std::vector<Run>>> runs(bots.size(), std::vector<std::vector<Run>>(positions.size()));
    for (std::size_t pi = 0; pi < positions.size(); ++pi) {
        const GameState& s = positions[pi];
        for (std::size_t bi = 0; bi < bots.size(); ++bi) {
            for (double budget : budgets) {
                bot::BotOptions po = bot::FindBotPreset(bots[bi])->options;
                po.timeLimit = budget;
                po.threads = threads;
                bot::Bot b(po);
                auto start = std::chrono::steady_clock::now();
                // The macro: the bot's actions to the first random one or the end of the turn.
                GameState t = s;
                std::string acts;
                for (int guard = 0; guard < 24; ++guard) {
                    const Action a = b.NextAction(t);
                    acts += (acts.empty() ? "" : " ") + ActionText(t, a);
                    std::vector<Outcome> outs = OutcomesUnchecked(t, a);
                    if (Validate(t, a) != ActionError::None || outs.size() != 1) break;
                    ApplyUnchecked(t, a, outs.front());
                    if (t.teamToMove != s.teamToMove || t.turnId != s.turnId || t.phase != Phase::Play) break;
                }
                const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                const double v = b.LastSearch().value.lower;
                runs[bi][pi].push_back({acts, v, sec});
                std::printf("position %2zu  %-10s %5gs  %6.1fs  value %7.3f  %s\n", pi + 1, bots[bi].c_str(), budget, sec, v,
                            acts.c_str());
                std::fflush(stdout);
                if (csv) {
                    std::fprintf(csv, "%zu,%s,%g,%.2f,%.4f,\"%s\",\"%s\"\n", pi + 1, bots[bi].c_str(), budget, sec, v,
                                 acts.c_str(), FormatPosition(s).c_str());
                    std::fflush(csv);
                }
            }
        }
    }
    if (csv) std::fclose(csv);

    std::printf("\nagreement with the longest budget (%gs): same macro, %%; mean |value - its value|; mean seconds\n",
                budgets.back());
    for (std::size_t bi = 0; bi < bots.size(); ++bi) {
        std::printf("%s\n", bots[bi].c_str());
        for (std::size_t k = 0; k < budgets.size(); ++k) {
            int same = 0, nv = 0;
            double dv = 0, sec = 0;
            for (std::size_t pi = 0; pi < positions.size(); ++pi) {
                const Run& r = runs[bi][pi][k];
                const Run& ref = runs[bi][pi].back();
                same += r.actions == ref.actions;
                sec += r.seconds;
                if (!std::isnan(r.value) && !std::isnan(ref.value)) {
                    dv += std::fabs(r.value - ref.value);
                    ++nv;
                }
            }
            const double n = positions.empty() ? 1 : double(positions.size());
            std::printf("  %6gs  same %5.1f%%  dvalue %6.3f  %6.1fs\n", budgets[k], 100.0 * same / n, nv ? dv / nv : 0.0,
                        sec / n);
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage:\n"
                     "  ssbot bots\n"
                     "  ssbot selfplay [--white BOT|random] [--black BOT|random] [--halfturns N] [--maxactions N] [--seed S]\n"
                     "                 [--start-position POSITION] [--episode-turns N] [--trace file.csv] [--out file]\n"
                     "                 [--time S] [--wtime S] [--btime S] [--threads N] [--silent]\n"
                     "  ssbot search \"<position>\" --bot BOT [--time S] [--turn-depth N] [--width W] [--root-as-node] [--tree-width W] [--tree-c C]\n"
                     "               [--tree-temp T] [--tree-widen B E] [--tree-share F] [--tree-sims N]\n"
                     "               [--tree-no-transpositions] [--tree-categories | --tree-no-categories] [--race-defence] [--profile-tree]\n"
                     "  ssbot estimate \"<position>\" --bot BOT [--race-defence]\n"
                     "  ssbot goal-static \"<position>\"\n"
                     "  ssbot attack-episode \"<position>\"\n"
                     "  ssbot goal-dfs | goal-team-dfs | goal-team-moves \"<position>\" [--time S] [--expansions N]\n"
                     "               [--memory-mb M] [--action-depth N | --turn-depth N] [--threads N]\n"
                     "  ssbot team-moves \"<position>\"\n"
                     "  ssbot reach-front \"<position>\" [--turn-depth N]\n"
                     "  ssbot reach-pass \"<position>\"\n"
                     "  ssbot replay <file>\n"
                     "  ssbot positions <file>\n"
                     "  ssbot stats <file>...\n"
                     "  ssbot ball-race \"<position>\" | <game.ssn>... [--verbose]\n"
                     "  ssbot timing <game.ssn>... [--bots a,b] [--budgets 2.5,5,10,20,40,80] [--every N] [--max M]\n"
                     "               [--threads T] [--out f.csv]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "bots") {
        for (const bot::BotPreset& p : bot::BotPresets())
            std::printf("%-24s %s%s\n", p.name.c_str(), p.description.c_str(), p.name == bot::DefaultBotName() ? " [default]" : "");
        return 0;
    }
    if (cmd == "stats") return Stats(argc, argv);
    if (cmd == "timing") return Timing(argc, argv);
    if (cmd == "ball-race") return BallRaceCommand(argc, argv);
    if (cmd == "reach-prune-check") return ReachPruneCheck(argc, argv);
    if (cmd == "reach-breakaway-check") return ReachBreakawayCheck(argc, argv);
    if (cmd == "reach-counter-check") return ReachCounterCheck(argc, argv);
    if (cmd == "route-static") return RouteStaticCommand(argc, argv);
    if (cmd == "possession-stats") return PossessionStats(argc, argv);
    if (cmd == "reach-timing") return ReachTiming(argc, argv);
    if (cmd == "delivery") return DeliveryCommand(argc, argv);
    if (cmd == "run-border") return RunBorderCommand(argc, argv);
    if (cmd == "possession-field") return PossessionFieldCommand(argc, argv);
    if (cmd == "possession-analysis") return PossessionAnalysisCommand(argc, argv);
    if (cmd == "apply-moves") return ApplyMovesCommand(argc, argv);
    if (cmd == "calm-moves") return CalmMovesCommand(argc, argv);
    Options o;
    if (!ParseOptions(argc, argv, 2, o)) return 2;
    if (cmd == "selfplay") return SelfPlay(o);
    if (cmd == "replay") return Replay(o);
    if (cmd == "positions") return Positions(o);
    if (cmd == "goal-dfs") return ExactCommand(o, false, false);
    if (cmd == "goal-team-dfs") return ExactCommand(o, true, false);
    if (cmd == "goal-team-moves") return ExactCommand(o, true, true);
    if (cmd == "goal-static") return GoalStaticCommand(o);
    if (cmd == "attack-episode") return AttackEpisodeCommand(o);
    if (cmd == "team-moves") return TeamMovesCommand(o);
    if (cmd == "reach-front") return ReachFrontCommand(o);
    if (cmd == "reach-pass") return ReachPassCommand(o);
    if (cmd == "ball-plans") return BallPlansCommand(o);
    if (cmd == "plan-residual") return PlanResidualCommand(o);
    if (cmd == "episode-moves") return EpisodeMovesCommand(o);
    if (cmd == "ball-chains") return BallChainsCommand(o);
    if (cmd == "ball-routes") return BallRoutesCommand(o);
    if (cmd == "route-tables") return RouteTablesCommand(o);
    if (cmd == "episode-table") return EpisodeTableCommand(o);
    if (cmd == "search") return SearchCommand(o);
    if (cmd == "episode") return EpisodeCountCommand(o);
    if (cmd == "estimate") return EstimateCommand(o);
    if (cmd == "episode-value") return EpisodeValueCommand(o);
    if (cmd == "table-bench") return TableBenchCommand(o);
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
