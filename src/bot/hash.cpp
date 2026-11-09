#include "bot/hash.h"

namespace soccer {
namespace bot {

namespace {

inline uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h *= 0xbf58476d1ce4e5b9ULL;
    return h ^ (h >> 31);
}

inline uint64_t PackCell(Cell c) { return uint64_t(c.x + 16) << 8 | uint64_t(c.y + 16); }

}  // namespace

uint64_t PositionHash(const GameState& s) {
    uint64_t h = 0x5ca1ab1e;
    for (const PlayerState& p : s.players) {
        uint64_t v = PackCell(p.pos) << 32 | PackCell(p.turnStart) << 16 | PackCell(p.prevTurnStart);
        h = Mix(h, v);
        h = Mix(h, PackCell(p.attackDir) << 2 | uint64_t(p.moved) << 1 | uint64_t(p.attacked));
    }
    const BallState& b = s.ball;
    h = Mix(h, PackCell(b.pos) << 32 | PackCell(b.vel) << 16 | PackCell(b.dec));
    h = Mix(h, uint64_t(b.holder + 1) << 8 | uint64_t(b.stepPhase) << 4 | uint64_t(b.lastTouchTeam) << 1 |
                   uint64_t(b.lastTouchWasPass));
    h = Mix(h, static_cast<uint64_t>(b.attackTeam));
    h = Mix(h, uint64_t(s.phase) << 40 | uint64_t(s.lineupStep) << 36 | uint64_t(s.teamToMove) << 32 |
                   uint64_t(s.halfTurns) << 16 | uint64_t(s.ballPlayed) << 8 | uint64_t(s.sealHistory[0]) << 4 |
                   uint64_t(s.sealHistory[1]));
    h = Mix(h, uint64_t(s.score[0]) << 16 | uint64_t(s.score[1]));
    h = Mix(h, uint64_t(s.keeperRestartTeam + 1) << 1 | uint64_t(s.keeperBackPass));
    return h;
}

bool SameSearchState(const GameState& a, const GameState& b) {
    if (a.phase != b.phase || a.lineupStep != b.lineupStep || a.teamToMove != b.teamToMove ||
        a.halfTurns != b.halfTurns || a.turnId != b.turnId || a.ballPlayed != b.ballPlayed ||
        a.committed != b.committed || a.score[0] != b.score[0] || a.score[1] != b.score[1] ||
        a.sealHistory[0] != b.sealHistory[0] || a.sealHistory[1] != b.sealHistory[1] ||
        a.keeperRestartTeam != b.keeperRestartTeam || a.keeperBackPass != b.keeperBackPass) return false;
    const BallState& x = a.ball;
    const BallState& y = b.ball;
    if (x.pos != y.pos || x.vel != y.vel || x.dec != y.dec || x.holder != y.holder ||
        x.stepPhase != y.stepPhase || x.lastTouchTeam != y.lastTouchTeam ||
        x.lastTouchWasPass != y.lastTouchWasPass || x.attackTeam != y.attackTeam) return false;
    for (int i = 0; i < kNumPlayers; ++i) {
        const PlayerState& p = a.players[i];
        const PlayerState& q = b.players[i];
        if (p.pos != q.pos || p.turnStart != q.turnStart || p.prevTurnStart != q.prevTurnStart ||
            p.moved != q.moved || p.attacked != q.attacked || p.attackDir != q.attackDir) return false;
    }
    return true;
}

}  // namespace bot
}  // namespace soccer
