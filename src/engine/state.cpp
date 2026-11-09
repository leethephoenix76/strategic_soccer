#include "internal.h"

namespace soccer {

namespace {

// Kick-off positions: White along the left side line in its half, Black along the right one.
const Cell kKickoff[kNumPlayers] = {
    {0, 11}, {0, 12}, {0, 13}, {0, 14},   // White
    {10, 9}, {10, 8}, {10, 7}, {10, 6},   // Black
};

}  // namespace

GameState NewMatch(uint64_t seed) {
    GameState s;
    s.rng.Seed(seed);
    detail::ResetField(s, kWhite);
    return s;
}

int PlayerAt(const GameState& s, Cell c) {
    for (int k = 0; k < kNumPlayers; ++k)
        if (s.players[k].pos == c) return k;
    return kNoPlayer;
}

namespace detail {

void Emit(EventLog* events, EventType type, int team, int player) {
    if (events) events->push_back(Event{type, team, player});
}

void ResetField(GameState& s, int firstTeam) {
    for (int k = 0; k < kNumPlayers; ++k) {
        PlayerState& p = s.players[k];
        p.pos = p.turnStart = p.prevTurnStart = kKickoff[k];
        p.moved = false;
        p.attacked = false;
        p.attackDir = {};
    }
    BallState& b = s.ball;
    b.pos = {kCenterX, kCenterY};
    b.vel = b.dec = {};
    b.holder = kNoPlayer;
    b.stepPhase = 0;
    b.lastTouchWasPass = false;
    b.attackTeam = firstTeam;

    s.phase = Phase::Lineup;
    s.lineupStep = 0;
    s.teamToMove = firstTeam;
    s.ballPlayed = false;
    s.committed = false;
    s.keeperRestartTeam = -1;
    s.keeperBackPass = false;
    s.sealHistory[0] = s.sealHistory[1] = 0;
    ++s.turnId;
}

void Restart(GameState& s, int firstTeam, EventLog* events) {
    ResetField(s, firstTeam);
    Emit(events, EventType::LineupStarted, firstTeam);
}

void RestartInterrupted(GameState& s, int firstTeam, EventLog* events) {
    ++s.halfTurns;   // the interrupted turn counts as played: the clock runs on
    if (s.halfTurns == kHalfTurnsPerHalf) {
        Emit(events, EventType::HalfTime);
        Restart(s, kBlack, events);
    } else if (s.halfTurns >= kHalfTurnsPerMatch) {
        s.phase = Phase::Finished;
        s.keeperRestartTeam = -1;
        ++s.turnId;
        Emit(events, EventType::MatchOver);
    } else {
        Restart(s, firstTeam, events);
    }
}

}  // namespace detail
}  // namespace soccer
