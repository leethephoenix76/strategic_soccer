// Deadlock detection: a team seals the ball so that the opponent can not reach it.
#include "internal.h"

namespace soccer {

bool BallSealedBy(const GameState& s, int team) {
    const BallState& b = s.ball;
    if (!b.IsFree() && TeamOf(b.holder) != team) return false;
    const Cell ball = b.pos;

    // A wall around the ball needs at least three players of `team` near it.
    int near = 0;
    for (int k = FirstPlayerOf(team); k < FirstPlayerOf(team) + kPlayersPerTeam; ++k)
        if (ChebyshevDistance(s.players[k].pos, ball) <= 3) ++near;
    if (near < 3) return false;

    // Obstacles are the players of `team` only: the opponent can move his own players away.
    bool wall[kFieldWidth][kGridHeight] = {};
    bool opponent[kFieldWidth][kGridHeight] = {};
    bool seen[kFieldWidth][kGridHeight] = {};
    Cell queue[kFieldWidth * kGridHeight];
    int head = 0, tail = 0;
    for (int k = 0; k < kNumPlayers; ++k) {
        Cell c = s.players[k].pos;
        if (TeamOf(k) == team) wall[c.x][c.y] = true;
        else opponent[c.x][c.y] = true;
    }

    // An opponent player has contact with the ball when he reaches the ball cell or
    // a neighbouring one (enter the ball cell, tackle or poke the ball). He moves in
    // one-cell steps in 8 directions through cells free of `team`; a diagonal step is
    // impossible through a corridor closed on both sides. The steps are symmetric,
    // so the search goes backwards: from the contact cells to any opponent player.
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            Cell c{ball.x + dx, ball.y + dy};
            if (!InField(c) || wall[c.x][c.y]) continue;
            seen[c.x][c.y] = true;
            queue[tail++] = c;
        }
    while (head < tail) {
        Cell c = queue[head++];
        if (opponent[c.x][c.y]) return false;
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                Cell n{c.x + dx, c.y + dy};
                if ((dx == 0 && dy == 0) || !InField(n) || wall[n.x][n.y] || seen[n.x][n.y]) continue;
                if (dx && dy && wall[n.x][c.y] && wall[c.x][n.y]) continue;   // closed corridor
                seen[n.x][n.y] = true;
                queue[tail++] = n;
            }
        }
    }
    return true;
}

}  // namespace soccer
