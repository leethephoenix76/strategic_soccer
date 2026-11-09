#include "bot/value.h"

#include "bot/bot.h"
#include "bot/reach.h"
#include "engine/rules.h"

#include <algorithm>

namespace soccer {
namespace bot {

ValueTable ValueTable::FromMover(const ValueTable& m) {
    ValueTable t;
    const int mover = Kind(BallOwner::Mover);
    for (int x = 0; x < kFieldWidth; ++x)
        for (int y = 0; y < kGridHeight; ++y) {
            const Cell c{x, y}, r{kFieldWidth - 1 - x, kGridHeight - 1 - y};
            const double ours = m.Get(mover, c), theirs = m.Get(mover, r);
            t.Set(mover, c, ours);
            t.Set(Kind(BallOwner::Other), c, -theirs);
            for (int d = -kMaxRace; d <= kMaxRace; ++d) t.Set(Kind(BallOwner::Free, d), c, d >= 0 ? ours : -theirs);
            t.Set(kFreeAny, c, 0.5 * (ours - theirs));
        }
    return t;
}

extern const double kPossessionTableData[kFieldWidth * kGridHeight];

const ValueTable& ValueTable::PossessionDefault() {
    static const ValueTable table = [] {
        ValueTable m;
        for (int x = 0; x < kFieldWidth; ++x)
            for (int y = 0; y < kGridHeight; ++y)
                m.Set(Kind(BallOwner::Mover), {x, y}, kPossessionTableData[x * kGridHeight + y]);
        return FromMover(m);
    }();
    return table;
}

BallOwner ValueTable::OwnerAt(const GameState& s) {
    if (s.ball.IsFree()) return BallOwner::Free;
    return TeamOf(s.ball.holder) == s.teamToMove ? BallOwner::Mover : BallOwner::Other;
}

int ValueTable::Race(const GameState& s) {
    int turns[2] = {kUnreachable, kUnreachable};
    for (int k = 0; k < kNumPlayers; ++k) turns[TeamOf(k)] = std::min(turns[TeamOf(k)], PlayerReach(s, k).Turns(s.ball.pos));
    const int d = turns[Opponent(s.teamToMove)] - turns[s.teamToMove];
    return std::max(-kMaxRace, std::min(kMaxRace, d));
}

int ValueTable::KindAt(const GameState& s) {
    const BallOwner owner = OwnerAt(s);
    return owner == BallOwner::Free ? Kind(owner, Race(s)) : Kind(owner);
}

double ValueTable::OfState(GameState s, int team) const {
    for (int guard = 0; guard < 16 && s.phase == Phase::Lineup; ++guard) {
        Action a = LineupAction(s);
        if (Validate(s, a) != ActionError::None) break;
        ApplyUnchecked(s, a, Outcome::Certain());
    }
    if (s.phase != Phase::Play) return 0;
    const double v = Get(KindAt(s), Frame(s.ball.pos, s.teamToMove));
    return s.teamToMove == team ? v : -v;
}

}  // namespace bot
}  // namespace soccer
