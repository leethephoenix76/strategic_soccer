// Fights for the ball: tackling the ball holder and poking out a sealed free ball.
#include "internal.h"

namespace soccer {

bool AttackDirectionAllowed(Cell attacker, Cell ball, Cell velocity, Cell displacement) {
    const int dx = ball.x - attacker.x, dy = ball.y - attacker.y;
    return dx * velocity.x + dy * velocity.y >= 0 && dx * displacement.x + dy * displacement.y >= 0;
}

double TackleChance(Cell holder, Cell attacker, int attackerTeam) {
    // Base 35%; -5% if not in the same column; +-5% per row the attacker stands
    // closer to (further from) his own goal than the holder.
    const double inc = attackerTeam == kBlack ? 0.05 : -0.05;
    return 0.35 - 0.05 * (holder.x != attacker.x) + inc * (holder.y - attacker.y);
}

AttackKind GetAttackKind(const GameState& s, int player) {
    if (s.phase != Phase::Play || player < 0 || player >= kNumPlayers) return AttackKind::None;
    if (TeamOf(player) != s.teamToMove) return AttackKind::None;
    const PlayerState& a = s.players[player];
    if (a.attacked) return AttackKind::None;   // one attack per player per turn

    const BallState& b = s.ball;
    if (!b.IsFree() && TeamOf(b.holder) == s.teamToMove) return AttackKind::None;

    // Only from an adjacent cell (including diagonals).
    int dx = b.pos.x - a.pos.x, dy = b.pos.y - a.pos.y;
    if (Abs(dx) > 1 || Abs(dy) > 1 || (dx == 0 && dy == 0)) return AttackKind::None;

    if (!AttackDirectionAllowed(a.pos, b.pos, a.Velocity(), a.Displacement())) return AttackKind::None;

    if (!b.IsFree()) return AttackKind::Tackle;

    // A free ball can be attacked only diagonally, when both corridor cells are
    // occupied by opponents (otherwise the player has to step onto the ball).
    if (dx == 0 || dy == 0) return AttackKind::None;
    int c1 = PlayerAt(s, {a.pos.x, b.pos.y});
    int c2 = PlayerAt(s, {b.pos.x, a.pos.y});
    if (c1 == kNoPlayer || c2 == kNoPlayer) return AttackKind::None;
    if (TeamOf(c1) == s.teamToMove || TeamOf(c2) == s.teamToMove) return AttackKind::None;
    return AttackKind::Poke;
}

namespace detail {

namespace {

void StartAttack(GameState& s, int attacker) {
    PlayerState& a = s.players[attacker];
    a.attacked = true;
    a.attackDir = {s.ball.pos.x - a.pos.x, s.ball.pos.y - a.pos.y};   // a later move must not go against it
    s.committed = true;
}

void GiveBallTo(GameState& s, int player) {
    BallState& b = s.ball;
    b.holder = player;
    b.attackTeam = TeamOf(player);
    b.pos = s.players[player].pos;
    b.vel = {};
    Touch(s, TeamOf(player), false);
}

// Outcomes of a fight for the ball: won with probability p, knocked away with
// 0.2 (into a uniformly random cell around `center`, not into the cells of the
// two fighters), otherwise nothing changes.
void DuelOutcomes(double p, Cell center, Cell other, std::vector<Outcome>& out) {
    Outcome won;
    won.kind = OutcomeKind::Won;
    won.probability = p;
    if (p > 0) out.push_back(won);
    Cell cells[25];
    int count = CellsAround(center, center, other, cells);
    for (int i = 0; i < count; ++i) {
        Outcome o;
        o.kind = OutcomeKind::KnockedAway;
        o.cell = cells[i];
        o.probability = kKnockAwayChance / count;
        out.push_back(o);
    }
    Outcome kept;
    kept.kind = OutcomeKind::Kept;
    kept.probability = 1 - p - kKnockAwayChance;
    if (kept.probability > 0) out.push_back(kept);
}

double TackleProbability(const GameState& s, int attacker) {
    return TackleChance(s.players[s.ball.holder].pos, s.players[attacker].pos, TeamOf(attacker));
}

double PokeProbability(const GameState& s, int attacker) {
    // 50 / 35 / 25% against 2 / 3 / 4 opponents next to the ball.
    int n = 0;
    for (int k = 0; k < kNumPlayers; ++k)
        if (TeamOf(k) != TeamOf(attacker) && ChebyshevDistance(s.players[k].pos, s.ball.pos) <= 1) ++n;
    return n <= 2 ? 0.5 : (n == 3 ? 0.35 : 0.25);
}

void ApplyDuel(GameState& s, int attacker, const Outcome& o, EventLog* events, EventType won,
               EventType knocked, EventType failed) {
    StartAttack(s, attacker);
    if (o.kind == OutcomeKind::Won) {
        GiveBallTo(s, attacker);
        Emit(events, won, TeamOf(attacker), attacker);
    } else if (o.kind == OutcomeKind::KnockedAway) {
        BallState& b = s.ball;
        b.pos = o.cell;
        b.holder = kNoPlayer;
        b.vel = {};
        Touch(s, TeamOf(attacker), false);
        UpdatePossession(s);
        if (b.IsFree()) KeeperCheck(s, b.pos);
        Emit(events, knocked, TeamOf(attacker), attacker);
    } else {
        Emit(events, failed, TeamOf(attacker), attacker);
    }
}

}  // namespace

void TackleOutcomes(const GameState& s, int attacker, std::vector<Outcome>& out) {
    DuelOutcomes(TackleProbability(s, attacker), s.players[s.ball.holder].pos, s.players[attacker].pos, out);
}

void PokeOutcomes(const GameState& s, int attacker, std::vector<Outcome>& out) {
    DuelOutcomes(PokeProbability(s, attacker), s.ball.pos, s.players[attacker].pos, out);
}

void ResolveTackle(GameState& s, int attacker, const Outcome& o, EventLog* events) {
    ApplyDuel(s, attacker, o, events, EventType::TackleWon, EventType::TackleKnockedAway, EventType::TackleFailed);
}

void ResolvePoke(GameState& s, int attacker, const Outcome& o, EventLog* events) {
    ApplyDuel(s, attacker, o, events, EventType::PokeWon, EventType::PokeKnockedAway, EventType::PokeFailed);
}

}  // namespace detail
}  // namespace soccer
