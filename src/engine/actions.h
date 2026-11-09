// Actions a team can take. A turn is a sequence of actions ending with EndTurn.
#pragma once

#include "types.h"

namespace soccer {

enum class ActionType {
    Place,    // line-up: put own player on a free cell of the own half (once per player)
    Move,     // play: move own player to one of his destinations (once per player)
    Attack,   // play: tackle the ball holder or poke a sealed free ball (once per player)
    Pass,     // play: the own ball holder passes to a cell (once per team per turn, shared with Shot)
    Shot,     // play: the own ball holder shoots at the goal
    EndTurn,  // finish the turn (line-up or play)
};

struct Action {
    ActionType type = ActionType::EndTurn;
    int player = kNoPlayer;   // Place, Move, Attack
    Cell target;              // Place, Move, Pass

    static Action Place(int player, Cell target) { return {ActionType::Place, player, target}; }
    static Action Move(int player, Cell target) { return {ActionType::Move, player, target}; }
    static Action Attack(int player) { return {ActionType::Attack, player, {}}; }
    static Action Pass(Cell target) { return {ActionType::Pass, kNoPlayer, target}; }
    static Action Shot() { return {ActionType::Shot, kNoPlayer, {}}; }
    static Action EndTurn() { return {ActionType::EndTurn, kNoPlayer, {}}; }
};

// Why an action is not allowed.
enum class ActionError {
    None,
    WrongPhase,         // e.g. a pass during the line-up, anything after the match
    NotYourPlayer,      // the player belongs to the other team
    AlreadyMoved,       // the player has already moved (placed) in this turn
    OutsideField,
    OppositeHalf,       // line-up: the cell is in the opponent's half
    CellOccupied,
    NoMove,             // the target is the player's current cell
    Unreachable,        // not among the player's destinations
    CannotAttack,
    NoBall,             // the team does not have the ball
    BallAlreadyPlayed,  // the team has already passed or shot in this turn
    BadPassTarget,      // outside the field, too far or the passer's own cell
    BadOutcome,         // ApplyOutcome: not a possible outcome of the action
};

}  // namespace soccer
