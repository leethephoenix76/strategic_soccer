// Player movement: inertia and blocking by other players; line-up placement.
#include "internal.h"

#include <cmath>

namespace soccer {

namespace {

// Is the move start -> target blocked by player k standing at `other`?
// Sets the "box" flags used to forbid slipping diagonally between two players.
struct BoxFlags {
    bool nearTargetX = false, nearTargetY = false;   // the two cells beside the target end
    bool nearStartX = false, nearStartY = false;     // the two cells beside the start
};

bool BlocksMove(Cell start, Cell target, Cell other, BoxFlags& box) {
    if (other == target) return true;   // the target cell is occupied

    if (target.x == start.x) {
        // vertical move: blocked by a player in the same column between start and target
        if (other.x == start.x &&
            ((other.y < target.y && start.y < other.y) || (other.y > target.y && start.y > other.y)))
            return true;
        return false;
    }

    // Distance from the centre of the other player's cell to the line through the
    // centres of the start and target cells.
    double a = double(target.y - start.y) / (target.x - start.x);
    double b = ((target.x + 0.5) * (start.y + 0.5) - (start.x + 0.5) * (target.y + 0.5)) / (target.x - start.x);
    double cross = std::fabs(a * (other.x + 0.5) - (other.y + 0.5) + b) / std::sqrt(a * a + 1);

    // The line passes through the other player's cell (within the bounding box of the move).
    bool between = (target.x - other.x) * (other.x - start.x) >= 0 &&
                   (target.y - other.y) * (other.y - start.y) >= 0;
    if (cross < 0.707 && between) return true;

    // The line just touches the corner of the other player's cell: remember it,
    // two such players on both sides of the line close the corridor.
    if (cross > 0.706 && cross < 0.708) {
        box.nearTargetX = box.nearTargetX || (target.x == other.x && other.y == (target.y + start.y) / 2);
        box.nearTargetY = box.nearTargetY || (target.y == other.y && other.x == (target.x + start.x) / 2);
        int incY = target.y > start.y ? 1 : -1;
        box.nearStartX = box.nearStartX || (other.x == start.x && other.y == start.y + incY);
        int incX = target.x > start.x ? 1 : -1;
        box.nearStartY = box.nearStartY || (other.y == start.y && other.x == start.x + incX);
    }
    return false;
}

}  // namespace

bool MovePathBlocked(Cell start, Cell target, const Cell* blockers, int blockerCount) {
    BoxFlags box;
    for (int i = 0; i < blockerCount; ++i)
        if (BlocksMove(start, target, blockers[i], box)) return true;
    return (box.nearTargetX && box.nearTargetY) || (box.nearStartX && box.nearStartY);
}

std::vector<Cell> MoveDestinations(const GameState& s, int player) {
    std::vector<Cell> result;
    if (player < 0 || player >= kNumPlayers) return result;
    const PlayerState& me = s.players[player];
    Cell start = me.turnStart;
    Cell v = me.Velocity();
    // Inertia: the 3x3 square of destinations is centred one cell ahead in the
    // direction of the previous movement.
    Cell center{start.x + Sign(v.x), start.y + Sign(v.y)};
    Cell blockers[kNumPlayers - 1];
    int blockerCount = 0;
    for (int k = 0; k < kNumPlayers; ++k) {
        if (k == player) continue;
        const Cell other = s.players[k].pos;
        if (Abs(other.x - center.x) <= 1 && Abs(other.y - center.y) <= 1)
            blockers[blockerCount++] = other;
    }

    for (int i = -1; i <= 1; ++i) {
        for (int j = -1; j <= 1; ++j) {
            Cell target{center.x + i, center.y + j};
            if (!InField(target)) continue;

            // After an attack in this turn the player can not move against its direction.
            if (me.attacked &&
                (target.x - start.x) * me.attackDir.x + (target.y - start.y) * me.attackDir.y < 0)
                continue;
            if (!MovePathBlocked(start, target, blockers, blockerCount)) result.push_back(target);
        }
    }
    return result;
}

bool CanPlace(const GameState& s, int player, Cell target, ActionError* why) {
    ActionError err = ActionError::None;
    if (s.phase != Phase::Lineup) err = ActionError::WrongPhase;
    else if (player < 0 || player >= kNumPlayers || TeamOf(player) != s.teamToMove) err = ActionError::NotYourPlayer;
    else if (s.players[player].moved) err = ActionError::AlreadyMoved;
    else if (!InField(target)) err = ActionError::OutsideField;
    else if ((TeamOf(player) == kWhite && target.y < kCenterY) || (TeamOf(player) == kBlack && target.y > kCenterY))
        err = ActionError::OppositeHalf;
    else {
        int k = PlayerAt(s, target);
        if (k != kNoPlayer && k != player) err = ActionError::CellOccupied;
    }
    if (why) *why = err;
    return err == ActionError::None;
}

}  // namespace soccer
