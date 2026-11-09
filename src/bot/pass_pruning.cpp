#include "bot/pass_pruning.h"

#include "engine/rules.h"

#include <algorithm>
#include <vector>

namespace soccer {
namespace bot {

namespace {

Cell ClipToField(Cell cell) {
    cell.x = std::max(0, std::min(kFieldWidth - 1, cell.x));
    cell.y = std::max(kFirstRow, std::min(kLastRow, cell.y));
    return cell;
}

bool MayReachBox(Cell player, int moves, Cell from, Cell to) {
    // A legal move changes each coordinate by at most two. Ignoring inertia and
    // blockers makes this an upper set of all possible player positions.
    const int radius = 2 * moves;
    return player.x + radius >= std::min(from.x, to.x) &&
           player.x - radius <= std::max(from.x, to.x) &&
           player.y + radius >= std::min(from.y, to.y) &&
           player.y - radius <= std::max(from.y, to.y);
}

bool BoxMeetsMouth(Cell from, Cell to) {
    for (int row : {kFirstRow, kLastRow})
        for (int x = kGoalLeft; x <= kGoalRight; ++x)
            if (x >= std::min(from.x, to.x) && x <= std::max(from.x, to.x) &&
                row >= std::min(from.y, to.y) && row <= std::max(from.y, to.y))
                return true;
    return false;
}

}  // namespace

bool ProvenOpponentPickupBeforeBallStep(const GameState& s) {
    if (s.phase != Phase::Play || !s.ball.IsFree() || !InField(s.ball.pos) ||
        s.ball.stepPhase != 1 || !s.ballPlayed || s.keeperRestartTeam >= 0 ||
        s.halfTurns + 1 == kHalfTurnsPerHalf || s.halfTurns + 1 >= kHalfTurnsPerMatch ||
        s.sealHistory[s.teamToMove] != 0)
        return false;

    const int ourTeam = s.teamToMove;
    for (int player = FirstPlayerOf(ourTeam); player < FirstPlayerOf(ourTeam) + kPlayersPerTeam; ++player)
        if (ChebyshevDistance(s.players[player].pos, s.ball.pos) <= 3) return false;

    for (int opponent = FirstPlayerOf(Opponent(ourTeam));
         opponent < FirstPlayerOf(Opponent(ourTeam)) + kPlayersPerTeam; ++opponent) {
        if (s.players[opponent].moved || s.players[opponent].pos == s.ball.pos) continue;
        const std::vector<Cell> destinations = MoveDestinations(s, opponent);
        if (std::find(destinations.begin(), destinations.end(), s.ball.pos) == destinations.end()) continue;
        const PlayerState& defender = s.players[opponent];
        const Cell velocity = defender.Velocity();
        const Cell center{defender.turnStart.x + Sign(velocity.x),
                          defender.turnStart.y + Sign(velocity.y)};
        bool couldBlock = false;
        for (int player = FirstPlayerOf(ourTeam);
             player < FirstPlayerOf(ourTeam) + kPlayersPerTeam; ++player) {
            const PlayerState& ours = s.players[player];
            if (Abs(ours.pos.x - center.x) <= 1 && Abs(ours.pos.y - center.y) <= 1) {
                couldBlock = true;
                break;
            }
            if (ours.moved) continue;
            const Cell v = ours.Velocity();
            const Cell ourCenter{ours.turnStart.x + Sign(v.x),
                                 ours.turnStart.y + Sign(v.y)};
            if (Abs(ourCenter.x - center.x) <= 2 && Abs(ourCenter.y - center.y) <= 2) {
                couldBlock = true;
                break;
            }
        }
        if (!couldBlock) return true;
    }
    return false;
}

bool ProvenOpponentPickupAtNextBallStep(const GameState& s) {
    if (s.phase != Phase::Play || !s.ball.IsFree() || !InField(s.ball.pos) ||
        !s.ballPlayed || s.ball.stepPhase != 1 || !s.ball.lastTouchWasPass ||
        s.ball.lastTouchTeam != s.teamToMove || s.keeperRestartTeam >= 0 ||
        s.sealHistory[Opponent(s.teamToMove)] != 0 ||
        (s.ball.vel.x == 0 && s.ball.vel.y == 0))
        return false;

    const Cell ball = s.ball.pos;
    const Cell landing{ball.x + s.ball.vel.x, ball.y + s.ball.vel.y};
    if (!InField(landing) || BoxMeetsMouth(ball, landing)) return false;

    const int ourTeam = s.teamToMove;
    // An optimistic superset of every own position after this turn. It also
    // excludes free-ball attacks, touches and possible blocks of the defender.
    for (int player = FirstPlayerOf(ourTeam); player < FirstPlayerOf(ourTeam) + kPlayersPerTeam; ++player) {
        const PlayerState& ours = s.players[player];
        const int moves = ours.moved ? 0 : 1;
        if (ChebyshevDistance(ours.pos, ball) <= 2 * moves + 1 ||
            MayReachBox(ours.pos, moves, ball, landing))
            return false;
    }

    const int theirTeam = Opponent(ourTeam);
    for (int defender = FirstPlayerOf(theirTeam);
         defender < FirstPlayerOf(theirTeam) + kPlayersPerTeam; ++defender) {
        const PlayerState& runner = s.players[defender];
        if (runner.moved || runner.pos == landing) continue;
        const std::vector<Cell> destinations = MoveDestinations(s, defender);
        if (std::find(destinations.begin(), destinations.end(), landing) == destinations.end()) continue;

        // MoveDestinations only consults other players inside this 3x3 square.
        // No own remaining move may enter it, so the checked move stays legal
        // regardless of our action order. Other defenders remain stationary.
        const Cell velocity = runner.Velocity();
        const Cell center{runner.turnStart.x + Sign(velocity.x),
                          runner.turnStart.y + Sign(velocity.y)};
        bool couldBlockOrDeflect = false;
        for (int player = FirstPlayerOf(ourTeam);
             player < FirstPlayerOf(ourTeam) + kPlayersPerTeam; ++player) {
            const PlayerState& ours = s.players[player];
            const int moves = ours.moved ? 0 : 1;
            if (ChebyshevDistance(ours.pos, center) <= 1 + 2 * moves) {
                couldBlockOrDeflect = true;
                break;
            }
        }
        if (couldBlockOrDeflect) continue;
        for (int opponent = FirstPlayerOf(theirTeam);
             opponent < FirstPlayerOf(theirTeam) + kPlayersPerTeam; ++opponent) {
            if (opponent != defender &&
                MayReachBox(s.players[opponent].pos, 0, ball, landing)) {
                couldBlockOrDeflect = true;
                break;
            }
        }
        if (!couldBlockOrDeflect) return true;
    }
    return false;
}

bool ProvenNoOwnRecoveryOnPassiveReply(const GameState& s) {
    if (s.phase != Phase::Play || !s.ball.IsFree() || !InField(s.ball.pos) ||
        !s.ballPlayed || s.ball.stepPhase != 1 || !s.ball.lastTouchWasPass ||
        s.ball.lastTouchTeam != s.teamToMove || s.keeperRestartTeam >= 0 ||
        s.sealHistory[Opponent(s.teamToMove)] != 0)
        return false;

    const int team = s.teamToMove;
    Cell ball = s.ball.pos;
    Cell velocity = s.ball.vel;
    for (int step = 0; step < kMaxPassDistance; ++step) {
        // A sealing penalty against the passive opponent could restart play
        // for us before the out; the current attack might then continue.
        int nearOpponents = 0;
        for (int player = FirstPlayerOf(Opponent(team));
             player < FirstPlayerOf(Opponent(team)) + kPlayersPerTeam; ++player)
            if (ChebyshevDistance(s.players[player].pos, ball) <= 3) ++nearOpponents;
        if (nearOpponents >= 3) return false;
        // Before the next ball step, our side can finish the current turn and
        // then gets one more turn after each preceding ball step.
        for (int player = FirstPlayerOf(team); player < FirstPlayerOf(team) + kPlayersPerTeam; ++player) {
            const int moves = step + (s.players[player].moved ? 0 : 1);
            if (ChebyshevDistance(s.players[player].pos, ball) <= 2 * moves + 1)
                return false;  // pickup or a possible adjacent free-ball attack
        }
        if (velocity.x == 0 && velocity.y == 0) return false;

        const Cell raw{ball.x + velocity.x, ball.y + velocity.y};
        const Cell next = ClipToField(raw);
        if (BoxMeetsMouth(ball, next)) return false;
        for (int player = FirstPlayerOf(team); player < FirstPlayerOf(team) + kPlayersPerTeam; ++player) {
            const int moves = step + (s.players[player].moved ? 0 : 1);
            if (MayReachBox(s.players[player].pos, moves, ball, next))
                return false;  // a possible pickup or stochastic touch
        }
        bool opponentAtEnd = false;
        for (int player = FirstPlayerOf(Opponent(team));
             player < FirstPlayerOf(Opponent(team)) + kPlayersPerTeam; ++player) {
            const Cell opponent = s.players[player].pos;  // passive reply
            if (opponent == next) { opponentAtEnd = true; continue; }
            if (MayReachBox(opponent, 0, ball, next))
                return false;  // a deflection could change the ball or last touch
        }
        if (InField(raw) || opponentAtEnd) {
            int nearAtEnd = 0;
            for (int player = FirstPlayerOf(Opponent(team));
                 player < FirstPlayerOf(Opponent(team)) + kPlayersPerTeam; ++player)
                if (ChebyshevDistance(s.players[player].pos, next) <= 3) ++nearAtEnd;
            if (nearAtEnd >= 3) return false;
        }
        if (opponentAtEnd) return true;
        if (!InField(raw)) return true;  // out after our deliberate pass

        ball = next;
        if (velocity.x) velocity.x += s.ball.dec.x;
        if (velocity.y) velocity.y += s.ball.dec.y;
    }
    return false;
}

}  // namespace bot
}  // namespace soccer
