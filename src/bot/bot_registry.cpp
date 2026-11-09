#include "bot/bot_registry.h"

namespace soccer {
namespace bot {

namespace {

// The selective search of goal-sel-run-ep (docs/bot_design.md): a team-turn
// beam of width 8 with alpha-beta and transpositions, the reply-aware static
// with the possession beyond the episode, the support over three own turns and
// the attack episode by the race to the goal.
GoalDfsOptions RunEpisodeSearch() {
    GoalDfsOptions g;
    g.completeTurns = true;
    g.groupMovements = true;
    g.selectiveWidth = 8;
    g.selectiveGreedyMovements = true;
    g.selectiveEndTurnPreview = true;
    g.selectiveBallDiversity = true;
    g.selectiveAlphaBeta = true;
    g.selectiveTransposition = true;
    g.selectiveRestartPosition = true;
    g.selectiveBallStepPreview = true;
    g.selectiveKindDiversity = true;
    g.selectiveRaceFreeBall = true;
    g.selectiveReplyAware = true;
    g.selectiveRareOutcomeProbability = 0.02;
    g.selectiveContinuation = true;
    g.selectiveSupportHorizon = 3;
    g.selectiveAttackEpisode = true;
    g.selectiveEpisodeExits = true;
    g.maxMemoryBytes = 512u * 1024u * 1024u;
    return g;
}

std::vector<BotPreset> Make() {
    std::vector<BotPreset> v;
    {
        // The exact reference: every legal decision, no static estimate.
        BotPreset p;
        p.name = "goal-team-moves";
        p.description = "exact team-turn goal search over distinct move-reachable states (reference)";
        p.options.search.completeTurns = true;
        p.options.search.groupMovements = true;
        p.options.timeLimit = 120;
        v.push_back(p);
    }
    for (int seconds : {90, 120, 180, 300}) {
        const std::string t = "-b8-t" + std::to_string(seconds);
        const std::string per = ", " + std::to_string(seconds) + " seconds per team turn";
        BotPreset run;
        run.name = "goal-sel-run-ep" + t;
        run.description = "fixed-depth beam with the attack episode by the race to the goal" + per;
        run.options.search = RunEpisodeSearch();
        run.options.timeLimit = seconds;
        v.push_back(run);

        // The free ball by the race from the state as it is - no step preview
        // that skips the mover's turn - with the defenders' turns before the
        // pickup (7.71).
        BotPreset runFb = run;
        runFb.name = "goal-sel-run-fb" + t;
        runFb.description = "goal-sel-run-ep-b8 with the free ball by the race from the state as it is" + per;
        runFb.options.search.selectiveRaceDefence = true;
        v.push_back(runFb);

        BotPreset geometric = runFb;
        geometric.name = "goal-sel-run-geo" + t;
        geometric.description = "fixed-depth beam with the geometric attack graph" + per;
        geometric.options.search.selectiveGeometricStatic = true;
        geometric.options.search.selectiveContinuation = false;
        geometric.options.search.selectiveReplyAware = false;
        geometric.options.search.selectiveSupportHorizon = 0;
        geometric.options.search.selectiveRaceFreeBall = false;
        geometric.options.search.selectiveRaceDefence = false;
        geometric.options.search.selectiveBallStepPreview = false;
        v.push_back(geometric);

        BotPreset geometricPickup = geometric;
        geometricPickup.name = "goal-sel-run-geo-pickup" + t;
        geometricPickup.description = "geometric beam with certified next-step free-ball pickup" + per;
        geometricPickup.options.search.selectiveForcedPickupPreview = true;
        v.push_back(geometricPickup);

        BotPreset geometricExclusive = geometric;
        geometricExclusive.name = "goal-sel-run-geo-exclusive" + t;
        geometricExclusive.description = "geometric beam with one immediate attack channel" + per;
        geometricExclusive.options.search.selectiveExclusiveAttackChannels = true;
        v.push_back(geometricExclusive);

        BotPreset geometricPickupExclusive = geometricPickup;
        geometricPickupExclusive.name = "goal-sel-run-geo-pickup-exclusive" + t;
        geometricPickupExclusive.description = "geometric beam with forced pickup and one attack channel" + per;
        geometricPickupExclusive.options.search.selectiveExclusiveAttackChannels = true;
        v.push_back(geometricPickupExclusive);

        BotPreset geometricOrder = geometric;
        geometricOrder.name = "goal-sel-run-geo-order" + t;
        geometricOrder.description = "geometric beam with the defence's reply before later attack actions" + per;
        geometricOrder.options.search.selectiveTurnOrderStatic = true;
        v.push_back(geometricOrder);

        BotPreset geometricOrderExclusive = geometricOrder;
        geometricOrderExclusive.name = "goal-sel-run-geo-order-exclusive" + t;
        geometricOrderExclusive.description = "turn-order geometric beam with one attack channel" + per;
        geometricOrderExclusive.options.search.selectiveExclusiveAttackChannels = true;
        v.push_back(geometricOrderExclusive);

        BotPreset geometricCover = geometricOrder;
        geometricCover.name = "goal-sel-run-geo-cover" + t;
        geometricCover.description = "turn-order geometric beam; each defender covers one attack channel" + per;
        geometricCover.options.search.selectiveCoverageStatic = true;
        v.push_back(geometricCover);

        BotPreset geometricCoverRealized = geometricCover;
        geometricCoverRealized.name = "goal-sel-run-geo-cover-realized" + t;
        geometricCoverRealized.description = "covering geometric beam; a partial turn ranked by the moves made" + per;
        geometricCoverRealized.options.search.selectivePartialTurnRealized = true;
        v.push_back(geometricCoverRealized);

        BotPreset geometricCoverRoot = geometricCoverRealized;
        geometricCoverRoot.name = "goal-sel-run-geo-cover-root" + t;
        geometricCoverRoot.description = "covering realized beam; the root searched as an ordinary team node" + per;
        geometricCoverRoot.options.search.selectiveRootAsNode = true;
        v.push_back(geometricCoverRoot);

        BotPreset geometricCoverStates = geometricCoverRealized;
        geometricCoverStates.name = "goal-sel-run-geo-cover-states" + t;
        geometricCoverStates.description = "covering realized beam; every kept movement state brings its best decision" + per;
        geometricCoverStates.options.search.selectiveStateDiversity = true;
        v.push_back(geometricCoverStates);

        BotPreset geometricCoverRootStates = geometricCoverRoot;
        geometricCoverRootStates.name = "goal-sel-run-geo-cover-root-states" + t;
        geometricCoverRootStates.description = "root as a node with state diversity of decisions" + per;
        geometricCoverRootStates.options.search.selectiveStateDiversity = true;
        v.push_back(geometricCoverRootStates);

        BotPreset geometricCoverCategories = geometricCoverRealized;
        geometricCoverCategories.name = "goal-sel-run-geo-cover-cat" + t;
        geometricCoverCategories.description = "covering realized beam; ball-team decisions by channel category" + per;
        geometricCoverCategories.options.search.selectivePartnerCategories = true;
        v.push_back(geometricCoverCategories);

        BotPreset geometricCoverCategoriesStates = geometricCoverCategories;
        geometricCoverCategoriesStates.name = "goal-sel-run-geo-cover-cat-states" + t;
        geometricCoverCategoriesStates.description = "channel categories plus state diversity of decisions" + per;
        geometricCoverCategoriesStates.options.search.selectiveStateDiversity = true;
        v.push_back(geometricCoverCategoriesStates);

        BotPreset geometricCoverCategoryRoot = geometricCoverRealized;
        geometricCoverCategoryRoot.name = "goal-sel-run-geo-cover-catroot" + t;
        geometricCoverCategoryRoot.description = "covering realized beam; ball team's root by refined channel categories" + per;
        geometricCoverCategoryRoot.options.search.selectiveCategoryRoot = true;
        v.push_back(geometricCoverCategoryRoot);

        BotPreset geometricCoverTimed = geometricCoverRealized;
        geometricCoverTimed.name = "goal-sel-run-geo-cover-timed" + t;
        geometricCoverTimed.description = "covering realized beam; defenders block runs where they arrive in time" + per;
        geometricCoverTimed.options.search.selectiveTimedRoute = true;
        v.push_back(geometricCoverTimed);

        BotPreset geometricCoverSigned = geometricCoverRealized;
        geometricCoverSigned.name = "goal-sel-run-geo-cover-signed" + t;
        geometricCoverSigned.description = "covering realized beam; no zero floor without an available shot" + per;
        geometricCoverSigned.options.search.selectiveNoShotFloor = true;
        v.push_back(geometricCoverSigned);

        BotPreset geometricCoverTimedSigned = geometricCoverTimed;
        geometricCoverTimedSigned.name = "goal-sel-run-geo-cover-timed-signed" + t;
        geometricCoverTimedSigned.description = "timed run blocking and no zero floor" + per;
        geometricCoverTimedSigned.options.search.selectiveNoShotFloor = true;
        v.push_back(geometricCoverTimedSigned);

        BotPreset geometricCoverTimedBall = geometricCoverTimed;
        geometricCoverTimedBall.name = "goal-sel-run-geo-cover-timed-ball" + t;
        geometricCoverTimedBall.description = "timed run blocking; a free ball goes to the first player in time" + per;
        geometricCoverTimedBall.options.search.selectiveTimedFreeBall = true;
        v.push_back(geometricCoverTimedBall);

        BotPreset geometricCoverTimedTerminal = geometricCoverTimed;
        geometricCoverTimedTerminal.name = "goal-sel-run-geo-cover-timed-terminal" + t;
        geometricCoverTimedTerminal.description =
            "timed run blocking; first pickup or restart for a free ball with a terminal path" + per;
        geometricCoverTimedTerminal.options.search.selectiveTerminalFreeBall = true;
        v.push_back(geometricCoverTimedTerminal);

        // 1 v 0 leaves (bot_design.md, "Выход 1 в 0"): a proven lower bound of
        // the holder's goal probability as a floor of the static.
        BotPreset geometricCoverTimedBreakaway = geometricCoverTimed;
        geometricCoverTimedBreakaway.name = "goal-sel-run-geo-cover-timed-bk" + t;
        geometricCoverTimedBreakaway.description =
            "timed run blocking; a 1 v 0 leaf is worth at least its proven shot" + per;
        geometricCoverTimedBreakaway.options.search.selectiveBreakawayBound = true;
        v.push_back(geometricCoverTimedBreakaway);

        // Leaves in one scale: a free ball by who reaches it first (the state
        // after a certain take valued as possession), and every possession at
        // least its proven shot (bot_design.md 4.4, "Выход 1 в 0").
        BotPreset geometricCoverTimedRaceBound = geometricCoverTimed;
        geometricCoverTimedRaceBound.name = "goal-sel-run-geo-cover-timed-rb" + t;
        geometricCoverTimedRaceBound.description =
            "timed run blocking; free-ball leaves by the race, every possession at least its proven shot" + per;
        geometricCoverTimedRaceBound.options.search.selectiveBallRace = true;
        geometricCoverTimedRaceBound.options.search.selectiveBreakawayBound = true;
        geometricCoverTimedRaceBound.options.search.selectiveBreakawayAnyEpisode = true;
        v.push_back(geometricCoverTimedRaceBound);

        // The same and a 2 v 1 by its own lower bound: the defender can not
        // cover both attackers (bot_design.md 4.4, "2 в 1").
        BotPreset geometricCoverTimedRaceBound2 = geometricCoverTimedRaceBound;
        geometricCoverTimedRaceBound2.name = "goal-sel-run-geo-cover-timed-rb2" + t;
        geometricCoverTimedRaceBound2.description =
            "timed run blocking; free-ball leaves by the race, proven shot of every possession and of a 2 v 1" + per;
        geometricCoverTimedRaceBound2.options.search.selectiveTwoVsOneBound = true;
        v.push_back(geometricCoverTimedRaceBound2);

        // -rb- whose beam ranks the decisions that free the ball by the race
        // as well, as their leaves are valued (bot_design.md 4.4).
        BotPreset geometricCoverTimedRaceRank = geometricCoverTimedRaceBound;
        geometricCoverTimedRaceRank.name = "goal-sel-run-geo-cover-timed-rbr" + t;
        geometricCoverTimedRaceRank.description =
            "-rb- with the beam's ranking of free-ball decisions by the race" + per;
        geometricCoverTimedRaceRank.options.search.selectiveRaceRanking = true;
        v.push_back(geometricCoverTimedRaceRank);

        // -rbr- with the episode extension of -ext2w- (bot_design.md 3.5): a
        // leaf a round before the attack's shot and inside the alpha-beta
        // window is searched a round deeper with a narrow beam.
        BotPreset geometricCoverTimedRaceRankExt = geometricCoverTimedRaceRank;
        geometricCoverTimedRaceRankExt.name = "goal-sel-run-geo-cover-timed-rbr-ext2w" + t;
        geometricCoverTimedRaceRankExt.description =
            "-rbr- with the narrow episode extension inside the alpha-beta window (-ext2w-)" + per;
        geometricCoverTimedRaceRankExt.options.search.selectiveEpisodeExtension = true;
        geometricCoverTimedRaceRankExt.options.search.selectiveExtensionWidth = 2;
        geometricCoverTimedRaceRankExt.options.search.selectiveExtensionInWindow = true;
        v.push_back(geometricCoverTimedRaceRankExt);

        // -rbr-ext2w- whose attack episode knows who has a free ball: its
        // carrier is the player the race solver proves takes it first
        // (bot_design.md 4.4, 02.11.2026), so the extension, the bounds and
        // the outsiders follow the ball's owner after a pass into space.
        BotPreset geometricEpisodeRace = geometricCoverTimedRaceRankExt;
        geometricEpisodeRace.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-ebr" + t;
        geometricEpisodeRace.description =
            "-rbr-ext2w- with the attack episode of a free ball by the race solver" + per;
        geometricEpisodeRace.options.search.selectiveEpisodeBallRace = true;
        v.push_back(geometricEpisodeRace);

        // -rbr-ext2w- whose partial turns are ranked with the race solver for a
        // free ball, as the leaves value it (02.11.2026, game_default_live 34.B).
        BotPreset geometricRaceMovements = geometricCoverTimedRaceRankExt;
        geometricRaceMovements.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-rm" + t;
        geometricRaceMovements.description = "-rbr-ext2w- with partial turns ranked by the race solver" + per;
        geometricRaceMovements.options.search.selectiveRaceMovements = true;
        v.push_back(geometricRaceMovements);

        // -rbr-ext2w- with pickup diversity: taking the free ball always stays
        // among the candidates (02.11.2026, game_default_live 34.B).
        BotPreset geometricPickupDiversity = geometricCoverTimedRaceRankExt;
        geometricPickupDiversity.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pk" + t;
        geometricPickupDiversity.description = "-rbr-ext2w- with pickup diversity in the root and the movement beam" + per;
        geometricPickupDiversity.options.search.selectivePickupDiversity = 2;
        v.push_back(geometricPickupDiversity);
        BotPreset geometricPickupRoot = geometricCoverTimedRaceRankExt;
        geometricPickupRoot.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr" + t;
        geometricPickupRoot.description = "-rbr-ext2w- with pickup diversity in the root's first actions only" + per;
        geometricPickupRoot.options.search.selectivePickupDiversity = 1;
        v.push_back(geometricPickupRoot);

        BotPreset geometricReachPass = geometricPickupRoot;
        geometricReachPass.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-wave" + t;
        geometricReachPass.description = "-pkr- with an own-front space pass as a separate root kind" + per;
        geometricReachPass.options.search.selectiveReachPassRootKind = true;
        v.push_back(geometricReachPass);

        // Experimental (02.11.2026): -pkr- whose turn in a 1 v 0 or 2 v 1 attack
        // episode is chosen by the episode table with the moving front and passes
        // ahead (bot.h, episodeTableMoves); everything else as -pkr-.
        BotPreset geometricEpisodeMoves = geometricPickupRoot;
        geometricEpisodeMoves.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-etm" + t;
        geometricEpisodeMoves.description = "-pkr- with 1 v 0 / 2 v 1 turns chosen by the episode table" + per;
        geometricEpisodeMoves.options.episodeTableMoves = true;
        v.push_back(geometricEpisodeMoves);

        // Experimental (06.11.2026): -pkr- whose inner nodes also try the best
        // four ball actions of the unmoved state beyond the beam width, as the
        // root does with its first actions (server: the inner node's loss in
        // positions with the ball at the side to move 0.023 -> 0.004 at depth 3).
        BotPreset geometricBallFirst = geometricPickupRoot;
        geometricBallFirst.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-bf" + t;
        geometricBallFirst.description = "-pkr- with four ball-first decisions beyond the beam in inner nodes" + per;
        geometricBallFirst.options.search.selectiveBallFirstExtra = 4;
        v.push_back(geometricBallFirst);

        // Experimental (07.11.2026, the user's rule): -pkr- searching a pass only
        // along "ball through" and a tackle not along "knocked away"; a caught or
        // deflected pass is a loss valued by the static after the end of the turn.
        // Experimental (09.11.2026, the user's rule): -pkr- whose defending side
        // keeps every tackle and every interception it can make this turn in its
        // beams (root first actions, movement states, decisions), beyond the width.
        BotPreset geometricDefenceForced = geometricPickupRoot;
        geometricDefenceForced.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-df" + t;
        geometricDefenceForced.description = "-pkr- with every tackle and interception of the defending side searched" + per;
        geometricDefenceForced.options.search.selectiveDefenceForced = true;
        v.push_back(geometricDefenceForced);

        // Experimental (09.11.2026, the user's rule): -pkr- whose root decision
        // comes only from depths ending after the opponent's reply (2, 4, ...).
        BotPreset geometricRoundDepth = geometricPickupRoot;
        geometricRoundDepth.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-rd" + t;
        geometricRoundDepth.description = "-pkr- deciding at the root only from depths ending after the opponent's reply" + per;
        geometricRoundDepth.options.search.selectiveRoundDepth = true;
        v.push_back(geometricRoundDepth);

        // Experimental (09.11.2026, the user): -pkr- whose root decides by the mean
        // of a decision's values at the last two depths (odd and even horizon).
        BotPreset geometricRootAverage = geometricPickupRoot;
        geometricRootAverage.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-ra" + t;
        geometricRootAverage.description = "-pkr- deciding at the root by the mean of the last two depths" + per;
        geometricRootAverage.options.search.selectiveRootDepthAverage = true;
        v.push_back(geometricRootAverage);

        BotPreset geometricMainOutcome = geometricPickupRoot;
        geometricMainOutcome.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-mo" + t;
        geometricMainOutcome.description = "-pkr- with passes and tackles searched along the main outcome only" + per;
        geometricMainOutcome.options.search.selectiveMainOutcome = true;
        v.push_back(geometricMainOutcome);

        // Experimental (07.11.2026, the user's rule): -pkr- that does not
        // generate a pass to an empty cell whose untouched ball the reach map
        // gives to the opponent first (root and inner nodes).
        BotPreset geometricReachPrune = geometricPickupRoot;
        geometricReachPrune.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-rp" + t;
        geometricReachPrune.description = "-pkr- without passes the reach map gives to the opponent" + per;
        geometricReachPrune.options.search.selectiveReachPassPrune = true;
        v.push_back(geometricReachPrune);

        // Experimental (07.11.2026): -pkr- whose episode extension (two more
        // team turns, width 2, in the window) also takes the leaves where the
        // reach map gives the ball holder, first of all, a cell within two
        // rounds with an open shot above the leaf's value for his team.
        BotPreset geometricReachExtension = geometricPickupRoot;
        geometricReachExtension.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-rx" + t;
        geometricReachExtension.description = "-pkr- with the episode extension also on the reach map's breakaways" + per;
        geometricReachExtension.options.search.selectiveReachExtension = true;
        v.push_back(geometricReachExtension);

        // Experimental (07.11.2026): -pkr-mo- whose lost balls are searched
        // where the reach map shows the catcher a possible breakaway.
        BotPreset geometricMainOutcomeCheck = geometricMainOutcome;
        geometricMainOutcomeCheck.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-mo-lc" + t;
        geometricMainOutcomeCheck.description = "-pkr-mo- with lost balls searched on the reach map's counter breakaways" + per;
        geometricMainOutcomeCheck.options.search.selectiveLossReachCheck = true;
        v.push_back(geometricMainOutcomeCheck);

        // Experimental (07.11.2026, the user's decision, variant 4): -pkr- on the
        // scale "who scores next" with the possession value W = 0.2 (his games: a
        // possession scores ~0.35, P(next goal ours | our ball) ~ 1/(2 - 0.35)):
        // every held ball is worth +-W (2k - 1) on top of the static's chance (k:
        // kept until the holder acts), a shot gives the ball away (p (1 + W) - W),
        // a restart and the end of an episode without a goal are worth +-W.
        BotPreset geometricPossession = geometricPickupRoot;
        geometricPossession.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-pv" + t;
        geometricPossession.description = "-pkr- with the possession value 0.2 on top of the static (scale: who scores next)" + per;
        geometricPossession.options.search.selectivePossessionValue = 0.2;
        v.push_back(geometricPossession);

        // The same with W = 0.1 and W = 0.3 (08.11.2026, the user's decision: the
        // sensitivity to W in the server tournament).
        for (int percent : {10, 30}) {
            BotPreset pv = geometricPossession;
            pv.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-pv" + std::to_string(percent) + t;
            pv.description = "-pkr- with the possession value " + std::string(percent == 10 ? "0.1" : "0.3") +
                             " on top of the static (scale: who scores next)" + per;
            pv.options.search.selectivePossessionValue = percent / 100.0;
            v.push_back(pv);
        }

        // -ebr- with the episode table (bot_design.md, "Таблица эпизода",
        // 02.11.2026): a leaf in a 1 v 0 or 2 v 1 episode is worth at least
        // the minimax value of the episode over its participants.
        BotPreset geometricEpisodeTable = geometricEpisodeRace;
        geometricEpisodeTable.name = "goal-sel-run-geo-cover-timed-rbr-ext2w-ept" + t;
        geometricEpisodeTable.description = "-ebr- with the episode table in 1 v 0 and 2 v 1 leaves" + per;
        geometricEpisodeTable.options.search.selectiveEpisodeTable = true;
        v.push_back(geometricEpisodeTable);

        // Variable depth (bot_design.md 3.5): a leaf whose attack can shoot
        // from its best cell within the next round is searched a round deeper.
        BotPreset geometricCoverTimedExt = geometricCoverTimed;
        geometricCoverTimedExt.name = "goal-sel-run-geo-cover-timed-ext" + t;
        geometricCoverTimedExt.description =
            "timed run blocking; a leaf one round before the attack's shot is searched a round deeper" + per;
        geometricCoverTimedExt.options.search.selectiveEpisodeExtension = true;
        v.push_back(geometricCoverTimedExt);

        for (int width : {2, 3}) {
            BotPreset narrow = geometricCoverTimedExt;
            narrow.name = "goal-sel-run-geo-cover-timed-ext" + std::to_string(width) + t;
            narrow.description = "episode extension with beam width " + std::to_string(width) +
                                 " inside the extension" + per;
            narrow.options.search.selectiveExtensionWidth = width;
            v.push_back(narrow);
        }
        BotPreset windowed = geometricCoverTimedExt;
        windowed.name = "goal-sel-run-geo-cover-timed-ext2w" + t;
        windowed.description = "narrow episode extension only for leaves inside the alpha-beta window" + per;
        windowed.options.search.selectiveExtensionWidth = 2;
        windowed.options.search.selectiveExtensionInWindow = true;
        v.push_back(windowed);

        // Free-ball leaves by who reaches the ball first (bot_design.md 4.4):
        // a certain take or restart becomes the state after it, a contested
        // race at the horizon is searched one team turn further.
        BotPreset geometricCoverTimedRace = geometricCoverTimed;
        geometricCoverTimedRace.name = "goal-sel-run-geo-cover-timed-race" + t;
        geometricCoverTimedRace.description =
            "timed run blocking; free-ball leaves by who reaches the ball first" + per;
        geometricCoverTimedRace.options.search.selectiveBallRace = true;
        v.push_back(geometricCoverTimedRace);

        // A best-first tree over the same candidates and static (7.68), the
        // ball team's candidates by the ball's category (7.69).
        BotPreset tree = run;
        tree.name = "goal-sel-tree-cat" + t;
        tree.description = "best-first tree, the ball team's candidates by the ball's category" + per;
        tree.options.search.selectiveTree = true;
        tree.options.search.selectiveTreeBallCategories = true;
        v.push_back(tree);

        BotPreset treeFb = tree;
        treeFb.name = "goal-sel-tree-fb" + t;
        treeFb.description = "goal-sel-tree-cat-b8 with the free ball by the race from the state as it is" + per;
        treeFb.options.search.selectiveRaceDefence = true;
        v.push_back(treeFb);

        BotPreset treeGeo = geometric;
        treeGeo.name = "goal-sel-tree-geo" + t;
        treeGeo.description = "best-first tree with ball categories and the geometric attack graph" + per;
        treeGeo.options.search.selectiveTree = true;
        treeGeo.options.search.selectiveTreeBallCategories = true;
        v.push_back(treeGeo);

        BotPreset treeGeoPickup = treeGeo;
        treeGeoPickup.name = "goal-sel-tree-geo-pickup" + t;
        treeGeoPickup.description = "geometric tree with certified next-step free-ball pickup" + per;
        treeGeoPickup.options.search.selectiveForcedPickupPreview = true;
        v.push_back(treeGeoPickup);

        BotPreset treeGeoTackle = treeGeo;
        treeGeoTackle.name = "goal-sel-tree-geo-tackle" + t;
        treeGeoTackle.description = "geometric tree with one reserved legal tackle decision" + per;
        treeGeoTackle.options.search.selectiveTreeTackleSlot = true;
        v.push_back(treeGeoTackle);

        BotPreset treeGeoTackleZones = treeGeoTackle;
        treeGeoTackleZones.name = "goal-sel-tree-geo-tackle-zones" + t;
        treeGeoTackleZones.description = "geometric tackle tree with legal next-turn tackle zones" + per;
        treeGeoTackleZones.options.search.selectiveLegalTackleZones = true;
        v.push_back(treeGeoTackleZones);

        BotPreset treeGeoTackleSafe = treeGeoTackleZones;
        treeGeoTackleSafe.name = "goal-sel-tree-geo-tackle-safe" + t;
        treeGeoTackleSafe.description = "legal tackle zones with near-hard threatened possession penalty" + per;
        treeGeoTackleSafe.options.search.selectiveThreatenedRetention = 0.001;
        v.push_back(treeGeoTackleSafe);
    }
    return v;
}

}  // namespace

const std::vector<BotPreset>& BotPresets() {
    static const std::vector<BotPreset> presets = Make();
    return presets;
}

const BotPreset* FindBotPreset(const std::string& name) {
    for (const BotPreset& p : BotPresets())
        if (p.name == name) return &p;
    return nullptr;
}

const std::string& DefaultBotName() {
    static const std::string name = "goal-sel-run-geo-cover-timed-rbr-ext2w-pkr-b8-t120";
    return name;
}

std::unique_ptr<Bot> MakeBot(const BotPreset& preset) { return std::unique_ptr<Bot>(new Bot(preset.options)); }

}  // namespace bot
}  // namespace soccer
