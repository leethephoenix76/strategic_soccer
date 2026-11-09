#include "ui/renderer.h"

#include "engine/rules.h"
#include "ui/layout.h"
#include "ui/text.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace soccer {
namespace ui {

namespace {

const COLORREF kWhiteColor = RGB(255, 255, 255);
const COLORREF kGrass = RGB(0, 255, 0);
const COLORREF kGoalColor = RGB(255, 255, 0);
const COLORREF kPassZone = RGB(200, 200, 0);
const COLORREF kWhitePlayer = RGB(255, 255, 255), kWhiteMoved = RGB(200, 200, 200);
const COLORREF kBlackPlayer = RGB(0, 0, 0), kBlackMoved = RGB(75, 75, 75);
const COLORREF kDestination = RGB(0, 255, 255);
const COLORREF kSelected = RGB(0, 0, 255);
const COLORREF kBall = RGB(255, 0, 0);

// Rectangle (or ellipse) filled with a solid colour; the brush is released afterwards.
void FillShape(HDC dc, COLORREF color, int left, int top, int right, int bottom, bool ellipse = false) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ old = SelectObject(dc, brush);
    if (ellipse) Ellipse(dc, left, top, right, bottom);
    else Rectangle(dc, left, top, right, bottom);
    SelectObject(dc, old);
    DeleteObject(brush);
}

void FillCell(HDC dc, Cell c, COLORREF color) {
    FillShape(dc, color, c.x * kCellSize, c.y * kCellSize, (c.x + 1) * kCellSize, (c.y + 1) * kCellSize);
}

void DrawBallAt(HDC dc, Cell c, COLORREF color) {
    FillShape(dc, color, c.x * kCellSize + 5, c.y * kCellSize + 5, (c.x + 1) * kCellSize - 5,
              (c.y + 1) * kCellSize - 5, true);
}

void PutText(HDC dc, int x, int y, const char* text) { TextOutA(dc, x, y, text, (int)std::strlen(text)); }

int CellCenter(int v) { return v * kCellSize + kCellSize / 2; }

const COLORREF kDisabled = RGB(225, 225, 225);

void DrawButton(HDC dc, const ButtonRect& b, COLORREF color, const char* label) {
    FillShape(dc, color, b.left, b.top, b.right, b.bottom);
    RECT r{b.left, b.top, b.right, b.bottom};
    DrawTextA(dc, label, -1, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
}

void DrawButtons(HDC dc, const GameState& g, const UiState& ui, const PanelState& panel) {
    const bool play = g.phase == Phase::Play && !panel.replay;
    const bool human = !panel.replay && !panel.bot[g.teamToMove] && g.phase != Phase::Finished;
    const bool holderSelected = human && play && ui.selected != kNoPlayer && ui.selected == g.ball.holder &&
                                TeamOf(ui.selected) == g.teamToMove && !g.ballPlayed;
    for (const ButtonRect& b : kButtons) {
        COLORREF color = kWhiteColor;
        const char* label = b.label;
        std::string text;
        switch (b.id) {
            case Button::Pass: color = !human ? kDisabled : holderSelected ? RGB(0, 255, 0) : kWhiteColor; break;
            case Button::Shot: color = !human ? kDisabled : holderSelected ? RGB(0, 0, 255) : kWhiteColor; break;
            case Button::Attack:
                color = !human ? kDisabled : GetAttackKind(g, ui.selected) != AttackKind::None ? RGB(255, 0, 0) : kWhiteColor;
                break;
            case Button::TakeBack: color = !human ? kDisabled : !g.committed ? RGB(0, 255, 255) : kWhiteColor; break;
            case Button::EndTurn: color = !human ? kDisabled : RGB(150, 150, 150); break;
            case Button::WhitePlayer:
            case Button::BlackPlayer: {
                const int team = b.id == Button::WhitePlayer ? kWhite : kBlack;
                text = std::string(team == kWhite ? "White: " : "Black: ") + (panel.bot[team] ? panel.botName[team] : "human");
                label = text.c_str();
                break;
            }
            case Button::PrevTurn:
            case Button::PrevAction:
            case Button::NextAction:
            case Button::NextTurn:
            case Button::PlayFromHere: color = panel.replay ? RGB(255, 230, 150) : kDisabled; break;
            default: break;
        }
        DrawButton(dc, b, color, label);
    }
}

void DrawPanelTexts(HDC dc, const PanelState& panel) {
    if (panel.replay) PutText(dc, kReplayX, kReplayY, panel.replayPosition.c_str());
    else PutText(dc, kReplayX, kReplayY, "Replay: Open a game");
    int y = kInfoY;
    const std::string* lines[] = {&panel.lastActions, &panel.events, &panel.note};
    for (const std::string* line : lines) {
        if (!line->empty()) {
            RECT r{kInfoX, y, kWindowWidth - 25, y + 2 * kInfoLine};
            DrawTextA(dc, line->c_str(), -1, &r, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
        }
        y += 2 * kInfoLine + 4;
    }
}

void DrawStatus(HDC dc, const GameState& g) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "White - Black %d : %d", g.score[kWhite], g.score[kBlack]);
    PutText(dc, kScoreX, kScoreY, buf);
    PutText(dc, kTeamX, kTeamY, TeamName(g.teamToMove));
    if (g.phase == Phase::Finished) std::snprintf(buf, sizeof buf, "Time %d min - over", g.halfTurns / 2);
    else std::snprintf(buf, sizeof buf, "Time %d min", g.halfTurns / 2);
    PutText(dc, kTimeX, kTimeY, buf);
}

void DrawField(HDC dc) {
    for (int y = kFirstRow; y <= kLastRow; ++y)
        for (int x = 0; x < kFieldWidth; ++x) FillCell(dc, {x, y}, kGrass);
    FillShape(dc, kGoalColor, kGoalLeft * kCellSize, 0, (kGoalRight + 1) * kCellSize, kCellSize);
    FillShape(dc, kGoalColor, kGoalLeft * kCellSize, (kGridHeight - 1) * kCellSize, (kGoalRight + 1) * kCellSize,
              kGridHeight * kCellSize);
}

void DrawPlayers(HDC dc, const GameState& g) {
    for (int k = 0; k < kNumPlayers; ++k) {
        const PlayerState& p = g.players[k];
        COLORREF color = TeamOf(k) == kWhite ? (p.moved ? kWhiteMoved : kWhitePlayer)
                                             : (p.moved ? kBlackMoved : kBlackPlayer);
        FillCell(dc, p.pos, color);
        if (g.phase == Phase::Play) {
            // Inertia: where the player would be if he kept his current velocity.
            Cell from = p.moved ? p.turnStart : p.prevTurnStart;
            MoveToEx(dc, CellCenter(p.pos.x), CellCenter(p.pos.y), nullptr);
            LineTo(dc, CellCenter(2 * p.pos.x - from.x), CellCenter(2 * p.pos.y - from.y));
        }
    }
}

void DrawSelection(HDC dc, const GameState& g, const UiState& ui) {
    if (ui.selected == kNoPlayer) return;
    const PlayerState& p = g.players[ui.selected];
    if (g.phase == Phase::Play && !p.moved && !ui.passMode)
        for (Cell c : MoveDestinations(g, ui.selected)) FillCell(dc, c, kDestination);
    FillCell(dc, p.pos, kSelected);
}

void DrawBall(HDC dc, const GameState& g, const UiState& ui) {
    DrawBallAt(dc, g.ball.pos, kBall);
    std::vector<Cell> path = ui.hasPassPreview ? PredictPassPath(g, ui.passPreview) : PredictBallPath(g);
    int shade = 200;
    for (Cell c : path) {
        DrawBallAt(dc, c, RGB(shade, shade, 0));
        if (shade > 40) shade -= 40;
    }
}

}  // namespace

void Render(HDC dc, const RECT& client, const GameState& game, const UiState& ui, const PanelState& panel) {
    HBRUSH background = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);

    DrawStatus(dc, game);
    DrawButtons(dc, game, ui, panel);
    DrawPanelTexts(dc, panel);
    DrawField(dc);
    if (ui.passMode)
        for (int y = kFirstRow; y <= kLastRow; ++y)
            for (int x = 0; x < kFieldWidth; ++x)
                if (CanPassTo(game, {x, y})) FillCell(dc, {x, y}, kPassZone);
    DrawPlayers(dc, game);
    DrawSelection(dc, game, ui);
    DrawBall(dc, game, ui);
}

}  // namespace ui
}  // namespace soccer
