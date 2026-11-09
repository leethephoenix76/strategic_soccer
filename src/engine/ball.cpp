// The ball: possession, rolling with deceleration, interceptions, throw-outs,
// and the keeper taking a ball in the goal mouth.
#include "internal.h"

#include <algorithm>
#include <cmath>

namespace soccer {

double InterceptionWeight(double distance) { return 0.6 * std::exp(-2.0 * distance * distance); }

namespace {

// Once the ball is already on an edge, any outward velocity leaves the field.
// Clipping the whole vector could move it sideways onto another edge cell.
bool ExitsFromBoundary(Cell pos, Cell vel) {
    return !InField({pos.x + Sign(vel.x), pos.y + Sign(vel.y)});
}

// Interactions on the final in-field cell still happen before an out.
bool ClipAtLines(Cell& p) {
    const bool outside = !InField(p);
    p.x = std::max(0, std::min(kFieldWidth - 1, p.x));
    p.y = std::max(kFirstRow, std::min(kLastRow, p.y));
    return outside;
}

// The step from -> to brings the ball into a goal mouth cell where no player
// stands: it ends there or runs along an end line across the mouth.
bool FindMouthCell(const GameState& s, Cell from, Cell to, Cell& mouth) {
    if (from.y == to.y && from.x != to.x && (to.y == kFirstRow || to.y == kLastRow)) {
        int step = to.x > from.x ? 1 : -1;
        for (int x = from.x + step; x != to.x + step; x += step) {
            if (InGoalMouth(x, to.y) && PlayerAt(s, {x, to.y}) == kNoPlayer) {
                mouth = {x, to.y};
                return true;
            }
        }
        return false;
    }
    if (InGoalMouth(to.x, to.y) && PlayerAt(s, to) == kNoPlayer) {
        mouth = to;
        return true;
    }
    return false;
}

// Path of a ball starting at `pos` with velocity `vel` (ignoring players).
// Stops early when the ball would be taken by a keeper.
std::vector<Cell> RollPath(const GameState& s, Cell pos, Cell vel, bool* reachesMouth = nullptr, int* mouthRow = nullptr) {
    std::vector<Cell> path;
    Cell dec{-Sign(vel.x), -Sign(vel.y)};
    if (reachesMouth) *reachesMouth = false;
    while (vel.x != 0 || vel.y != 0) {
        if (ExitsFromBoundary(pos, vel)) {
            path.push_back(pos);   // already at the last in-field cell
            return path;
        }
        Cell next{pos.x + vel.x, pos.y + vel.y};
        const bool outside = ClipAtLines(next);
        Cell mouth;
        if (FindMouthCell(s, pos, next, mouth)) {
            path.push_back(mouth);
            if (reachesMouth) *reachesMouth = true;
            if (mouthRow) *mouthRow = mouth.y;
            return path;
        }
        path.push_back(next);
        if (outside) return path;
        pos = next;
        if (vel.x) vel.x += dec.x;
        if (vel.y) vel.y += dec.y;
    }
    return path;
}

}  // namespace

namespace detail {

void Touch(GameState& s, int team, bool pass) {
    s.ball.lastTouchTeam = team;
    s.ball.lastTouchWasPass = pass;
}

void UpdatePossession(GameState& s) {
    BallState& b = s.ball;
    b.holder = PlayerAt(s, b.pos);
    if (b.holder != kNoPlayer) {
        b.vel = {};   // the ball is stopped by the player
        b.attackTeam = TeamOf(b.holder);
        Touch(s, TeamOf(b.holder), false);
    }
}

void Decelerate(BallState& b) {
    if (b.vel.x) b.vel.x += b.dec.x;
    if (b.vel.y) b.vel.y += b.dec.y;
}

int CellsAround(Cell center, Cell ex1, Cell ex2, Cell cells[25]) {
    int count = 0;
    for (int x = center.x - 2; x <= center.x + 2; ++x)
        for (int y = center.y - 2; y <= center.y + 2; ++y) {
            Cell c{x, y};
            if (InField(c) && c != ex1 && c != ex2) cells[count++] = c;
        }
    if (count == 0) cells[count++] = center;   // never happens on a real field
    return count;
}

void KeeperCheck(GameState& s, Cell c) {
    if (s.keeperRestartTeam >= 0 || !InGoalMouth(c.x, c.y) || PlayerAt(s, c) != kNoPlayer) return;
    int keeperTeam = MouthOwner(c.y);
    // A deliberate pass of the keeper's own team gives the ball to the opponent.
    s.keeperBackPass = s.ball.lastTouchWasPass && s.ball.lastTouchTeam == keeperTeam;
    s.keeperRestartTeam = s.keeperBackPass ? Opponent(keeperTeam) : keeperTeam;
}

bool ResolveKeeperRestart(GameState& s, EventLog* events, bool midTurn) {
    if (s.keeperRestartTeam < 0) return false;
    int team = s.keeperRestartTeam;
    if (s.keeperBackPass) Emit(events, EventType::BackPassPenalty, Opponent(team));
    else Emit(events, EventType::KeeperTookBall, team);
    if (midTurn) RestartInterrupted(s, team, events);
    else Restart(s, team, events);
    return true;
}

namespace {

// Geometry of a ball segment pos -> pos + vel and the players who may touch the
// ball on the way, in the order the ball reaches them.
struct Segment {
    Cell start, end;
    int n = 0;
    int players[kNumPlayers];
    double p[kNumPlayers];   // touch probability of each candidate
};

Segment MakeSegment(const GameState& s, Cell start, Cell vel, int excludePlayer) {
    Segment g;
    g.start = start;
    if (ExitsFromBoundary(start, vel)) {
        g.end = start;
        return g;
    }
    g.end = {start.x + vel.x, start.y + vel.y};
    ClipAtLines(g.end);
    int dx = g.end.x - start.x, dy = g.end.y - start.y;   // actual displacement of this step
    double len2 = double(dx * dx + dy * dy);
    if (len2 == 0) return g;

    // Candidates: players inside the bounding box of the segment, except the passer
    // and the player in the end cell (he simply receives the ball).
    struct Candidate { double t; int player; };
    Candidate candidates[kNumPlayers];
    int n = 0;
    for (int k = 0; k < kNumPlayers; ++k) {
        Cell p = s.players[k].pos;
        if (k == excludePlayer || p == g.end) continue;
        if ((start.x - p.x) * (p.x - g.end.x) < 0 || (start.y - p.y) * (p.y - g.end.y) < 0) continue;
        candidates[n++] = {((p.x - start.x) * dx + (p.y - start.y) * dy) / len2, k};
    }
    std::stable_sort(candidates, candidates + n,
                     [](const Candidate& a, const Candidate& c) { return a.t < c.t; });
    for (int c = 0; c < n; ++c) {
        Cell p = s.players[candidates[c].player].pos;
        double dist = std::fabs(dx * double(p.y - start.y) - dy * double(p.x - start.x)) / std::sqrt(len2);
        g.players[c] = candidates[c].player;
        g.p[c] = InterceptionWeight(dist);
    }
    g.n = n;
    return g;
}

}  // namespace

void SegmentOutcomes(const GameState& s, Cell start, Cell vel, int excludePlayer, std::vector<Outcome>& out) {
    Segment g = MakeSegment(s, start, vel, excludePlayer);
    // Each candidate in turn: interception with probability p^2, deflection with
    // p - p^2 (into a uniformly random cell around him), otherwise the ball goes on.
    double reach = 1.0;
    for (int c = 0; c < g.n; ++c) {
        int k = g.players[c];
        double p = g.p[c];
        Outcome o;
        o.kind = OutcomeKind::Intercepted;
        o.player = k;
        o.probability = reach * p * p;
        if (o.probability > 0) out.push_back(o);

        Cell pos = s.players[k].pos;
        Cell cells[25];
        int count = CellsAround(pos, pos, pos, cells);
        double each = reach * (p - p * p) / count;
        if (each > 0)
            for (int i = 0; i < count; ++i) {
                Outcome d;
                d.kind = OutcomeKind::Deflected;
                d.player = k;
                d.cell = cells[i];
                d.probability = each;
                out.push_back(d);
            }
        reach *= 1 - p;
    }
    Outcome through;
    through.kind = OutcomeKind::BallThrough;
    through.probability = reach;
    out.push_back(through);
}

double SegmentThroughProbability(const GameState& s, Cell start, Cell vel, int excludePlayer) {
    const Segment g = MakeSegment(s, start, vel, excludePlayer);
    double through = 1.0;
    for (int c = 0; c < g.n; ++c) through *= 1.0 - g.p[c];
    return through;
}

bool ApplySegment(GameState& s, const Outcome& o, EventLog* events) {
    BallState& b = s.ball;
    if (o.kind == OutcomeKind::Intercepted) {
        const int k = o.player;
        b.pos = s.players[k].pos;
        b.vel = {};
        b.holder = k;
        b.attackTeam = TeamOf(k);
        Touch(s, TeamOf(k), false);
        Emit(events, EventType::BallIntercepted, TeamOf(k), k);
        return false;
    }
    if (o.kind == OutcomeKind::Deflected) {   // into a random cell around the player
        const int k = o.player;
        b.pos = o.cell;
        b.vel = {};
        Touch(s, TeamOf(k), false);
        UpdatePossession(s);
        if (b.IsFree()) KeeperCheck(s, b.pos);
        Emit(events, EventType::BallDeflected, TeamOf(k), k);
        return false;
    }

    // Nobody touched the ball: it reaches the end of the segment.
    Cell start = b.pos;
    if (ExitsFromBoundary(start, b.vel)) {
        Emit(events, EventType::BallOut, b.lastTouchTeam);
        return true;
    }
    Cell end{start.x + b.vel.x, start.y + b.vel.y};
    const bool outside = ClipAtLines(end);
    Cell mouth;
    bool inMouth = FindMouthCell(s, start, end, mouth);
    b.pos = end;
    b.holder = kNoPlayer;
    if (inMouth) {            // the keeper takes the ball there
        b.pos = mouth;
        KeeperCheck(s, mouth);
        return false;
    }
    if (outside && PlayerAt(s, end) == kNoPlayer) {
        Emit(events, EventType::BallOut, b.lastTouchTeam);
        return true;
    }
    return false;
}

}  // namespace detail

double PassThroughProbability(const GameState& s, Cell target) {
    if (!CanPassTo(s, target)) return 0;
    return detail::SegmentThroughProbability(s, s.ball.pos,
                                             {target.x - s.ball.pos.x, target.y - s.ball.pos.y},
                                             s.ball.holder);
}

std::vector<Cell> PredictBallPath(const GameState& s) {
    if (!s.ball.IsFree()) return {};
    return RollPath(s, s.ball.pos, s.ball.vel);
}

std::vector<Cell> PredictPassPath(const GameState& s, Cell target) {
    if (s.ball.IsFree()) return {};
    return RollPath(s, s.ball.pos, {target.x - s.ball.pos.x, target.y - s.ball.pos.y});
}

bool PassReachesOwnKeeper(const GameState& s, Cell target) {
    if (s.ball.IsFree()) return false;
    bool reaches = false;
    int row = -1;
    RollPath(s, s.ball.pos, {target.x - s.ball.pos.x, target.y - s.ball.pos.y}, &reaches, &row);
    return reaches && row == OwnMouthRow(TeamOf(s.ball.holder));
}

}  // namespace soccer
