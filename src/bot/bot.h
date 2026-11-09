// A bot playing one team: chooses the next action of the team to move with the
// goal search (goal_dfs_search.h). The search returns a plan of the team turn up
// to its first random action; the bot plays it out and searches again after a
// random event (or when the position is not the expected one).
#pragma once

#include "bot/goal_dfs_search.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace soccer {
namespace bot {

struct BotOptions {
    double timeLimit = 120;   // seconds per team turn
    int threads = 0;          // of the search; 0: all cores
    GoalDfsOptions search;
    // Experimental (02.11.2026, -etm-): at the start of a turn with the ball in a
    // 1 v 0 or 2 v 1 attack episode, the participants' turn is chosen by the
    // episode table (episode_value.h: the other opponents hold the moving front,
    // passes ahead); the other players of the team move by the episode's outsider
    // policy. Otherwise, or when the table is not complete in its share of the
    // time, the search decides.
    bool episodeTableMoves = false;
};

class Bot {
public:
    explicit Bot(const BotOptions& options = BotOptions());

    Action NextAction(const GameState& s);

    // What the bot intends (the search's note), for logs.
    const std::string& Note() const { return note_; }
    const GoalSearchResult& LastSearch() const { return last_; }
    GoalDfsStats LastSearchStats() const { return search_->Stats(); }

private:
    bool Selective() const { return options_.search.selectiveWidth > 0; }
    Action Decide(const GameState& s);
    void StartBudget(const GameState& s);
    bool EpisodeTablePlan(const GameState& s, double seconds, std::vector<Action>* plan);

    BotOptions options_;
    std::unique_ptr<GoalDfsSearch> search_;
    GoalSearchResult last_;
    int budgetTurnId_ = -1;
    std::chrono::steady_clock::time_point budgetDeadline_;
    std::vector<Action> queue_;
    std::size_t next_ = 0;
    uint64_t expected_ = 0;
    std::string note_;
};

// Line-up: the next placement of a simple formation (a diamond; a player on the
// ball if it is still free), or End Turn when done.
Action LineupAction(const GameState& s);

}  // namespace bot
}  // namespace soccer
