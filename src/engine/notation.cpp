// Strategic Soccer Notation: cells, actions, outcomes, positions, game records.
#include "notation.h"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <sstream>

namespace soccer {

namespace {

constexpr int kRankBase = kGridHeight - 1;   // rank = 20 - y

std::vector<std::string> SplitWords(const std::string& text) {
    std::vector<std::string> words;
    std::istringstream in(text);
    std::string w;
    while (in >> w) words.push_back(w);
    return words;
}

bool ParseInt(const std::string& text, int* value) {
    if (text.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(text.c_str(), &end, 10);
    if (*end != '\0') return false;
    *value = static_cast<int>(v);
    return true;
}

std::string Signed(int v) { return (v > 0 ? "+" : "") + std::to_string(v); }

void SetError(std::string* error, const std::string& message) {
    if (error) *error = message;
}

}  // namespace

// ---------------------------------------------------------------- cells, players

std::string CellName(Cell c) {
    return std::string(1, static_cast<char>('a' + c.x)) + std::to_string(kRankBase - c.y);
}

bool ParseCell(const std::string& text, Cell* c) {
    if (text.size() < 2 || text[0] < 'a' || text[0] >= 'a' + kFieldWidth) return false;
    int rank;
    if (!ParseInt(text.substr(1), &rank)) return false;
    Cell r{text[0] - 'a', kRankBase - rank};
    if (!InField(r)) return false;
    *c = r;
    return true;
}

std::string PlayerName(int player) {
    return std::string(TeamOf(player) == kWhite ? "W" : "B") + std::to_string(player % kPlayersPerTeam + 1);
}

bool ParsePlayer(const std::string& text, int* player) {
    if (text.size() != 2 || (text[0] != 'W' && text[0] != 'B') || text[1] < '1' || text[1] > '4') return false;
    *player = (text[0] == 'W' ? 0 : kPlayersPerTeam) + (text[1] - '1');
    return true;
}

// ---------------------------------------------------------------- actions, outcomes

std::string FormatAction(const GameState& s, const Action& a) {
    const int first = FirstPlayerOf(s.teamToMove);
    switch (a.type) {
        case ActionType::Place:
        case ActionType::Move: return std::to_string(a.player - first + 1) + CellName(a.target);
        case ActionType::Attack: return std::to_string(a.player - first + 1) + "x";
        case ActionType::Pass: return ">" + CellName(a.target);
        case ActionType::Shot: return "#";
        case ActionType::EndTurn: return "";
    }
    return "";
}

bool ParseAction(const GameState& s, const std::string& token, Action* a) {
    if (token == "#") {
        *a = Action::Shot();
        return true;
    }
    Cell c;
    if (token.size() > 1 && token[0] == '>') {
        if (!ParseCell(token.substr(1), &c)) return false;
        *a = Action::Pass(c);
        return true;
    }
    if (token.size() < 2 || token[0] < '1' || token[0] > '4') return false;
    const int player = FirstPlayerOf(s.teamToMove) + (token[0] - '1');
    if (token == std::string(1, token[0]) + "x") {
        *a = Action::Attack(player);
        return true;
    }
    if (!ParseCell(token.substr(1), &c)) return false;
    *a = s.phase == Phase::Lineup ? Action::Place(player, c) : Action::Move(player, c);
    return true;
}

std::string FormatOutcome(const Outcome& o) {
    switch (o.kind) {
        case OutcomeKind::Certain: return "";
        case OutcomeKind::BallThrough: return "{ok}";
        case OutcomeKind::Intercepted: return "{int " + PlayerName(o.player) + "}";
        case OutcomeKind::Deflected: return "{defl " + PlayerName(o.player) + " " + CellName(o.cell) + "}";
        case OutcomeKind::Won: return "{won}";
        case OutcomeKind::KnockedAway: return "{loose " + CellName(o.cell) + "}";
        case OutcomeKind::Kept: return "{kept}";
        case OutcomeKind::Goal: return "{goal}";
        case OutcomeKind::Caught: return "{caught}";
        case OutcomeKind::Parried: return "{parried " + CellName(o.cell) + "}";
    }
    return "";
}

bool ParseOutcome(const std::string& text, Outcome* o) {
    if (text.size() < 2 || text.front() != '{' || text.back() != '}') return false;
    std::vector<std::string> w = SplitWords(text.substr(1, text.size() - 2));
    if (w.empty()) return false;
    Outcome r;
    const std::string& k = w[0];
    if (k == "ok" && w.size() == 1) r.kind = OutcomeKind::BallThrough;
    else if (k == "won" && w.size() == 1) r.kind = OutcomeKind::Won;
    else if (k == "kept" && w.size() == 1) r.kind = OutcomeKind::Kept;
    else if (k == "goal" && w.size() == 1) r.kind = OutcomeKind::Goal;
    else if (k == "caught" && w.size() == 1) r.kind = OutcomeKind::Caught;
    else if (k == "int" && w.size() == 2) {
        r.kind = OutcomeKind::Intercepted;
        if (!ParsePlayer(w[1], &r.player)) return false;
    } else if (k == "defl" && w.size() == 3) {
        r.kind = OutcomeKind::Deflected;
        if (!ParsePlayer(w[1], &r.player) || !ParseCell(w[2], &r.cell)) return false;
    } else if (k == "loose" && w.size() == 2) {
        r.kind = OutcomeKind::KnockedAway;
        if (!ParseCell(w[1], &r.cell)) return false;
    } else if (k == "parried" && w.size() == 2) {
        r.kind = OutcomeKind::Parried;
        if (!ParseCell(w[1], &r.cell)) return false;
    } else {
        return false;
    }
    *o = r;
    return true;
}

// ---------------------------------------------------------------- positions

std::string FormatPosition(const GameState& s) {
    std::string out = s.teamToMove == kWhite ? "w" : "b";
    out += " " + std::to_string(s.halfTurns) + " " + std::to_string(s.score[kWhite]) + "-" +
           std::to_string(s.score[kBlack]);
    for (int k = 0; k < kNumPlayers; ++k) {
        const PlayerState& p = s.players[k];
        out += " " + CellName(p.turnStart);
        if (p.prevTurnStart != p.turnStart) out += "/" + CellName(p.prevTurnStart);
    }
    const BallState& b = s.ball;
    if (!b.IsFree()) {
        out += " @" + PlayerName(b.holder);
    } else {
        out += " @" + CellName(b.pos);
        if (b.vel.x != 0 || b.vel.y != 0 || b.stepPhase != 0)
            out += "," + Signed(b.vel.x) + "," + Signed(-b.vel.y) + "," + std::to_string(b.stepPhase);
    }
    out += std::string(" t:") + (b.lastTouchTeam == kWhite ? "W" : "B") + (b.lastTouchWasPass ? "p" : "");
    if (b.IsFree() && b.attackTeam != b.lastTouchTeam)
        out += std::string(" a:") + (b.attackTeam == kWhite ? "W" : "B");
    // Inside a turn (07.11.2026): who has moved and where to, who has attacked
    // and in which direction, whether the ball was played and a random event
    // happened. At a turn start none of these is written, so turn starts read
    // as before.
    std::string moved, attacked;
    for (int k = 0; k < kNumPlayers; ++k) {
        const PlayerState& p = s.players[k];
        if (p.moved) moved += (moved.empty() ? "" : ",") + PlayerName(k) + CellName(p.pos);
        else if (p.pos != p.turnStart) moved += (moved.empty() ? "" : ",") + PlayerName(k) + CellName(p.pos) + "!";
        if (p.attacked)
            attacked += (attacked.empty() ? "" : ",") + PlayerName(k) + (p.attackDir.x >= 0 ? "+" : "") +
                        std::to_string(p.attackDir.x) + (-p.attackDir.y >= 0 ? "+" : "") + std::to_string(-p.attackDir.y);
    }
    if (!moved.empty()) out += " m:" + moved;
    if (!attacked.empty()) out += " x:" + attacked;
    if (s.ballPlayed || s.committed)
        out += std::string(" f:") + (s.ballPlayed ? "p" : "") + (s.committed ? "c" : "");
    if (s.phase == Phase::Lineup) out += " l:" + std::to_string(s.lineupStep);   // a restart's line-up
    if (s.sealHistory[kWhite] || s.sealHistory[kBlack])
        out += " s:" + std::to_string(s.sealHistory[kWhite]) + "," + std::to_string(s.sealHistory[kBlack]);
    return out;
}

bool ParsePosition(const std::string& text, GameState* out, std::string* error) {
    std::vector<std::string> w = SplitWords(text);
    if (w.size() < 12 || w.size() > 19) return SetError(error, "position: expected 12 to 19 fields"), false;
    GameState s = NewMatch(0);
    s.phase = Phase::Play;
    s.lineupStep = 2;

    if (w[0] != "w" && w[0] != "b") return SetError(error, "position: side to move must be w or b"), false;
    s.teamToMove = w[0] == "w" ? kWhite : kBlack;
    if (!ParseInt(w[1], &s.halfTurns) || s.halfTurns < 0 || s.halfTurns >= kHalfTurnsPerMatch)
        return SetError(error, "position: bad half-turn count"), false;
    std::size_t dash = w[2].find('-');
    if (dash == std::string::npos || !ParseInt(w[2].substr(0, dash), &s.score[kWhite]) ||
        !ParseInt(w[2].substr(dash + 1), &s.score[kBlack]))
        return SetError(error, "position: bad score"), false;

    for (int k = 0; k < kNumPlayers; ++k) {
        const std::string& t = w[3 + k];
        std::size_t slash = t.find('/');
        Cell pos, prev;
        if (!ParseCell(t.substr(0, slash), &pos)) return SetError(error, "position: bad cell of " + PlayerName(k)), false;
        prev = pos;
        if (slash != std::string::npos && !ParseCell(t.substr(slash + 1), &prev))
            return SetError(error, "position: bad previous cell of " + PlayerName(k)), false;
        if (Abs(pos.x - prev.x) > 2 || Abs(pos.y - prev.y) > 2)
            return SetError(error, "position: " + PlayerName(k) + " moved more than 2 cells"), false;
        for (int j = 0; j < k; ++j)
            if (s.players[j].pos == pos) return SetError(error, "position: two players in " + CellName(pos)), false;
        PlayerState& p = s.players[k];
        p.pos = p.turnStart = pos;
        p.prevTurnStart = prev;
        p.moved = p.attacked = false;
        p.attackDir = {};
    }

    const std::string& bt = w[11];
    if (bt.size() < 2 || bt[0] != '@') return SetError(error, "position: ball must start with @"), false;
    BallState& b = s.ball;
    b = BallState{};
    int holder;
    if (ParsePlayer(bt.substr(1), &holder)) {
        b.holder = holder;
        b.pos = s.players[holder].pos;
        b.lastTouchTeam = TeamOf(holder);
    } else {
        std::vector<std::string> parts;
        std::string part;
        std::istringstream in(bt.substr(1));
        while (std::getline(in, part, ',')) parts.push_back(part);
        if (parts.empty() || !ParseCell(parts[0], &b.pos)) return SetError(error, "position: bad ball cell"), false;
        if (parts.size() != 1 && parts.size() != 4) return SetError(error, "position: ball is @cell or @cell,dx,dy,phase"), false;
        if (parts.size() == 4) {
            int dx, dy;
            if (!ParseInt(parts[1], &dx) || !ParseInt(parts[2], &dy) || !ParseInt(parts[3], &b.stepPhase) ||
                b.stepPhase < 0 || b.stepPhase > 1 || Abs(dx) > kMaxPassDistance || Abs(dy) > kMaxPassDistance)
                return SetError(error, "position: bad ball velocity"), false;
            b.vel = {dx, -dy};
            b.dec = {-Sign(b.vel.x), -Sign(b.vel.y)};
        }
        // A free ball may lie under a player inside a turn: a pass flying over
        // him, a deflection onto his cell (07.11.2026; the engine makes such
        // states, and a position must read back as it was written).
        b.lastTouchTeam = Opponent(s.teamToMove);
    }
    bool hasAttackTeam = false, hasMoved = false, hasAttacked = false, hasFlags = false, hasSeal = false;
    const auto items = [](const std::string& list) {
        std::vector<std::string> parts;
        std::string part;
        std::istringstream in(list);
        while (std::getline(in, part, ',')) parts.push_back(part);
        return parts;
    };
    for (std::size_t i = 12; i < w.size(); ++i) {
        const std::string& t = w[i];
        if (t == "t:W" || t == "t:B" || t == "t:Wp" || t == "t:Bp") {
            b.lastTouchTeam = t[2] == 'W' ? kWhite : kBlack;
            b.lastTouchWasPass = t.size() == 4;
        } else if ((t == "a:W" || t == "a:B") && !hasAttackTeam) {
            b.attackTeam = t[2] == 'W' ? kWhite : kBlack;
            hasAttackTeam = true;
        } else if (t.rfind("m:", 0) == 0 && !hasMoved) {
            // m:W1f12,W3e7 - moved this turn, now there; W2e5! - displaced without moving
            hasMoved = true;
            for (std::string item : items(t.substr(2))) {
                const bool displaced = !item.empty() && item.back() == '!';
                if (displaced) item.pop_back();
                int k;
                Cell c;
                if (item.size() < 4 || !ParsePlayer(item.substr(0, 2), &k) || !ParseCell(item.substr(2), &c))
                    return SetError(error, "position: bad moved player " + item), false;
                s.players[k].pos = c;
                s.players[k].moved = !displaced;
            }
            for (int k = 0; k < kNumPlayers; ++k)
                for (int j = 0; j < k; ++j)
                    if (s.players[j].pos == s.players[k].pos)
                        return SetError(error, "position: two players in " + CellName(s.players[k].pos)), false;
        } else if (t.rfind("x:", 0) == 0 && !hasAttacked) {
            // x:B2+1-1 - attacked this turn towards (dx, dy)
            hasAttacked = true;
            for (const std::string& item : items(t.substr(2))) {
                int k, dx, dy;
                std::size_t at = 2;
                const auto signedNumber = [&](int* v) {
                    if (at >= item.size() || (item[at] != '+' && item[at] != '-')) return false;
                    std::size_t end = at + 1;
                    while (end < item.size() && std::isdigit(static_cast<unsigned char>(item[end]))) ++end;
                    if (end == at + 1 || !ParseInt(item.substr(at, end - at), v)) return false;
                    at = end;
                    return true;
                };
                if (item.size() < 6 || !ParsePlayer(item.substr(0, 2), &k) || !signedNumber(&dx) || !signedNumber(&dy) ||
                    at != item.size())
                    return SetError(error, "position: bad attack " + item), false;
                s.players[k].attacked = true;
                s.players[k].attackDir = {dx, -dy};
            }
        } else if (t.rfind("f:", 0) == 0 && !hasFlags) {
            hasFlags = true;
            for (char c : t.substr(2)) {
                if (c == 'p') s.ballPlayed = true;
                else if (c == 'c') s.committed = true;
                else return SetError(error, "position: bad turn flags"), false;
            }
        } else if ((t == "l:0" || t == "l:1") && s.phase == Phase::Play) {
            s.phase = Phase::Lineup;
            s.lineupStep = t[2] - '0';
        } else if (t.rfind("s:", 0) == 0 && !hasSeal) {
            hasSeal = true;
            const std::vector<std::string> v = items(t.substr(2));
            if (v.size() != 2 || !ParseInt(v[0], &s.sealHistory[kWhite]) || !ParseInt(v[1], &s.sealHistory[kBlack]))
                return SetError(error, "position: bad seal history"), false;
        } else return SetError(error, "position: bad optional field"), false;
    }
    if (!b.IsFree()) b.pos = s.players[b.holder].pos;   // the holder's cell inside a turn
    if (!hasAttackTeam) b.attackTeam = b.IsFree() ? b.lastTouchTeam : TeamOf(b.holder);
    if (!b.IsFree() && b.attackTeam != TeamOf(b.holder))
        return SetError(error, "position: attack team differs from holder"), false;
    *out = s;
    return true;
}

// ---------------------------------------------------------------- games

std::string GameRecord::Tag(const std::string& name) const {
    for (const auto& t : tags)
        if (t.first == name) return t.second;
    return "";
}

void GameRecord::SetTag(const std::string& name, const std::string& value) {
    for (auto& t : tags)
        if (t.first == name) {
            t.second = value;
            return;
        }
    tags.emplace_back(name, value);
}

GameState RecordStart(const GameRecord& record) {
    uint64_t seed = std::strtoull(record.Tag("Seed").c_str(), nullptr, 10);
    GameState s = NewMatch(seed);
    std::string position = record.Tag("Position");
    if (!position.empty() && ParsePosition(position, &s)) s.rng.Seed(seed);
    return s;
}

bool ReplayRecord(GameRecord& record, GameState* final, std::string* error) {
    if (!record.Tag("Position").empty()) {
        GameState tmp;
        std::string why;
        if (!ParsePosition(record.Tag("Position"), &tmp, &why)) {
            SetError(error, why);
            return false;
        }
    }
    GameState s = RecordStart(record);
    for (std::size_t i = 0; i < record.actions.size(); ++i) {
        RecordedAction& r = record.actions[i];
        ActionError err = r.hasOutcome ? ApplyOutcome(s, r.action, r.outcome) : Apply(s, r.action, nullptr, &r.outcome);
        if (err != ActionError::None) {
            SetError(error, "action " + std::to_string(i + 1) + " is not possible");
            if (final) *final = s;
            return false;
        }
        r.hasOutcome = true;
    }
    if (final) *final = s;
    return true;
}

namespace {

std::string TeamLetter(int team) { return team == kWhite ? "W" : "B"; }

std::string Score(const GameState& s) {
    return std::to_string(s.score[kWhite]) + "-" + std::to_string(s.score[kBlack]);
}

// Comments for the events worth a line in the record.
void CommentEvents(const GameState& after, const EventLog& events, std::string& out) {
    for (const Event& e : events) {
        switch (e.type) {
            case EventType::Goal: out += "; goal " + TeamLetter(e.team) + ", " + Score(after) + "\n"; break;
            case EventType::KeeperCaught: out += "; the keeper of " + TeamLetter(e.team) + " caught the ball\n"; break;
            case EventType::KeeperTookBall: out += "; the keeper of " + TeamLetter(e.team) + " took the ball\n"; break;
            case EventType::BallOut: out += "; ball out by " + TeamLetter(e.team) + ", " + TeamLetter(Opponent(e.team)) + " restarts\n"; break;
            case EventType::BackPassPenalty: out += "; back pass to the keeper by " + TeamLetter(e.team) + "\n"; break;
            case EventType::SealWarning: out += "; " + TeamLetter(e.team) + " seals the ball (warning)\n"; break;
            case EventType::SealPenalty: out += "; " + TeamLetter(e.team) + " sealed the ball, possession lost\n"; break;
            case EventType::HalfTime: out += "; half-time, " + Score(after) + "\n"; break;
            case EventType::MatchOver: out += "; match over, " + Score(after) + "\n"; break;
            default: break;
        }
    }
}

// "12. W" for a turn in play (number = half-turn of the match), "L. W" for a line-up.
std::string TurnLabel(const GameState& s) {
    return (s.phase == Phase::Lineup ? std::string("L.") : std::to_string(s.halfTurns + 1) + ".") + " " +
           TeamLetter(s.teamToMove);
}

}  // namespace

std::string WriteRecord(const GameRecord& record) {
    std::string out;
    for (const auto& t : record.tags) out += "[" + t.first + " \"" + t.second + "\"]\n";
    out += "\n";

    GameState s = RecordStart(record);
    std::string line, comments;
    int lineTurn = -1;
    bool lineHasActions = false;
    auto flush = [&]() {
        if (lineTurn < 0) return;
        out += line + (lineHasActions ? "" : " --") + "\n" + comments;
        line.clear();
        comments.clear();
        lineTurn = -1;
        lineHasActions = false;
    };

    for (const RecordedAction& r : record.actions) {
        if (lineTurn != s.turnId) {
            flush();
            line = TurnLabel(s);
            lineTurn = s.turnId;
        }
        const bool random = Outcomes(s, r.action).size() > 1;
        std::string text = r.action.type == ActionType::EndTurn
                               ? (random ? " | " + FormatOutcome(r.outcome) : "")
                               : " " + FormatAction(s, r.action) + (random ? FormatOutcome(r.outcome) : "");
        EventLog events;
        if (ApplyOutcome(s, r.action, r.outcome, &events) != ActionError::None) {
            line += " ; illegal action";
            break;
        }
        line += text;
        if (r.action.type != ActionType::EndTurn) lineHasActions = true;
        CommentEvents(s, events, comments);
    }
    flush();
    return out;
}

namespace {

// Splits a line into tokens; a group in braces belongs to the preceding token.
std::vector<std::string> Tokenize(const std::string& line, std::string* error) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '{') {
            std::size_t close = line.find('}', i);
            if (close == std::string::npos) {
                SetError(error, "unclosed {");
                return {};
            }
            std::string group = line.substr(i, close - i + 1);
            if (tokens.empty()) tokens.push_back(group);
            else tokens.back() += group;
            i = close + 1;
            continue;
        }
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r' && line[j] != '{') ++j;
        tokens.push_back(line.substr(i, j - i));
        i = j;
    }
    return tokens;
}

}  // namespace

