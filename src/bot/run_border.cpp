#include "bot/run_border.h"

#include "engine/rules.h"

#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <utility>

namespace soccer {
namespace bot {

namespace {

// The procedure is fixed (see run_border.h). Do not change these rules.

struct Value {
    int rows = 0;    // rows left to the goal line: the runner wants fewer
    int offset = 0;  // distance from the centre file: the runner wants less
    bool operator<(const Value& o) const { return rows != o.rows ? rows < o.rows : offset < o.offset; }
};

struct Solver {
    GameState s;
    int runner = 0, defender = 0;
    int lastRow = 0;  // the field row next to the goal line the runner attacks
    std::unordered_map<uint64_t, std::pair<Value, int>> memo;  // value and the index of the best move
    long nodes = 0;

    static int Code(const PlayerState& p) {
        const Cell v = p.Velocity();
        return ((p.turnStart.y * kFieldWidth + p.turnStart.x) * 5 + (v.x + 2)) * 5 + (v.y + 2);
    }
    uint64_t Key(bool runnerMoves) const {
        return (static_cast<uint64_t>(Code(s.players[runner])) * 1000003u + Code(s.players[defender])) * 2 + runnerMoves;
    }
    int RowsLeft(Cell c) const { return std::abs(c.y - lastRow); }
    Value Here() const {
        const Cell c = s.players[runner].turnStart;
        return {RowsLeft(c), std::abs(c.x - kFieldWidth / 2)};
    }
    static void Step(PlayerState& p, Cell target) {
        p.prevTurnStart = p.turnStart;
        p.turnStart = target;
        p.pos = target;
    }

    // Runner's legal destinations keeping the full advance (empty: loss of tempo).
    std::vector<Cell> RunnerMoves() const {
        const PlayerState& me = s.players[runner];
        const Cell v = me.Velocity();
        const Cell center{me.turnStart.x + Sign(v.x), me.turnStart.y + Sign(v.y)};
        int best = 1 << 20;
        for (int i = -1; i <= 1; ++i)
            for (int j = -1; j <= 1; ++j) {
                const Cell t{center.x + i, center.y + j};
                if (InField(t) && RowsLeft(t) < best) best = RowsLeft(t);
            }
        std::vector<Cell> out;
        if (best >= RowsLeft(me.turnStart)) return out;  // no advance at all
        for (Cell t : MoveDestinations(s, runner))
            if (RowsLeft(t) == best) out.push_back(t);
        return out;
    }

    Value Solve(bool runnerMoves) {
        ++nodes;
        if (RowsLeft(s.players[runner].turnStart) == 0) return Here();
        const uint64_t key = Key(runnerMoves);
        auto it = memo.find(key);
        if (it != memo.end()) return it->second.first;
        Value best;
        int bestIndex = -1;
        if (runnerMoves) {
            const std::vector<Cell> moves = RunnerMoves();
            if (moves.empty()) {
                best = Here();
            } else {
                for (std::size_t i = 0; i < moves.size(); ++i) {
                    const PlayerState saved = s.players[runner];
                    Step(s.players[runner], moves[i]);
                    const Value v = Solve(false);
                    s.players[runner] = saved;
                    if (bestIndex < 0 || v < best) { best = v; bestIndex = static_cast<int>(i); }
                }
            }
        } else {
            std::vector<Cell> moves = MoveDestinations(s, defender);
            if (moves.empty()) moves.push_back(s.players[defender].turnStart);
            for (std::size_t i = 0; i < moves.size(); ++i) {
                const PlayerState saved = s.players[defender];
                Step(s.players[defender], moves[i]);
                const Value v = Solve(true);
                s.players[defender] = saved;
                if (bestIndex < 0 || best < v) { best = v; bestIndex = static_cast<int>(i); }
            }
        }
        memo[key] = {best, bestIndex};
        return best;
    }

    // Replays the stored best moves.
    void Line(bool runnerMoves, RunBorderResult& r) {
        for (int guard = 0; guard < 64; ++guard) {
            if (RowsLeft(s.players[runner].turnStart) == 0) return;
            auto it = memo.find(Key(runnerMoves));
            if (it == memo.end() || it->second.second < 0) return;
            const int mover = runnerMoves ? runner : defender;
            std::vector<Cell> moves = runnerMoves ? RunnerMoves() : MoveDestinations(s, defender);
            if (!runnerMoves && moves.empty()) moves.push_back(s.players[defender].turnStart);
            const Cell t = moves[it->second.second];
            Step(s.players[mover], t);
            r.movers.push_back(mover);
            r.cells.push_back(t);
            runnerMoves = !runnerMoves;
        }
    }
};

}  // namespace

RunBorderResult SolveRunBorder(const GameState& start, int runner, int defender) {
    RunBorderResult r;
    if (start.phase != Phase::Play || runner < 0 || runner >= kNumPlayers || defender < 0 || defender >= kNumPlayers ||
        TeamOf(runner) == TeamOf(defender))
        return r;
    Solver solver;
    solver.s = start;
    // Start of a turn for both: positions are the turn starts.
    for (PlayerState& p : solver.s.players) {
        p.pos = p.turnStart;
        p.moved = false;
        p.attacked = false;
    }
    solver.runner = runner;
    solver.defender = defender;
    solver.lastRow = TeamOf(runner) == kWhite ? kFirstRow : kLastRow;
    const bool runnerFirst = start.teamToMove == TeamOf(runner);
    const Value v = solver.Solve(runnerFirst);
    r.valid = true;
    r.rowsLeft = v.rows;
    r.fileOffset = v.offset;
    r.nodes = solver.nodes;
    solver.s = start;
    for (PlayerState& p : solver.s.players) p.pos = p.turnStart;
    solver.Line(runnerFirst, r);
    return r;
}

}  // namespace bot
}  // namespace soccer
