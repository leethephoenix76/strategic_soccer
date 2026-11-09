// Basic types and constants of the Strategic Soccer field.
#pragma once

namespace soccer {

// Field geometry. Cells are (x, y): x = 0..10, y = 1..19 are playable.
// Rows y = 0 and y = 20 are the goal lines; the goals occupy x = 4..6 there.
constexpr int kFieldWidth = 11;
constexpr int kFirstRow = 1;
constexpr int kLastRow = 19;
constexpr int kGridHeight = 21;          // rows 0..20, including the goal lines
constexpr int kGoalLeft = 4;
constexpr int kGoalRight = 6;
constexpr int kGoalCenterX = 5;
constexpr int kCenterX = 5;
constexpr int kCenterY = 10;

// Teams and players. Players 0..3 are White, 4..7 are Black.
constexpr int kNumTeams = 2;
constexpr int kPlayersPerTeam = 4;
constexpr int kNumPlayers = kNumTeams * kPlayersPerTeam;
constexpr int kNoPlayer = -1;

constexpr int kWhite = 0;   // attacks the goal at y = 0, lines up in y >= 10
constexpr int kBlack = 1;   // attacks the goal at y = 20, lines up in y <= 10

// Match length: every End Turn in play is half a minute.
constexpr int kHalfTurnsPerHalf = 90;   // 45 minutes
constexpr int kHalfTurnsPerMatch = 180; // 90 minutes

// Rules parameters.
constexpr int kMaxPassDistance = 5;     // per axis (Chebyshev distance)

struct Cell {
    int x = 0;
    int y = 0;
};

inline bool operator==(Cell a, Cell b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(Cell a, Cell b) { return !(a == b); }

inline int TeamOf(int player) { return player / kPlayersPerTeam; }
inline int Opponent(int team) { return team ^ 1; }
inline int FirstPlayerOf(int team) { return team * kPlayersPerTeam; }

inline int Sign(int v) { return (v > 0) - (v < 0); }
inline int Abs(int v) { return v < 0 ? -v : v; }

inline bool InField(int x, int y) {
    return x >= 0 && x < kFieldWidth && y >= kFirstRow && y <= kLastRow;
}
inline bool InField(Cell c) { return InField(c.x, c.y); }

// Chebyshev distance: adjacent cells (including diagonals) are at distance 1.
inline int ChebyshevDistance(Cell a, Cell b) {
    int dx = Abs(a.x - b.x), dy = Abs(a.y - b.y);
    return dx > dy ? dx : dy;
}

// Goal line row the team attacks (0 for White, 20 for Black).
inline int AttackedGoalRow(int team) { return team == kWhite ? 0 : kGridHeight - 1; }

// Goal mouth cells: x = 4..6 on the end rows of the field. Row 1 is in front of
// Black's goal, row 19 in front of White's goal.
inline bool InGoalMouth(int x, int y) {
    return x >= kGoalLeft && x <= kGoalRight && (y == kFirstRow || y == kLastRow);
}
inline int MouthOwner(int row) { return row == kFirstRow ? kBlack : kWhite; }
inline int OwnMouthRow(int team) { return team == kWhite ? kLastRow : kFirstRow; }

}  // namespace soccer