bool ReadRecord(const std::string& text, GameRecord* out, std::string* error) {
    GameRecord record;
    std::istringstream in(text);
    std::string raw;
    std::vector<std::pair<int, std::string>> moveLines;
    int lineNo = 0;
    while (std::getline(in, raw)) {
        ++lineNo;
        std::size_t semi = raw.find(';');
        std::string line = semi == std::string::npos ? raw : raw.substr(0, semi);
        std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue;
        if (line[first] == '[') {
            std::size_t q1 = line.find('"', first), q2 = line.rfind('"');
            if (q1 == std::string::npos || q2 <= q1) return SetError(error, "line " + std::to_string(lineNo) + ": bad tag"), false;
            std::string name = SplitWords(line.substr(first + 1, q1 - first - 1)).empty()
                                   ? ""
                                   : SplitWords(line.substr(first + 1, q1 - first - 1))[0];
            record.SetTag(name, line.substr(q1 + 1, q2 - q1 - 1));
            continue;
        }
        moveLines.emplace_back(lineNo, line);
    }

    std::string why;
    if (!record.Tag("Position").empty()) {
        GameState tmp;
        if (!ParsePosition(record.Tag("Position"), &tmp, &why)) return SetError(error, why), false;
    }
    GameState s = RecordStart(record);
    for (const auto& ml : moveLines) {
        const std::string where = "line " + std::to_string(ml.first) + ": ";
        std::vector<std::string> tokens = Tokenize(ml.second, &why);
        if (!why.empty()) return SetError(error, where + why), false;
        if (tokens.size() < 2) return SetError(error, where + "expected '<number>. <W|B> ...'"), false;
        if (tokens[0].empty() || tokens[0].back() != '.') return SetError(error, where + "expected a turn number"), false;
        if (tokens[1] != TeamLetter(s.teamToMove))
            return SetError(error, where + "it is " + TeamLetter(s.teamToMove) + "'s turn"), false;
        if (s.phase == Phase::Finished) return SetError(error, where + "the match is over"), false;

        const int turn = s.turnId;
        Outcome endOutcome;
        bool endHasOutcome = false;
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            std::string tok = tokens[i];
            if (tok == "--") continue;
            if (tok.size() >= 1 && tok[0] == '|') {
                std::string o = tok.substr(1);
                if (o.empty() && i + 1 < tokens.size()) o = tokens[++i];
                if (!ParseOutcome(o, &endOutcome)) return SetError(error, where + "bad outcome after |"), false;
                endHasOutcome = true;
                continue;
            }
            if (s.turnId != turn) return SetError(error, where + "actions after the end of the turn"), false;
            std::size_t brace = tok.find('{');
            std::string actionText = tok.substr(0, brace);
            Action a;
            if (!ParseAction(s, actionText, &a)) return SetError(error, where + "bad action '" + actionText + "'"), false;
            RecordedAction r;
            r.action = a;
            ActionError err;
            if (brace != std::string::npos) {
                if (!ParseOutcome(tok.substr(brace), &r.outcome)) return SetError(error, where + "bad outcome in '" + tok + "'"), false;
                err = ApplyOutcome(s, a, r.outcome);
            } else {
                err = Apply(s, a, nullptr, &r.outcome);   // missing outcome: sample it
            }
            if (err != ActionError::None) return SetError(error, where + "'" + tok + "' is not possible"), false;
            record.actions.push_back(r);
        }
        if (s.turnId == turn) {   // the turn ends with End Turn (unless a restart ended it)
            RecordedAction r;
            r.action = Action::EndTurn();
            ActionError err;
            if (endHasOutcome) {
                r.outcome = endOutcome;
                err = ApplyOutcome(s, r.action, r.outcome);
            } else {
                err = Apply(s, r.action, nullptr, &r.outcome);
            }
            if (err != ActionError::None) return SetError(error, where + "impossible outcome of End Turn"), false;
            record.actions.push_back(r);
        } else if (endHasOutcome) {
            return SetError(error, where + "| after the end of the turn"), false;
        }
    }
    *out = record;
    return true;
}

}  // namespace soccer
