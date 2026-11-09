#include "bot/reach_front.h"

#include "engine/rules.h"

#include <bitset>
#include <utility>

namespace soccer::bot {
namespace {

constexpr int kDirections = 9;  // 3 signs on each axis
constexpr int kLocalMasks = 1 << kDirections;
constexpr int kStates = kReachFrontCells * kDirections;
constexpr unsigned kWhitePlayers = (1u << kPlayersPerTeam) - 1;
constexpr unsigned kBlackPlayers = kWhitePlayers << kPlayersPerTeam;
using StateIndex = uint16_t;

static_assert(kStates <= (1 << 16), "Reach states must fit in StateIndex");
static_assert(kNumPlayers <= 8, "Reach claims must fit in a byte");

Cell FrontCell(int index) {
    return {index % kFieldWidth, kFirstRow + index / kFieldWidth};
}

StateIndex Pack(Cell cell, Cell velocity) {
    const int direction = (Sign(velocity.x) + 1) * 3 + Sign(velocity.y) + 1;
    return static_cast<StateIndex>(ReachFrontIndex(cell) * kDirections + direction);
}

struct LocalMoves {
    std::array<int16_t, kDirections> cells;
    std::array<StateIndex, kDirections> destinations;
    std::array<uint16_t, kLocalMasks> legal;
};

struct MoveTable {
    std::array<unsigned, kLocalMasks> firstBit;
    std::array<LocalMoves, kStates> moves;

    MoveTable() {
        firstBit[0] = 0;
        for (int mask = 1; mask < kLocalMasks; ++mask)
            firstBit[mask] = (mask & 1) ? 0 : 1 + firstBit[mask >> 1];
        for (int state = 0; state < kStates; ++state) Build(state);
    }

    void Build(int state) {
        LocalMoves& entry = moves[state];
        const Cell start = FrontCell(state / kDirections);
        const int direction = state % kDirections;
        const Cell center{start.x + direction / 3 - 1, start.y + direction % 3 - 1};
        std::array<Cell, kDirections> square;
        unsigned inField = 0;
        entry.cells.fill(-1);
        for (int i = 0; i < kDirections; ++i) {
            const Cell target{center.x + i / 3 - 1, center.y + i % 3 - 1};
            square[i] = target;
            if (!InField(target)) continue;
            entry.cells[i] = static_cast<int16_t>(ReachFrontIndex(target));
            entry.destinations[i] = Pack(target, {target.x - start.x, target.y - start.y});
            inField |= 1u << i;
        }

        // A target is closed by one cell or by two cells closing a diagonal.
        // Derive both cases from the same geometry as MoveDestinations.
        std::array<uint16_t, kLocalMasks> closed{};
        for (int a = 0; a < kDirections; ++a) {
            if (entry.cells[a] < 0) continue;
            for (int b = a; b < kDirections; ++b) {
                if (entry.cells[b] < 0) continue;
                const Cell blockers[]{square[a], square[b]};
                const unsigned mask = (1u << a) | (1u << b);
                for (int t = 0; t < kDirections; ++t) {
                    if (entry.cells[t] >= 0 && MovePathBlocked(start, square[t], blockers, a == b ? 1 : 2))
                        closed[mask] |= static_cast<uint16_t>(1u << t);
                }
            }
        }
        entry.legal[0] = static_cast<uint16_t>(inField);
        for (unsigned mask = 1; mask < kLocalMasks; ++mask) {
            const unsigned lowest = 1u << firstBit[mask];
            const unsigned rest = mask ^ lowest;
            uint16_t blocked = closed[rest] | closed[lowest];
            for (unsigned bits = rest; bits; bits &= bits - 1)
                blocked |= closed[lowest | (1u << firstBit[bits])];
            closed[mask] = blocked;
            entry.legal[mask] = static_cast<uint16_t>(inField & ~blocked);
        }
    }
};

struct Wave {
    std::vector<StateIndex> states;
    std::bitset<kStates> seen;

