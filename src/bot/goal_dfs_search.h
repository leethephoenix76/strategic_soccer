// The bot's search over the current attack and the immediate counterattack
// (docs/bot_design.md). Two modes share the engine's exact outcomes:
// - exact (selectiveWidth == 0): every legal decision, memory-bounded DFS that
//   keeps only root bounds, the current path and a replaceable cache - the
//   reference (goal-team-moves);
// - selective (selectiveWidth > 0): a beam of candidate decisions valued by the
//   static estimate, as a fixed-depth alpha-beta or a best-first tree.
#pragma once

#include "bot/goal_episode.h"
#include "engine/rules.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace soccer {
namespace bot {

struct GoalRootMove {
    Action action;
    GoalValueBounds bounds;
    bool opened = false;
};

struct GoalSearchResult {
    Action action = Action::EndTurn();
    // A legal deterministic prefix of the chosen team turn, ending at the
    // first ball action or EndTurn. Empty when the search has no such line.
    std::vector<Action> turnPlan;
    GoalValueBounds value;
    GoalValueBounds rootBounds;
    double regretBound = 2;
    bool proven = false;
    long positions = 0;
    long expansions = 0;
    int maxDepth = 0;        // half-turns reached below the root
    double seconds = 0;
    double estimatedMemoryMB = 0;
    bool memoryLimited = false;
    std::vector<GoalRootMove> moves;
    std::string note;
};

// Diagnostics (07.11.2026, P1 - the inner node against the root): a decision
// to trace through the stages of the selective inner node at `root` - the
// movement beam, the choice of movement states, the decision beam. Reports to
// stderr. Process-wide; set by ssbot node-diag.
struct NodeDiagTarget {
    bool active = false;
    GameState root;
    std::vector<GameState> prefixes;   // after 1..k of its moves (the last: the movement state)
    Action action;                     // its first ball action or EndTurn
};
void SetNodeDiag(const NodeDiagTarget& target);

struct GoalDfsOptions {
    double timeLimit = 120;           // <= 0: no wall-clock limit
    long expansions = 0;              // 0: no expansion limit
    std::size_t maxMemoryBytes = 64u * 1024u * 1024u;
    int maxActionDepth = 0;           // 0: keep deepening until proof or another limit
    int maxTurnDepth = 0;             // 0: keep deepening until proof or another limit
    bool completeTurns = false;       // count completed team turns instead of actions
    bool groupMovements = false;      // enumerate distinct move-reachable states within a turn
    int selectiveWidth = 0;           // > 0: heuristic beam per decision, approximate search
    bool selectiveGreedyMovements = false; // add legal forward team completions to the beam
    bool selectiveEndTurnPreview = false;   // rank states and EndTurn after the next ball step
    int selectiveEndTurnPreviewTop = 0;     // > 0: the EndTurn preview only for this many movement states,
                                            // the best by their own estimate (beam); 0: every state
    bool selectiveBallDiversity = false;    // retain distinct ball holders and cells
    bool selectiveAlphaBeta = false;        // windows within the same beam tree (same choice)
    bool selectiveTransposition = false;    // reuse same-depth values of the beam tree (same choice)
    bool selectiveRestartPosition = false;  // a restart is valued as the standard restart position
    bool selectiveBallStepPreview = false;  // a rolling free ball is valued after its next step
    bool selectiveKindDiversity = false;    // the beam keeps the best decision of every kind
    bool selectiveRaceFreeBall = false;     // a free ball is valued by the race for it (race.h)
    bool selectiveReplyAware = false;       // the attack's best plan against the defenders' reply
    double selectiveRareOutcomeProbability = 0; // > 0: outcomes less likely are valued statically
    double selectivePossessionValue = 0;  // (07.11) W: the scale "who scores next" - a restart, a lost ball and the end of
                                          // an episode without a goal are worth +-W for the team with the ball; 0 = off
    bool selectiveContinuation = false;     // possession beyond the episode valued by the possession table
    bool selectiveGeometricStatic = false;  // position-only attack graph; no possession table or reply search
    bool selectiveLegalTackleZones = false; // check blocked moves and attack direction for next-turn tackles
    double selectiveThreatenedRetention = 1.0; // scale the attack value if a legal tackle exists
    bool selectiveForcedPickupPreview = false; // evaluate a free ball as held only when the next pickup is forced
    bool selectiveExclusiveAttackChannels = false; // choose one immediate pass or holder attack, not their union
    bool selectiveCoverageStatic = false;   // turn-order static; one task per defender, the attack's best open channel
    bool selectiveTurnOrderStatic = false;
    bool selectiveTimedFreeBall = false;       // coverage static: a free ball goes to the first player to reach its path
    bool selectiveTerminalFreeBall = false;    // coverage static: value a path ending with a pickup, out or keeper
    bool selectiveBallRace = false;            // leaves with a free ball: who reaches it first (ball_race.h) -
                                               // the state after a certain take or restart; a contested race
                                               // is searched further (see selectiveBallRaceExtensions)
    int selectiveBallRaceExtensions = 1;       // extra team turns on a line for contested races at the horizon
    bool selectiveEpisodeExtension = false;    // a leaf whose attack episode can reach its best shooting cell
                                               // within two rounds is searched one more round (two team turns)
    int selectiveEpisodeExtensionLimit = 1;    // such extensions on one line
    bool selectiveReachExtension = false;      // (07.11) the same extension also where the reach map gives the
                                               // ball holder, first of all players, a cell within two rounds
                                               // whose open shot exceeds the leaf's value for his team
    bool selectiveExtensionInWindow = false;   // extend only a leaf whose own estimate lies inside the
                                               // alpha-beta window (one that can change the result)
    int selectiveExtensionWidth = 0;           // > 0: the beam width inside an episode extension (a cheaper
                                               // look one round further); 0: selectiveWidth
    bool selectiveBreakawayBound = false;      // leaves of a 1 v 0 attack episode: the static is raised to a
                                               // proven lower bound of the holder's goal probability (ball_race.h)
    bool selectiveBreakawayAnyEpisode = false; // the bound in every leaf with a held ball, not only 1 v 0
    bool selectiveRaceRanking = false;         // with selectiveBallRace: decisions are ranked (the detailed
                                               // estimate of the beam) with the race too, as their leaves are
    bool selectiveTwoVsOneBound = false;       // leaves of a 2 v 1 attack episode: also the 2 v 1 lower bound
    // The episode table (bot_design.md, "Таблица эпизода", 02.11.2026): a leaf in a 1 v 0 or 2 v 1
    // attack episode is worth at least the minimax value of the episode over its participants
    // (episode_value.h), computed lazily with a node limit and cached.
    bool selectiveEpisodeTable = false;
    std::string tableDumpPath;   // diagnostics: every computed episode table is appended here (ssbot table-bench)
    // The ranking of partial turns (movement states, the root's first actions)
    // values a free ball by the race solver, as the leaves do (02.11.2026).
    bool selectiveRaceMovements = false;
    // Pickup diversity (02.11.2026): a move that takes the free ball is its own
    // kind among the root's first actions and keeps a place in each ply of the
    // movement beam.
    int selectivePickupDiversity = 0;   // 0: off, 1: the root's first actions, 2: and the movement beam
    bool selectiveReachPassRootKind = false; // a possible own reception along a space pass gets its own root kind
    bool selectiveReachPassPrune = false;    // (07.11) a pass to an empty cell whose untouched ball the reach map gives to the opponent first is not generated
    bool selectiveRootSplit = false;  // threads > 1: split the root decisions instead of the ranking estimates
    bool profileRoot = false;    // diagnostics: the note lists each root decision's seconds per iteration
    unsigned frozenPlayers = 0;  // the exact search (goal-team-moves): bit k - player k never moves
    int selectiveEpisodeTableTurns = 4;
    long selectiveEpisodeTableNodes = 150000;   // movement states per table (~0.15 s)
                                               // (the defender seen before the attackers choose; ball_race.h)
    int selectiveStaticCacheBits = 17;         // geometric static: a cache of 2^bits values per search thread,
                                               // keyed by the full state (same results, fewer evaluations); 0: off
    bool selectiveNoShotFloor = false;         // coverage static: no max with an unavailable shot (keeps negative values)
    bool selectiveTimedRoute = false;          // coverage static: a defender blocks a run where he arrives in time (inertia)
    bool selectiveCategoryRoot = false;        // ball team's root: complete team turns grouped by channel category
    int selectiveCategoryShortlist = 3;        // category root: decisions per category refined by a short search
    int selectiveCategoryRefineDepth = 2;      // category root: team turns of that refinement (own turn + reply)
    bool selectivePartnerCategories = false;   // decision kinds of the ball team: hold, shot, pass to each partner
    bool selectiveStateDiversity = false;      // decision beam: each kept movement state brings its best decision
    int selectiveMovementWidth = 0;            // movement beam per ply (SelectiveMovements); 0: the beam width (06.11)
    int selectiveBallFirstExtra = 0;           // decision beam: + the best N ball actions of the unmoved state (06.11)
    bool selectiveRootDepthAverage = false;    // (09.11, the user) root decision by the mean of its values at the last two
                                               // depths (single-threaded root)
    bool selectiveRoundDepth = false;          // (09.11, the user) root decision only from depths ending after the
                                               // opponent's reply (even team turns; depth 1 as a fallback), step 2
    bool selectiveDefenceForced = false;       // (09.11, the user) the side without the attack keeps every tackle and
                                               // every interception it can make this turn in its beams, beyond the width
    int selectiveRefineStates = 0;             // inner node: the best N movement states ranked again by a short search (06.11)
    bool selectiveLossReachCheck = false;      // (07.11) with selectiveMainOutcome: a lost ball whose catcher the reach map
                                               // gives an open shot above his static within two rounds is searched
                                               // (rest of this turn and his turn) instead of valued statically
    bool selectiveMainOutcome = false;         // random actions (07.11): a pass is searched only along "ball through", a tackle not along the ball knocked away; the rest is the static after the end of the turn
    bool selectivePlayerDiversity = false;     // movement beam: the best move of each player first (06.11)
    bool selectiveLineageDiversity = false;    // movement beam and kept states: the best of each first move first (06.11)
    bool selectiveBallFirstKinds = false;      // decision beam: a ball action before any movement is its own kind (06.11)
    bool selectiveRootAsNode = false;          // beam root: whole team movements + ball action, as at inner nodes
    bool selectivePartialTurnRealized = false; // geometric static: rank a partial turn by the moves made, unmoved players of the team to move stay  // geometric static: the defence's reply before later shots, passes and receptions
    int selectiveSupportHorizon = 0;        // > 0: the attack's options over 2..N own turns by reach (7.63)
    bool selectiveRaceDefence = false;      // the free ball by the race from the state as it is (no step
                                            //   preview), the defenders' turns before the pickup (7.71)
    bool selectiveAttackEpisode = false;    // players outside the attack episode by the race to the goal
                                            // move by a fixed policy, not searched (episode.h, 7.67)
    bool selectiveEpisodeExits = false;     // the ball reaching a player outside the episode is a leaf
    bool selectiveEpisodeBallRace = false;  // the attack episode of a free ball by the race solver: its
                                            // carrier is who certainly takes it first (bot_design.md 4.4)
    // Best-first tree over the same candidates and static estimate instead of
    // the fixed-depth beam (docs/bot_design.md, 7.68): a node is a decision of
    // the team to move (its movements and one ball action or EndTurn), the
    // engine's exact outcomes follow; PUCT and progressive widening decide
    // which lines grow. Values are backed up exactly (max / min / expectation).
    bool selectiveTree = false;
    int selectiveTreeWidth = 0;             // candidates per node; 0: twice selectiveWidth
    double selectiveTreeCPuct = 0.2;        // exploration, in goals
    double selectiveTreeTemperature = 0.05; // prior: softmax of the candidates' estimates, in goals
    double selectiveTreeWidenBase = 0;      // > 0: progressive widening, a node with N visits has
    double selectiveTreeWidenExponent = 0.5;//   base * (N + 1)^exponent candidates open; 0: all open
    double selectiveTreeFinalShare = 0.1;   // the chosen candidate: the best value among those with
                                            //   at least this share of the most visited one's visits
    bool selectiveTreeRootHalving = true;   // the root's candidates by sequential halving: equal
                                            //   simulations per phase, the better half goes on
    bool selectiveTreeBallCategories = false;// the ball team's candidates by the ball's category:
                                            //   keep, shoot, pass to each partner (7.69)
    bool selectiveTreeTackleSlot = false;     // one existing-width decision for a legal tackle
                                            //   after at most one defender move
    bool selectiveTreeTranspositions = true;// one node per state and episode, however reached
    bool selectiveTreeReuse = true;         // keep the tree between searches
    long selectiveTreeSimulations = 0;      // > 0: a fixed number of simulations (deterministic)
    bool profileTree = false;                // diagnostic timing of tree search phases
    int threads = 0;                 // 0: all available cores; 1: deterministic sequential search
};

struct GoalDfsStats {
    int completedActionDepth = 0;
    int completedTurnDepth = 0;
    long moveExpansions = 0;
    long passExpansions = 0;
    long shotExpansions = 0;
    long attackExpansions = 0;
    long endTurnExpansions = 0;
    long ballStepExpansions = 0;      // subset of EndTurn expansions
    long placeExpansions = 0;
    long outcomesProcessed = 0;
    long cutoffs = 0;
    long forcedPickupPrunes = 0;
    long nextStepPickupPrunes = 0;
    long noRecoveryPrunes = 0;
    long cacheReplacements = 0;
    long cacheHits = 0;
    long selectiveMovementDrops = 0;
    long defenceForcedKept = 0;        // decisions and movement states kept by selectiveDefenceForced (09.11)
    long selectiveRefinedStates = 0;   // movement states ranked by a short search (06.11)
    long reachPassPrunes = 0;     // passes not generated: the reach map gives the ball to the opponent first (07.11)
    long reachMaps = 0;           // reach maps computed for pass pruning and leaf detection
    long lossChecks = 0;          // lost balls searched by the reach map's detector (07.11)
    long reachExtensions = 0;     // leaves extended by the reach map's breakaway detector (07.11)
    long mainOutcomeStatic = 0;   // outcomes of passes and tackles valued by the static only (07.11)
    long selectiveActionDrops = 0;
    long selectiveTableCuts = 0;      // selective nodes answered by the transposition table
    long selectiveCategoryEdges = 0;  // tree candidates added by the ball's categories (7.69)
    std::size_t cacheEntries = 0;
    double peakMemoryMB = 0;
};

// Best shot by the current holder now or after that holder's one legal move,
// with all other players fixed. Zero when this team cannot shoot this turn.
double HolderShotChanceThisTurn(const GameState& state);

struct GoalStaticSummary {
    double attackThreat = 0;
    double directPassThreat = 0;
    double leadPassThreat = 0;
    double keepChance = 1;
    double counterThreat = 0;
};

// Diagnostic components of the selective search's position estimate.
GoalStaticSummary EvaluateGoalStatic(const GameState& state);

class GoalDfsSearch {
public:
    explicit GoalDfsSearch(const GoalDfsOptions& options = GoalDfsOptions());
    ~GoalDfsSearch();
    GoalDfsSearch(const GoalDfsSearch&) = delete;
    GoalDfsSearch& operator=(const GoalDfsSearch&) = delete;

    GoalSearchResult Search(const GameState& state, double seconds = -1);
    GoalDfsStats Stats() const;
    // The selective static estimate of a state for the episode of the attacking
    // team `episodeTeam` (diagnostics); `afterEndTurn`: of the state after EndTurn.
    double SelectiveEstimate(const GameState& state, int episodeTeam, bool afterEndTurn) const;
    // The leaf value as the search uses it (free ball by the race, 1 v 0 / 2 v 1 floors).
    double SelectiveLeafEstimate(const GameState& state, int episodeTeam, bool afterEndTurn) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bot
}  // namespace soccer
