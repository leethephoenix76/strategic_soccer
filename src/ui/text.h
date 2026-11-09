// Texts shown to the user for engine events and errors.
#pragma once

#include "engine/actions.h"
#include "engine/events.h"

#include <string>

namespace soccer {
namespace ui {

const char* TeamName(int team);

// Message box text and title for an event.
std::string EventText(const Event& e);
const char* EventTitle(const Event& e);

// Message for a rejected action of the given type.
const char* ErrorText(ActionType type, ActionError err);

}  // namespace ui
}  // namespace soccer
