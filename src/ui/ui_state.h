// State of the user interface that is not part of the game: selection, pass
// mode, who plays each team, replay, texts of the status lines.
#pragma once

#include "engine/types.h"

#include <string>

namespace soccer {
namespace ui {

struct UiState {
    int selected = kNoPlayer;     // selected player
    bool passMode = false;        // the Pass button was pressed: waiting for the target cell
    bool hasPassPreview = false;  // show the path of the pass being confirmed
    Cell passPreview;
};

// The side panel: players, replay and status lines.
struct PanelState {
    bool bot[kNumTeams] = {false, true};   // the team is played by the bot
    std::string botName[kNumTeams];        // which bot of the collection (bot/bot_registry.h)
    bool replay = false;                   // a recorded game is shown
    std::string replayPosition;            // "Replay: action 12 / 340"
    std::string lastActions;               // the current turn in the game notation
    std::string events;                    // what happened last
    std::string note;                      // what the bot intends
};

}  // namespace ui
}  // namespace soccer
