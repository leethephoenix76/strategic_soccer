// Validation and application of actions, turn structure, legal action list.
#include "internal.h"

#include <cstddef>

namespace soccer {

using namespace detail;

namespace {

bool Contains(const std::vector<Cell>& cells, Cell c) {
    for (Cell x : cells)
        if (x == c) return true;
    return false;
}

ActionError ValidateMove(const GameState& s, int player, Cell target) {
    if (s.phase != Phase::Play) return ActionError::WrongPhase;
    if (player < 0 || player >= kNumPlayers || TeamOf(player) != s.teamToMove) return ActionError::NotYourPlayer;
    if (s.players[player].moved) return ActionError::AlreadyMoved;
    if (!InField(target)) return ActionError::OutsideField;
    if (target == s.players[player].pos) return ActionError::NoMove;
    if (!Contains(MoveDestinations(s, player), target)) return ActionError::Unreachable;
    return ActionError::None;
}

// The team to move holds the ball and has not played it in this turn.
ActionError ValidateBallAction(const GameState& s) {
    if (s.phase != Phase::Play) return ActionError::WrongPhase;
    if (s.ball.IsFree() || TeamOf(s.ball.holder) != s.teamToMove) return ActionError::NoBall;
    if (s.ballPlayed) return ActionError::BallAlreadyPlayed;
    return ActionError::None;
}

void ApplyPlace(GameState& s, int player, Cell target) {
    s.players[player].pos = target;
    s.players[player].moved = true;
}

void ApplyMove(GameState& s, int player, Cell target) {
    PlayerState& p = s.players[player];
    p.pos = target;
    p.moved = true;
    BallState& b = s.ball;
    if (b.holder == player) {
        b.pos = target;                          // dribbling
    } else if (b.IsFree() && b.pos == target) {
        b.holder = player;                       // picks up the free ball
        b.attackTeam = TeamOf(player);
        b.vel = {};
        Touch(s, TeamOf(player), false);
    }
}

void ApplyAttack(GameState& s, int player, const Outcome& o, EventLog* events) {
    if (GetAttackKind(s, player) == AttackKind::Tackle) ResolveTackle(s, player, o, events);
    else ResolvePoke(s, player, o, events);
    ResolveKeeperRestart(s, events, true);
}

void ApplyPass(GameState& s, Cell target, const Outcome& o, EventLog* events) {
    BallState& b = s.ball;
    const int passer = b.holder;
    const int team = TeamOf(passer);
    s.ballPlayed = true;
    s.committed = true;
    b.holder = kNoPlayer;
    b.vel = {target.x - b.pos.x, target.y - b.pos.y};
    b.dec = {-Sign(b.vel.x), -Sign(b.vel.y)};
    Touch(s, team, true);   // deliberate pass

    // The ball immediately runs to the target (unless intercepted on the way).
    if (ApplySegment(s, o, events)) {
        RestartInterrupted(s, Opponent(b.lastTouchTeam), events);
        return;
    }
    if (ResolveKeeperRestart(s, events, true)) return;
    if (b.IsFree()) UpdatePossession(s);
    if (b.IsFree()) {
        Decelerate(b);
        b.stepPhase = 1;   // the next step is at the End Turn of the opponent
    }
}

void EndPlayTurn(GameState& s, const Outcome& o, EventLog* events);

void ApplyShot(GameState& s, const Outcome& o, EventLog* events) {
    const int shooter = s.ball.holder;
    const int team = TeamOf(shooter);
    s.ballPlayed = true;
    s.committed = true;
    ResolveShot(s, shooter, o, events);
    if (o.kind == OutcomeKind::Goal) {
        ++s.score[team];
        RestartInterrupted(s, Opponent(team), events);   // the conceding team restarts
    } else if (o.kind == OutcomeKind::Caught) {
        RestartInterrupted(s, Opponent(team), events);   // the keeper's team restarts
    } else {
        // Parried: the shot ends the turn, the opponent is the first to go for the
        // rebound. The ball lies still, so its "step" at this End Turn changes nothing.
        Outcome still;
        still.kind = OutcomeKind::BallThrough;
        EndPlayTurn(s, still, events);
    }
}

void EndLineupTurn(GameState& s, EventLog* events) {
    for (int k = FirstPlayerOf(s.teamToMove); k < FirstPlayerOf(s.teamToMove) + kPlayersPerTeam; ++k) {
        PlayerState& p = s.players[k];
        p.prevTurnStart = p.turnStart = p.pos;   // placed players have no inertia
        p.moved = false;
    }
    s.teamToMove = Opponent(s.teamToMove);
    UpdatePossession(s);
    s.ballPlayed = false;
    s.committed = false;
    ++s.turnId;
    if (++s.lineupStep == 2) {
        s.phase = Phase::Play;
        Emit(events, EventType::TurnStarted, s.teamToMove);
    } else {
        Emit(events, EventType::LineupStarted, s.teamToMove);
    }
}

void EndPlayTurn(GameState& s, const Outcome& o, EventLog* events) {
    const int team = s.teamToMove;
    for (int k = FirstPlayerOf(team); k < FirstPlayerOf(team) + kPlayersPerTeam; ++k) {
        PlayerState& p = s.players[k];
        p.prevTurnStart = p.turnStart;
        p.turnStart = p.pos;
        p.moved = false;
        p.attacked = false;
    }

    // A free ball makes a step every second End Turn (at the End Turn of the
    // team that did not play it), so both teams can react to its path.
    BallState& b = s.ball;
    bool ballOut = false;
    if (b.IsFree()) {
        if (b.stepPhase == 0) {
            ballOut = ApplySegment(s, o, events);
            if (!ballOut) {
                UpdatePossession(s);
                if (b.IsFree()) Decelerate(b);
            }
        }
        if (!ballOut) {
            b.stepPhase ^= 1;
            UpdatePossession(s);
        }
    }

    // Deadlock: the team has sealed the ball so that the opponent can not reach it.
    bool sealPenalty = false, sealWarning = false;
    if (!ballOut && s.keeperRestartTeam < 0) {
        int& h = s.sealHistory[team];
        h = ((h << 1) | (BallSealedBy(s, team) ? 1 : 0)) & 7;
        if (h & 1) {
            int sealedTurns = (h & 1) + ((h >> 1) & 1) + ((h >> 2) & 1);
            if (sealedTurns >= 2) sealPenalty = true;   // sealed on 2 of the last 3 turns
            else sealWarning = true;
        }
    }

    s.teamToMove = Opponent(team);
    s.ballPlayed = false;
    s.committed = false;
    ++s.turnId;
    ++s.halfTurns;

    if (s.halfTurns == kHalfTurnsPerHalf) {
        Emit(events, EventType::HalfTime);
        Restart(s, kBlack, events);
    } else if (s.halfTurns >= kHalfTurnsPerMatch) {
        s.phase = Phase::Finished;
        Emit(events, EventType::MatchOver);
    } else if (ballOut) {
        Restart(s, Opponent(b.lastTouchTeam), events);
    } else if (ResolveKeeperRestart(s, events, false)) {
    } else if (sealPenalty) {
        Emit(events, EventType::SealPenalty, team);
        Restart(s, Opponent(team), events);
    } else {
        if (sealWarning) Emit(events, EventType::SealWarning, team);
        Emit(events, EventType::TurnStarted, s.teamToMove);
    }
}

}  // namespace

bool CanPassTo(const GameState& s, Cell target) {
    if (ValidateBallAction(s) != ActionError::None) return false;
    Cell from = s.players[s.ball.holder].pos;
    return InField(target) && target != from && Abs(target.x - from.x) <= kMaxPassDistance &&
           Abs(target.y - from.y) <= kMaxPassDistance;
}

ActionError Validate(const GameState& s, const Action& a) {
    if (s.phase == Phase::Finished) return ActionError::WrongPhase;
    switch (a.type) {
        case ActionType::Place: {
            ActionError err;
            CanPlace(s, a.player, a.target, &err);
            return err;
        }
        case ActionType::Move:
            return ValidateMove(s, a.player, a.target);
        case ActionType::Attack:
            if (s.phase != Phase::Play) return ActionError::WrongPhase;
            if (a.player < 0 || a.player >= kNumPlayers || TeamOf(a.player) != s.teamToMove)
                return ActionError::NotYourPlayer;
            return GetAttackKind(s, a.player) == AttackKind::None ? ActionError::CannotAttack : ActionError::None;
        case ActionType::Pass: {
            ActionError err = ValidateBallAction(s);
            if (err != ActionError::None) return err;
            return CanPassTo(s, a.target) ? ActionError::None : ActionError::BadPassTarget;
        }
        case ActionType::Shot:
            return ValidateBallAction(s);
        case ActionType::EndTurn:
            return ActionError::None;
    }
    return ActionError::WrongPhase;
}

namespace {

// Outcomes of a valid action.
void CollectOutcomes(const GameState& s, const Action& a, std::vector<Outcome>& out) {
    const BallState& b = s.ball;
    switch (a.type) {
        case ActionType::Attack:
            if (GetAttackKind(s, a.player) == AttackKind::Tackle) TackleOutcomes(s, a.player, out);
            else PokeOutcomes(s, a.player, out);
            return;
        case ActionType::Pass: {
            Cell from = b.pos;
            SegmentOutcomes(s, from, {a.target.x - from.x, a.target.y - from.y}, b.holder, out);
            return;
        }
        case ActionType::Shot:
            ShotOutcomes(s, b.holder, out);
            return;
        case ActionType::EndTurn:
            // The free ball makes a step at this End Turn.
            if (s.phase == Phase::Play && b.IsFree() && b.stepPhase == 0) {
                SegmentOutcomes(s, b.pos, b.vel, kNoPlayer, out);
                return;
            }
            break;
        default:
            break;
    }
    out.push_back(Outcome::Certain());
}

void ApplyValid(GameState& s, const Action& a, const Outcome& o, EventLog* events) {
    switch (a.type) {
        case ActionType::Place: ApplyPlace(s, a.player, a.target); break;
        case ActionType::Move: ApplyMove(s, a.player, a.target); break;
        case ActionType::Attack: ApplyAttack(s, a.player, o, events); break;
        case ActionType::Pass: ApplyPass(s, a.target, o, events); break;
        case ActionType::Shot: ApplyShot(s, o, events); break;
        case ActionType::EndTurn:
            if (s.phase == Phase::Lineup) EndLineupTurn(s, events);
            else EndPlayTurn(s, o, events);
            break;
    }
}

}  // namespace

std::vector<Outcome> Outcomes(const GameState& s, const Action& a) {
    std::vector<Outcome> out;
    if (Validate(s, a) == ActionError::None) CollectOutcomes(s, a, out);
    return out;
}

std::vector<Outcome> OutcomesUnchecked(const GameState& s, const Action& a) {
    std::vector<Outcome> out;
    CollectOutcomes(s, a, out);
    return out;
}

void ApplyUnchecked(GameState& s, const Action& a, const Outcome& o, EventLog* events) {
    ApplyValid(s, a, o, events);
}

ActionError Apply(GameState& s, const Action& a, EventLog* events, Outcome* outcome) {
    ActionError err = Validate(s, a);
    if (err != ActionError::None) return err;
    std::vector<Outcome> outcomes;
    CollectOutcomes(s, a, outcomes);
    std::size_t chosen = 0;
    if (outcomes.size() > 1) {   // sample an outcome with the generator of the state
        double r = s.rng.Uniform(), sum = 0;
        chosen = outcomes.size() - 1;
        for (std::size_t i = 0; i < outcomes.size(); ++i) {
            sum += outcomes[i].probability;
            if (r < sum) { chosen = i; break; }
        }
    }
    if (outcome) *outcome = outcomes[chosen];
    ApplyValid(s, a, outcomes[chosen], events);
    return ActionError::None;
}

ActionError ApplyOutcome(GameState& s, const Action& a, const Outcome& o, EventLog* events) {
    ActionError err = Validate(s, a);
    if (err != ActionError::None) return err;
    std::vector<Outcome> outcomes;
    CollectOutcomes(s, a, outcomes);
    for (const Outcome& x : outcomes)
        if (SameOutcome(x, o)) {
            ApplyValid(s, a, x, events);
            return ActionError::None;
        }
    return ActionError::BadOutcome;
}

std::vector<Action> LegalActions(const GameState& s) {
    std::vector<Action> actions;
    if (s.phase == Phase::Finished) return actions;
    const int first = FirstPlayerOf(s.teamToMove);

    if (s.phase == Phase::Lineup) {
        for (int k = first; k < first + kPlayersPerTeam; ++k)
            for (int x = 0; x < kFieldWidth; ++x)
                for (int y = kFirstRow; y <= kLastRow; ++y)
                    if (CanPlace(s, k, {x, y}) && s.players[k].pos != Cell{x, y})
                        actions.push_back(Action::Place(k, {x, y}));
    } else {
        for (int k = first; k < first + kPlayersPerTeam; ++k) {
            if (s.players[k].moved) continue;
            for (Cell c : MoveDestinations(s, k))
                if (c != s.players[k].pos) actions.push_back(Action::Move(k, c));
        }
        for (int k = first; k < first + kPlayersPerTeam; ++k)
            if (GetAttackKind(s, k) != AttackKind::None) actions.push_back(Action::Attack(k));
        if (ValidateBallAction(s) == ActionError::None) {
            Cell from = s.players[s.ball.holder].pos;
            for (int x = from.x - kMaxPassDistance; x <= from.x + kMaxPassDistance; ++x)
                for (int y = from.y - kMaxPassDistance; y <= from.y + kMaxPassDistance; ++y)
                    if (CanPassTo(s, {x, y})) actions.push_back(Action::Pass({x, y}));
            actions.push_back(Action::Shot());
        }
    }
    actions.push_back(Action::EndTurn());
    return actions;
}

}  // namespace soccer
