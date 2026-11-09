// Screen layout of the Win32 user interface: field cells and buttons.
#pragma once

#include "engine/types.h"

namespace soccer {
namespace ui {

constexpr int kCellSize = 35;          // pixels per field cell
constexpr int kWindowWidth = 820;
constexpr int kWindowHeight = 800;

enum class Button {
    None,
    // game
    Pass, Shot, Attack, TakeBack, EndTurn,
    // players, files
    WhitePlayer, BlackPlayer, NewGame, Open, Save,
    // replay of a recorded game
    PrevTurn, PrevAction, NextAction, NextTurn, PlayFromHere,
};

struct ButtonRect {
    Button id;
    int left, top, right, bottom;
    const char* label;   // nullptr: the label depends on the state (drawn by the renderer)
};

constexpr ButtonRect kButtons[] = {
    {Button::Pass, 420, 200, 520, 250, "Pass"},
    {Button::Shot, 420, 270, 520, 320, "Shot"},
    {Button::Attack, 420, 340, 520, 390, "Attack"},
    {Button::TakeBack, 420, 410, 520, 460, "Take Back"},
    {Button::EndTurn, 420, 480, 520, 530, "End Turn"},

    {Button::WhitePlayer, 560, 200, 790, 235, nullptr},
    {Button::BlackPlayer, 560, 245, 790, 280, nullptr},
    {Button::NewGame, 560, 300, 790, 335, "New Game"},
    {Button::Open, 560, 345, 670, 380, "Open..."},
    {Button::Save, 680, 345, 790, 380, "Save..."},

    {Button::PrevTurn, 560, 420, 612, 455, "<<"},
    {Button::PrevAction, 619, 420, 671, 455, "<"},
    {Button::NextAction, 678, 420, 730, 455, ">"},
    {Button::NextTurn, 737, 420, 790, 455, ">>"},
    {Button::PlayFromHere, 560, 465, 790, 500, "Play from here"},
};

inline Button HitButton(int px, int py) {
    for (const ButtonRect& b : kButtons)
        if (px >= b.left && px <= b.right && py >= b.top && py <= b.bottom) return b.id;
    return Button::None;
}

inline Cell HitCell(int px, int py) { return {px / kCellSize, py / kCellSize}; }

// Score and status texts.
constexpr int kScoreX = 420, kScoreY = 40;
constexpr int kTeamX = 420, kTeamY = 70;
constexpr int kTimeX = 560, kTimeY = 70;
constexpr int kReplayX = 560, kReplayY = 395;   // "Replay: action 12 / 340"
constexpr int kInfoX = 420, kInfoY = 560;       // status lines below the buttons
constexpr int kInfoLine = 22;

}  // namespace ui
}  // namespace soccer
