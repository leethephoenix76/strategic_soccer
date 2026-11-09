// Strategic Soccer Notation (SSN): text form of cells, actions, random outcomes,
// positions and whole games - the analogue of chess algebraic notation, FEN and
// PGN. The format is described in docs/notation.md.
//
// Cells: file a..k = x 0..10 (left to right), rank = 20 - y, so White plays
// "up" from its goal line (rank 0) towards Black's goal line (rank 20) like in
// chess. The centre (5, 10) is f10.
#pragma once

#include "rules.h"

#include <string>
#include <utility>
#include <vector>

namespace soccer {

// ---------------------------------------------------------------- cells, players

std::string CellName(Cell c);                    // "f10"
bool ParseCell(const std::string& text, Cell* c);

std::string PlayerName(int player);              // "W1".."W4", "B1".."B4"
bool ParsePlayer(const std::string& text, int* player);

// ---------------------------------------------------------------- actions, outcomes

// One action of the team to move in state `s` (the state before the action).
// Players of the moving team are numbered 1..4:
//   "3f12"  player 3 moves (or, in the line-up, is placed) to f12
//   "2x"    player 2 attacks (tackle or poke)
//   ">h15"  the ball holder passes to h15
//   "#"     the ball holder shoots
// End Turn has no token: it ends the line of the turn.
std::string FormatAction(const GameState& s, const Action& a);
bool ParseAction(const GameState& s, const std::string& token, Action* a);

// Random outcome in braces, appended to the action: "{ok}", "{int B2}",
// "{defl W3 e7}", "{won}", "{loose e7}", "{kept}", "{goal}", "{caught}",
// "{parried f17}". Empty string for a Certain outcome.
std::string FormatOutcome(const Outcome& o);
bool ParseOutcome(const std::string& text, Outcome* o);

// ---------------------------------------------------------------- positions

// A position at the start of a turn in play (like FEN):
//   "<w|b> <half-turns> <score W>-<score B> <W1> <W2> <W3> <W4> <B1> <B2> <B3> <B4> <ball> [t:<W|B>[p]]"
// Player: "f10" (standing) or "f10/f9" (current cell / cell a turn ago - the inertia).
// Ball: "@W2" (held) or "@e7" (free, resting) or "@e7,dx,dy,phase" (rolling; dx
// along the letters, dy along the rank numbers - up is +; phase 1 - the ball waits
// one End Turn before its step). Optional "a:W|B" records the attacking side
// when a free ball's last touch came from the defending side.
// t: the team that touched the ball last, "p" - with a deliberate pass.
std::string FormatPosition(const GameState& s);
bool ParsePosition(const std::string& text, GameState* s, std::string* error = nullptr);

// ---------------------------------------------------------------- games

struct RecordedAction {
    Action action;
    Outcome outcome;        // the applied outcome (Certain if not random)
    bool hasOutcome = true; // false: ReplayRecord samples the outcome (and fills it in)
};

struct GameRecord {
    std::vector<std::pair<std::string, std::string>> tags;   // [Name "value"] header
    std::vector<RecordedAction> actions;                     // every action, including End Turns

    std::string Tag(const std::string& name) const;          // "" if absent
    void SetTag(const std::string& name, const std::string& value);
};

// The start state of a record: the "Position" tag if present, else a new match;
// the "Seed" tag seeds the generator.
GameState RecordStart(const GameRecord& record);

// Applies the actions of a record to its start state. Actions without an outcome
// get a random one from the generator. Returns false (with a message) on an
// illegal action or impossible outcome; `final` gets the state reached so far.
bool ReplayRecord(GameRecord& record, GameState* final, std::string* error = nullptr);

// Text of a game: header tags, then one line per turn, comments ("; ...") for
// goals, restarts, half-time and the end of the match.
std::string WriteRecord(const GameRecord& record);
// Parses the text and replays it; outcomes missing in the text are sampled with
// the generator of the start state and filled in. Returns false with a message
// on a syntax error or an illegal action.
bool ReadRecord(const std::string& text, GameRecord* record, std::string* error = nullptr);

}  // namespace soccer
