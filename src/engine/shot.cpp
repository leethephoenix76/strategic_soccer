// Shots at the goal. The keeper is not a piece on the field: the outcome is
// purely probabilistic, depending on the shooter's position and blocking players.
#include "internal.h"

#include <cmath>

namespace soccer {

double OpenShotProbability(Cell from, int team) {
    // Distance and angle.
    const int goalRow = AttackedGoalRow(team);
    double d2 = double((from.x - kGoalCenterX) * (from.x - kGoalCenterX) + (from.y - goalRow) * (from.y - goalRow));
    return std::exp(-0.033 * d2) * (1 - 0.15 * Abs(from.x - kGoalCenterX));
}

double BlockFactor(double distance) { return 1 - 0.4 / (1 + distance); }

void ParryCells(int shootingTeam, Cell out[kParryCells]) {
    const int firstRow = shootingTeam == kBlack ? kLastRow - 3 : kFirstRow + 1;
    int n = 0;
    for (int x = 3; x <= 7; ++x)
        for (int y = firstRow; y < firstRow + 3; ++y) out[n++] = {x, y};
}

double ShotBlockFactor(Cell shooter, int team, Cell other) {
    const int goalRow = AttackedGoalRow(team);
    // Shot line: from the centre of the shooter's cell to the centre of the goal.
    const double sx = shooter.x + 0.5, sy = shooter.y + 0.5;
    const double lx = kGoalCenterX + 0.5 - sx, ly = goalRow + 0.5 - sy;
    const double len = std::sqrt(lx * lx + ly * ly);   // > 0: the shooter is inside the field
    // Only a player in the blocking zone (columns between the shooter and the
    // goal posts, rows from the shooter towards the goal) reduces the probability.
    const int left = shooter.x < kGoalLeft ? shooter.x : kGoalLeft;
    const int right = shooter.x > kGoalRight ? shooter.x : kGoalRight;
    if (other.x < left || other.x > right) return 1;
    const bool towardsGoal = team == kWhite ? other.y <= shooter.y : other.y >= shooter.y;
    if (!towardsGoal) return 1;
    const double dist = std::fabs(lx * (other.y + 0.5 - sy) - ly * (other.x + 0.5 - sx)) / len;
    return BlockFactor(dist);
}

double ShotProbability(const GameState& s, int shooter) {
    const int team = TeamOf(shooter);
    const Cell p = s.players[shooter].pos;
    double probability = OpenShotProbability(p, team);
    // Every other player in the blocking zone reduces the probability.
    for (int k = 0; k < kNumPlayers; ++k) {
        if (k == shooter) continue;
        probability *= ShotBlockFactor(p, team, s.players[k].pos);
    }
    return probability;
}

namespace detail {

void ShotOutcomes(const GameState& s, int shooter, std::vector<Outcome>& out) {
    const int team = TeamOf(shooter);
    const double p = ShotProbability(s, shooter);
    Outcome goal;
    goal.kind = OutcomeKind::Goal;
    goal.probability = p;
    if (p > 0) out.push_back(goal);
    Outcome caught;
    caught.kind = OutcomeKind::Caught;
    caught.probability = (1 - p) * (1 - p);
    if (caught.probability > 0) out.push_back(caught);
    // Parried, probability p(1 - p), into one of the cells in front of the goal.
    const double parried = p * (1 - p);
    if (parried <= 0) return;
    Cell cells[kParryCells];
    ParryCells(team, cells);
    for (Cell c : cells) {
        Outcome o;
        o.kind = OutcomeKind::Parried;
        o.cell = c;
        o.probability = parried / kParryCells;
        out.push_back(o);
    }
}

void ResolveShot(GameState& s, int shooter, const Outcome& o, EventLog* events) {
    const int team = TeamOf(shooter);
    Emit(events, EventType::ShotTaken, team, shooter);
    if (o.kind == OutcomeKind::Goal) {
        Emit(events, EventType::Goal, team);
        return;
    }
    if (o.kind == OutcomeKind::Caught) {
        Emit(events, EventType::KeeperCaught, Opponent(team));
        return;
    }
    BallState& b = s.ball;
    b.pos = o.cell;
    b.vel = {};
    b.holder = kNoPlayer;
    Touch(s, Opponent(team), false);   // touched by the keeper
    UpdatePossession(s);
    Emit(events, EventType::KeeperParried, Opponent(team));
}

}  // namespace detail
}  // namespace soccer
