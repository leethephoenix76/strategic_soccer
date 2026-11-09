// Drawing of the game with Win32 GDI.
#pragma once

#include "engine/state.h"
#include "ui/ui_state.h"

#include <windows.h>

namespace soccer {
namespace ui {

// Draws the whole window contents (field, players, ball, buttons, texts).
void Render(HDC dc, const RECT& client, const GameState& game, const UiState& ui, const PanelState& panel);

}  // namespace ui
}  // namespace soccer
