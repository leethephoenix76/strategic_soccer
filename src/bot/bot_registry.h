// The collection of bots: named configurations of Bot to play against each
// other and against a human (the game's player buttons, ssbot selfplay --white
// NAME --black NAME). The name goes into the game record (tags White / Black),
// so recorded games can be analysed per bot. Do not change what an existing
// name plays - make a new one instead. Bots removed in the cleanup of 26.10.2026
// are listed in docs/archive/bot_inventory.md.
#pragma once

#include "bot/bot.h"

#include <memory>
#include <string>
#include <vector>

namespace soccer {
namespace bot {

struct BotPreset {
    std::string name;          // short, stable: used in records and on the command line
    std::string description;   // one line
    BotOptions options;
};

// All bots, in the order of the game's player buttons.
const std::vector<BotPreset>& BotPresets();
// nullptr if there is no such bot.
const BotPreset* FindBotPreset(const std::string& name);
// The bot the game starts with.
const std::string& DefaultBotName();

std::unique_ptr<Bot> MakeBot(const BotPreset& preset);

}  // namespace bot
}  // namespace soccer
