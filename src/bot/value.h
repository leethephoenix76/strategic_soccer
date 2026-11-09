// The possession table T(c) (docs/bot_design.md, "Статика"): the expected goal
// difference for the side with the ball in cell c, to move. It values the
// possession beyond the search's episode (selectiveContinuation).
//
// T was computed once (18.10.2026) by the old attack planner over 639 positions
// with the ball taken from games - a fixed point of the planner's plans with
// their ends valued by T itself - and compiled into possession_table_data.cpp;
// the planner and the generator were removed in the cleanup of 26.10.2026
// (docs/archive/bot_design_full.md, 7.19). The table knows only the ball's cell,
// not the players: a known weakness (open questions in docs/bot_design.md).
//
// The full table: mover T(c); other -T of the opponent's cell; a free ball by
// the race d = T(other) - T(mover) (fewest turns to the ball by reach with
// inertia, the mover moves first): as if the faster side had it.
#pragma once

#include "engine/state.h"

namespace soccer {
namespace bot {

enum class BallOwner { Mover = 0, Other = 1, Free = 2 };

constexpr int kMaxRace = 3;                          // the race d is clipped to -3..3
constexpr int kKinds = 2 + 2 * kMaxRace + 1;         // mover, other, free with d = -3..3
constexpr int kTableRows = kKinds + 1;               // + a free ball with the race unknown (averaged)

class ValueTable {
public:
    // T compiled into the program (possession_table_data.cpp), all rows.
    static const ValueTable& PossessionDefault();
    // From the mover row: other -mover of the opponent's cell (the field turned);
    // a free ball by the race: the mover's at d >= 0, the other's below; the race
    // unknown: their mean.
    static ValueTable FromMover(const ValueTable& t);

    static int Kind(BallOwner owner, int race = 0) {
        return owner == BallOwner::Free ? 2 + kMaxRace + race : static_cast<int>(owner);
    }
    static constexpr int kFreeAny = kKinds;   // the row of a free ball with the race unknown

    // Value for the mover; `cell` in the mover's frame. A free ball without a race: kFreeAny.
    double Get(int kind, Cell cell) const { return v_[kind][cell.x][cell.y]; }
    double Get(BallOwner owner, Cell cell) const { return Get(owner == BallOwner::Free ? kFreeAny : Kind(owner), cell); }
    void Set(int kind, Cell cell, double value) { v_[kind][cell.x][cell.y] = value; }

    // Value for the team `mover` when it is to move with the ball in `cell` (field frame).
    double Of(BallOwner owner, Cell cell, int mover) const { return Get(owner, Frame(cell, mover)); }

    // Value for `team` of a state at the start of a turn in play; a restart (line-up)
    // is played out with the bot's line-up first. 0 when the match is over.
    double OfState(GameState s, int team) const;

    static Cell Frame(Cell c, int mover) {
        return mover == kWhite ? c : Cell{kFieldWidth - 1 - c.x, kGridHeight - 1 - c.y};
    }
    static BallOwner OwnerAt(const GameState& s);   // at a turn start in play
    static int Race(const GameState& s);            // d for a free ball, clipped
    static int KindAt(const GameState& s);          // at a turn start in play

private:
    double v_[kTableRows][kFieldWidth][kGridHeight] = {};
};

}  // namespace bot
}  // namespace soccer
