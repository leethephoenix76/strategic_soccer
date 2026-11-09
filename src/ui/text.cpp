#include "ui/text.h"

namespace soccer {
namespace ui {

const char* TeamName(int team) { return team == kWhite ? "White" : "Black"; }

std::string EventText(const Event& e) {
    const std::string team = e.team >= 0 ? TeamName(e.team) : "";
    const std::string other = e.team >= 0 ? TeamName(Opponent(e.team)) : "";
    switch (e.type) {
        case EventType::LineupStarted: return "Initialize team " + team;
        case EventType::TurnStarted: return "Move team " + team;
        case EventType::BallIntercepted: return "Ball intercepted!";
        case EventType::BallDeflected: return "Ball interception - deflected!";
        case EventType::BallOut: return "Ball out! " + other + " restarts from the centre";
        case EventType::TackleWon: return "Attacking player got the ball";
        case EventType::TackleKnockedAway: return "Ball lost control";
        case EventType::TackleFailed: return "Player under attack keeps the ball";
        case EventType::PokeWon: return "The ball is poked out!";
        case EventType::PokeKnockedAway: return "The ball is knocked away";
        case EventType::PokeFailed: return "The ball stays where it is";
        case EventType::ShotTaken: return "Goal Shot!";
        case EventType::Goal: return "Goal!!!";
        case EventType::KeeperCaught: return "Keeper caught the ball";
        case EventType::KeeperParried: return "Keeper reflected the ball";
        case EventType::KeeperTookBall: return "The ball rolled to the goal - the keeper takes it";
        case EventType::BackPassPenalty: return "Pass to the own keeper - possession goes to " + other;
        case EventType::SealWarning: return team + " is blocking the ball! Open it, or possession goes to " + other;
        case EventType::SealPenalty: return team + " blocked the ball - possession goes to " + other;
        case EventType::HalfTime: return "End of first half";
        case EventType::MatchOver: return "Game over!";
    }
    return "";
}

const char* EventTitle(const Event& e) {
    switch (e.type) {
        case EventType::LineupStarted:
        case EventType::TurnStarted: return "Advise";
        case EventType::ShotTaken:
        case EventType::SealWarning: return "Warning";
        default: return "Status";
    }
}

const char* ErrorText(ActionType type, ActionError err) {
    switch (err) {
        case ActionError::None: return "";
        case ActionError::WrongPhase:
            if (type == ActionType::Pass) return "No passes during the line-up";
            return "This action is not possible now";
        case ActionError::NotYourPlayer: return "You can not move players of the opposite team!";
        case ActionError::AlreadyMoved: return "This player has already moved";
        case ActionError::OutsideField: return "You can not move players outside of the field!";
        case ActionError::OppositeHalf: return "You can not place players on the opposite team's side!";
        case ActionError::CellOccupied: return "This field is occupied!";
        case ActionError::NoMove:
        case ActionError::Unreachable: return "No move";
        case ActionError::CannotAttack: return "This player can not attack!";
        case ActionError::NoBall:
            if (type == ActionType::Shot) return "This Player can not shoot!";
            return "Your team does not have the ball";
        case ActionError::BallAlreadyPlayed: return "You have already moved the ball";
        case ActionError::BadPassTarget: return "No Pass";
        case ActionError::BadOutcome: return "Impossible outcome";
    }
    return "";
}

}  // namespace ui
}  // namespace soccer