    void Insert(StateIndex state) {
        if (seen[state]) return;
        seen.set(state);
        states.push_back(state);
    }
};

}  // namespace

ReachFront ComputeReachFront(const GameState& s, int maxHalfTurns, unsigned ignore) {
    ReachFront result;
    result.firstArrival.fill(-1);
    if (s.phase != Phase::Play || (s.teamToMove != kWhite && s.teamToMove != kBlack))
        return result;

    ReachFrontFrame frame;
    std::array<Wave, kNumPlayers> waves;
    // Inside a turn (07.11.2026): a player of the team to move who has already
    // moved stands for the rest of this half-turn and carries his displacement
    // as inertia, as the ball race's runners do (RunnerOf). An attack's
    // direction limit on a later move is ignored: the area may only be larger.
    unsigned standing = 0;
    for (int i = 0; i < kNumPlayers; ++i) {
        const PlayerState& p = s.players[i];
        if (!InField(p.pos)) return result;
        if (ignore >> i & 1u) continue;   // left out: neither claims nor blocks
        const int cell = ReachFrontIndex(p.pos);
        if (frame.players[cell]) return result;
        frame.players[cell] = static_cast<uint8_t>(1u << i);
        result.firstArrival[cell] = 0;
        const bool moved = p.moved && TeamOf(i) == s.teamToMove;
        if (moved) standing |= 1u << i;
        waves[i].Insert(Pack(p.pos, moved ? p.Displacement() : p.Velocity()));
    }
    result.frames.push_back(frame);
    result.stop = ReachFrontStop::Horizon;
    if (maxHalfTurns <= 0) return result;

    static const MoveTable table;
    Wave next;
    int claimed = kNumPlayers;
    bool previousUnchanged = false;
    int team = s.teamToMove;
    for (int i = 0; i < maxHalfTurns; ++i, team ^= 1) {
        const int halfTurn = i + 1;
        const unsigned opponents = team == kWhite ? kBlackPlayers : kWhitePlayers;
        bool unchanged = true;
        for (int k = FirstPlayerOf(team); k < FirstPlayerOf(team) + kPlayersPerTeam; ++k) {
            if (i == 0 && (standing >> k & 1u)) continue;
            if (ignore >> k & 1u) continue;
            next.states.clear();
            next.seen.reset();
            for (StateIndex state : waves[k].states) {
                const LocalMoves& moves = table.moves[state];
                unsigned occupied = 0;
                for (int j = 0; j < kDirections; ++j)
                    if (moves.cells[j] >= 0 && (frame.players[moves.cells[j]] & opponents))
                        occupied |= 1u << j;
                for (unsigned mask = moves.legal[occupied]; mask; mask &= mask - 1)
                    next.Insert(moves.destinations[table.firstBit[mask]]);
            }
            for (StateIndex state : next.states) {
                const int cell = state / kDirections;
                if (!frame.players[cell]) {
                    ++claimed;
                    result.firstArrival[cell] = halfTurn;
                }
                frame.players[cell] |= static_cast<uint8_t>(1u << k);
            }
            unchanged = unchanged && next.seen == waves[k].seen;
            std::swap(next, waves[k]);
        }
        result.frames.push_back(frame);
        if (claimed == kReachFrontCells) {
            result.stop = ReachFrontStop::Full;
            break;
        }
        if (unchanged && previousUnchanged) {
            result.stop = ReachFrontStop::Stable;
            break;
        }
        previousUnchanged = unchanged;
    }
    return result;
}

int ReachPassFirstTeam(const GameState& s, const ReachFront& front,
                       const std::vector<BallWindow>& windows) {
    if (front.frames.empty() || s.phase != Phase::Play) return -1;
    for (const BallWindow& window : windows) {
        const int cell = ReachFrontIndex(window.cell);
        if (window.arrival >= 0) {
            const int frame = (window.arrival + 1) / 2;
            if (frame < static_cast<int>(front.frames.size())) {
                const unsigned mask = front.frames[frame].players[cell];
                if (mask) return mask & kWhitePlayers ? kWhite : kBlack;
            }
        }
        const int firstSlot = window.arrival < 0 ? 0 : (window.arrival + 1) / 2;
        const int lastSlot = window.openEnd ? static_cast<int>(front.frames.size()) - 2 : window.lastMoveSlot;
        for (int slot = firstSlot; slot <= lastSlot && slot + 1 < static_cast<int>(front.frames.size()); ++slot) {
            const unsigned mask = front.frames[slot + 1].players[cell];
            if (mask) return mask & kWhitePlayers ? kWhite : kBlack;
        }
    }
    return -1;
}

int ReachPassTaker(const GameState& s, const ReachFront& front, const Action& pass) {
    if (pass.type != ActionType::Pass || PlayerAt(s, pass.target) != kNoPlayer) return -1;
    GameState after = s;
    ApplyUnchecked(after, pass, Outcome{OutcomeKind::BallThrough});
    if (after.phase != Phase::Play || !after.ball.IsFree()) return -1;
    return ReachPassFirstTeam(s, front, BallPathWindows(after));
}

ReachBreakaway ReachHolderBreakaway(const GameState& s, const ReachFront& front) {
    ReachBreakaway r;
    if (front.frames.empty() || s.phase != Phase::Play || s.ball.IsFree()) return r;
    const int holder = s.ball.holder;
    const int team = TeamOf(holder);
    const unsigned bit = 1u << holder;
    for (std::size_t f = 0; f < front.frames.size(); ++f)
        for (int i = 0; i < kReachFrontCells; ++i) {
            if (!(front.frames[f].players[i] & bit)) continue;
            if (f > 0 && (front.frames[f - 1].players[i] & bit)) continue;
            const double p = OpenShotProbability(FrontCell(i), team);
            if (p > r.shot) {
                r.shot = p;
                r.halfTurn = static_cast<int>(f);
                r.cell = FrontCell(i);
            }
        }
    return r;
}

std::vector<std::array<int, 2>> ReachFrontBorders(const ReachFrontFrame& frame) {
    std::vector<std::array<int, 2>> borders;
    for (int i = 0; i < kReachFrontCells; ++i) {
        const unsigned mask = frame.players[i];
        if (!mask) continue;
        const unsigned opponents = mask & kWhitePlayers ? kBlackPlayers : kWhitePlayers;
        if (i % kFieldWidth != kFieldWidth - 1 && (frame.players[i + 1] & opponents))
            borders.push_back({i, i + 1});
        if (i + kFieldWidth < kReachFrontCells && (frame.players[i + kFieldWidth] & opponents))
            borders.push_back({i, i + kFieldWidth});
    }
    return borders;
}

}  // namespace soccer::bot
