#include "bot/reach.h"

#include <cstring>
#include <vector>

namespace soccer {
namespace bot {

namespace {

constexpr int kVel = 5;   // velocity components -2..2

inline int StateIndex(Cell c, Cell v) { return ((c.x * kGridHeight + c.y) * kVel + (v.x + 2)) * kVel + (v.y + 2); }

}  // namespace

namespace {

ReachMap Search(Cell cell, Cell velocity) {
    ReachMap map;
    std::memset(map.turns, kUnreachable, sizeof map.turns);

    static thread_local uint8_t seen[kFieldWidth * kGridHeight * kVel * kVel];
    std::memset(seen, 0, sizeof seen);
    struct Node { Cell c, v; };
    static thread_local Node queue[kFieldWidth * kGridHeight * kVel * kVel];
    int head = 0, tail = 0;
    int depthEnd = 0, depth = 0;

    seen[StateIndex(cell, velocity)] = 1;
    queue[tail++] = {cell, velocity};
    map.turns[cell.x][cell.y] = 0;
    depthEnd = tail;
    while (head < tail) {
        if (head == depthEnd) {
            ++depth;
            depthEnd = tail;
        }
        Node n = queue[head++];
        Cell center{n.c.x + Sign(n.v.x), n.c.y + Sign(n.v.y)};
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                Cell d{center.x + dx, center.y + dy};
                if (!InField(d)) continue;
                Cell v{d.x - n.c.x, d.y - n.c.y};
                int idx = StateIndex(d, v);
                if (seen[idx]) continue;
                seen[idx] = 1;
                queue[tail++] = {d, v};
                if (map.turns[d.x][d.y] == kUnreachable) map.turns[d.x][d.y] = static_cast<uint8_t>(depth + 1);
            }
    }
    return map;
}

}  // namespace

ReachMap ComputeReach(Cell cell, Cell velocity) {
    // The tables depend only on (cell, velocity): 209 x 25 states, computed once each.
    static thread_local std::vector<ReachMap> cache;
    static thread_local std::vector<char> done;
    if (cache.empty()) {
        cache.resize(kFieldWidth * kGridHeight * kVel * kVel);
        done.assign(cache.size(), 0);
    }
    if (cell.x < 0 || cell.x >= kFieldWidth || cell.y < 0 || cell.y >= kGridHeight || Abs(velocity.x) > 2 ||
        Abs(velocity.y) > 2)
        return Search(cell, velocity);
    const int i = StateIndex(cell, velocity);
    if (!done[i]) {
        cache[i] = Search(cell, velocity);
        done[i] = 1;
    }
    return cache[i];
}

ReachMap PlayerReach(const GameState& s, int k) {
    const PlayerState& p = s.players[k];
    return ComputeReach(p.turnStart, p.Velocity());
}

}  // namespace bot
}  // namespace soccer
