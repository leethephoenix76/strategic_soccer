#include "bot/bot.h"

#include "bot/episode.h"
#include "bot/episode_value.h"
#include "bot/hash.h"

#include <cstdio>

#include <algorithm>

namespace soccer {
namespace bot {

Action LineupAction(const GameState& s) {
    const int team = s.teamToMove;
    const int first = FirstPlayerOf(team);
    // Formation for White (own half y >= 10); mirrored for Black.
    const Cell ballCell{kCenterX, kCenterY};
    const bool ballFree = PlayerAt(s, ballCell) == kNoPlayer;
    Cell cells[kPlayersPerTeam] = {
        ballFree ? ballCell : Cell{kCenterX, kCenterY + 2},
        {kCenterX - 3, kCenterY + 2},
        {kCenterX + 3, kCenterY + 2},
        {kCenterX, kCenterY + 6},
    };
    for (int i = 0; i < kPlayersPerTeam; ++i) {
        Cell c = cells[i];
        if (team == kBlack) c = {kFieldWidth - 1 - c.x, kGridHeight - 1 - c.y};
        const int k = first + i;
        if (s.players[k].moved || s.players[k].pos == c) continue;
        if (CanPlace(s, k, c)) return Action::Place(k, c);
    }
    return Action::EndTurn();
}

Bot::Bot(const BotOptions& options) : options_(options) {
    GoalDfsOptions g = options_.search;
    g.timeLimit = options_.timeLimit;
    g.threads = options_.threads;
    search_.reset(new GoalDfsSearch(g));
}

// The team turn's time budget starts with its first decision.
void Bot::StartBudget(const GameState& s) {
    if (budgetTurnId_ != s.turnId) {
        budgetTurnId_ = s.turnId;
        budgetDeadline_ = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<long long>(options_.timeLimit * 1000));
    }
}

// One search within the team turn's budget, shared by its remaining decisions.
Action Bot::Decide(const GameState& s) {
    const auto now = std::chrono::steady_clock::now();
    StartBudget(s);
    int remainingActions = 1;  // EndTurn
    for (int k = FirstPlayerOf(s.teamToMove); k < FirstPlayerOf(s.teamToMove) + kPlayersPerTeam; ++k)
        if (!s.players[k].moved) ++remainingActions;
    if (s.phase == Phase::Play && !s.ballPlayed && !s.ball.IsFree() && TeamOf(s.ball.holder) == s.teamToMove)
        ++remainingActions;
    const double remaining = std::max(0.0, std::chrono::duration<double>(budgetDeadline_ - now).count());
    double seconds = remaining / remainingActions;
    if (Selective() && s.phase == Phase::Play) {
        // The selective search returns a team-turn plan. Spend the whole budget
        // when no player can reach the ball before EndTurn; otherwise reserve
        // one action's share for replanning after a random outcome.
        bool mayNeedReplan = !s.ball.IsFree() && TeamOf(s.ball.holder) == s.teamToMove;
        for (int k = FirstPlayerOf(s.teamToMove); k < FirstPlayerOf(s.teamToMove) + kPlayersPerTeam; ++k)
            if (!s.players[k].moved && ChebyshevDistance(s.players[k].pos, s.ball.pos) <= 3) mayNeedReplan = true;
        seconds = mayNeedReplan ? remaining * (1.0 - 1.0 / std::max(2, remainingActions)) : remaining;
    }
    last_ = search_->Search(s, seconds);
    note_ = last_.note;
    return last_.action;
}

// The participants' moves that take `s` to `target` (each mover once), in an
// order that is legal; empty with ok = false if none is.
static std::vector<Action> MovesTo(const GameState& s, const GameState& target, bool* ok) {
    std::vector<int> movers;
    for (int k = FirstPlayerOf(s.teamToMove); k < FirstPlayerOf(s.teamToMove) + kPlayersPerTeam; ++k)
        if (s.players[k].pos != target.players[k].pos) movers.push_back(k);
    std::sort(movers.begin(), movers.end());
    do {
        GameState t = s;
        std::vector<Action> moves;
        bool legal = true;
        for (int k : movers) {
            const Action a = Action::Move(k, target.players[k].pos);
            if (Validate(t, a) != ActionError::None) { legal = false; break; }
            ApplyUnchecked(t, a, Outcome::Certain());
            moves.push_back(a);
        }
        if (legal && t.ball.holder == target.ball.holder) { *ok = true; return moves; }
    } while (std::next_permutation(movers.begin(), movers.end()));
    *ok = false;
    return {};
}

bool Bot::EpisodeTablePlan(const GameState& s, double seconds, std::vector<Action>* plan) {
    const int us = s.teamToMove;
    if (s.phase != Phase::Play || s.ball.IsFree() || TeamOf(s.ball.holder) != us || s.ballPlayed || s.committed)
        return false;
    for (int k = FirstPlayerOf(us); k < FirstPlayerOf(us) + kPlayersPerTeam; ++k)
        if (s.players[k].moved || s.players[k].attacked) return false;   // only at the start of the turn
    const AttackEpisode e = AnalyzeAttackEpisode(s, false, true);
    if (e.team != us || (e.kind != AttackEpisodeKind::OneVsZero && e.kind != AttackEpisodeKind::TwoVsOne)) return false;
    const auto start = std::chrono::steady_clock::now();
    const unsigned opponents = 0xFu << FirstPlayerOf(Opponent(us));
    EpisodeValueResult best;
    int bestTurns = 0;
    for (int turns = 3; turns <= 9; turns += 2) {
        EpisodeValueOptions eo;
        eo.turns = turns;
        eo.nodeLimit = 10000000;
        eo.frontOpponents = opponents & ~e.defenders;
        eo.leadPasses = true;
        const EpisodeValueResult r = EpisodeValue(s, e.attackers, e.defenders, eo);
        if (!r.complete || !r.rootFound) break;
        best = r;
        bestTurns = turns;
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > seconds) break;
    }
    if (bestTurns == 0 || best.rootValue <= 0) return false;
    // The participants' moves, then the ball action; keeping the ball, the
    // outsiders move by the episode's policy before the turn ends.
    bool ok = false;
    std::vector<Action> out = MovesTo(s, best.rootState, &ok);
    if (!ok) return false;
    GameState t = s;
    for (const Action& a : out) ApplyUnchecked(t, a, Outcome::Certain());
    if (best.rootAction.type == ActionType::EndTurn) {
        for (const Action& a : OutsiderMoves(t, AnalyzeAttackEpisode(s, false, true))) {
            if (Validate(t, a) != ActionError::None) continue;
            GameState u = t;
            ApplyUnchecked(u, a, Outcome::Certain());
            if (u.ball.holder != t.ball.holder) continue;
            t = u;
            out.push_back(a);
        }
    }
    if (Validate(t, best.rootAction) != ActionError::None) return false;
    out.push_back(best.rootAction);
    *plan = out;
    char buffer[160];
    std::snprintf(buffer, sizeof buffer, "episode table: %s, value %.4f over %d team turns, %.1f s",
                  AttackEpisodeName(e.kind), best.rootValue, bestTurns,
                  std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    note_ = buffer;
    return true;
}

Action Bot::NextAction(const GameState& s) {
    if (s.phase == Phase::Lineup) {
        // The selective estimate has no placement value: the shared formation.
        if (Selective()) return LineupAction(s);
        const Action a = Decide(s);
        return Validate(s, a) == ActionError::None ? a : LineupAction(s);
    }
    if (s.phase != Phase::Play) return Action::EndTurn();

    if (next_ >= queue_.size() || PositionHash(s) != expected_) {
        std::vector<Action> episodePlan;
        StartBudget(s);
        if (options_.episodeTableMoves && EpisodeTablePlan(s, options_.timeLimit * 0.25, &episodePlan)) {
            queue_ = episodePlan;
        } else {
            const Action selected = Decide(s);
            queue_ = Selective() && !last_.turnPlan.empty() ? last_.turnPlan : std::vector<Action>{selected};
        }
        next_ = 0;
        expected_ = PositionHash(s);
    }
    Action a = queue_[next_++];
    if (Validate(s, a) != ActionError::None) {   // should not happen: fall back to ending the turn
        queue_.clear();
        return Action::EndTurn();
    }
    std::vector<Outcome> outs = OutcomesUnchecked(s, a);
    if (outs.size() == 1 && next_ < queue_.size()) {
        GameState t = s;
        ApplyUnchecked(t, a, outs.front());
        expected_ = PositionHash(t);
    } else {
        queue_.clear();   // a random event (or the end of the turn): replan next time
    }
    return a;
}

}  // namespace bot
}  // namespace soccer
