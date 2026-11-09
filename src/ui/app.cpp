// Win32 application: window, input handling and message boxes.
// The controller turns mouse clicks into engine actions (src/engine/rules.h)
// and shows the resulting events; all game logic lives in the engine.
//
// Each team is played by a human (mouse) or by a bot of the collection
// (bot/bot_registry.h; a click on the team's button goes human -> each bot ->
// human), one action per timer tick so that its moves can be followed; the bot's
// name goes into the game record. Every game is recorded in
// the game notation (docs/notation.md) and can be saved; a saved game can be
// opened and replayed action by action, and play can continue from any moment.
#include "bot/bot_registry.h"
#include "bot/bot.h"
#include "engine/notation.h"
#include "engine/rules.h"
#include "ui/layout.h"
#include "ui/renderer.h"
#include "ui/text.h"
#include "ui/ui_state.h"

#include <windows.h>
#include <commdlg.h>

#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace soccer {
namespace ui {

namespace {

const UINT kMsgShowStart = WM_APP + 1;   // shows the first line-up message after the window appears
const UINT_PTR kBotTimer = 1;
const UINT kMsgBotAction = WM_APP + 2;  // the bot thread has chosen an action
const UINT kBotDelayMs = 300;            // pause between two bot actions
// The bots search in a thread of their own (bot/bot_registry.h: about 20 s per
// search, a search at every decision of a turn), so the window stays alive.
std::shared_ptr<bot::Bot> MakeBot(const std::string& name) {
    const bot::BotPreset* p = bot::FindBotPreset(name);
    if (!p) p = bot::FindBotPreset(bot::DefaultBotName());
    return std::shared_ptr<bot::Bot>(bot::MakeBot(*p).release());
}

// Events worth a message box when the bot plays; the others go to the status line.
bool Important(EventType t) {
    return t == EventType::Goal || t == EventType::HalfTime || t == EventType::MatchOver ||
           t == EventType::SealPenalty || t == EventType::BackPassPenalty || t == EventType::BallOut;
}

class App {
public:
    void SetBlackBot(const std::string& name) { panel_.botName[kBlack] = name; }
    void SetLiveRecordPath(const std::string& path) { liveRecordPath_ = path; }

    void Start(HWND hwnd) {
        hwnd_ = hwnd;
        NewGame();
        SetTimer(hwnd_, kBotTimer, kBotDelayMs, nullptr);
    }

    void ShowStart() {
        if (!panel_.bot[game_.teamToMove]) Show(Event{EventType::LineupStarted, game_.teamToMove});
    }

    void Paint(HDC dc, const RECT& client) {
        // Double buffering: draw into a memory bitmap, then copy it to the window.
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        Render(mem, client, game_, ui_, panel_);
        BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    void OnClick(int px, int py) {
        if (busy_) return;
        busy_ = true;
        Button b = HitButton(px, py);
        switch (b) {
            case Button::WhitePlayer: NextPlayer(kWhite); break;
            case Button::BlackPlayer: NextPlayer(kBlack); break;
            case Button::NewGame: NewGame(); break;
            case Button::Open: OnOpen(); break;
            case Button::Save: OnSave(); break;
            case Button::PrevTurn: ReplayTurn(-1); break;
            case Button::PrevAction: ReplayGoto(replayIndex_ - (replayIndex_ > 0 ? 1 : 0)); break;
            case Button::NextAction: ReplayGoto(replayIndex_ + 1); break;
            case Button::NextTurn: ReplayTurn(+1); break;
            case Button::PlayFromHere: PlayFromHere(); break;
            default:
                if (!panel_.replay && !panel_.bot[game_.teamToMove]) {   // the human's turn
                    switch (b) {
                        case Button::TakeBack: OnTakeBack(); break;
                        case Button::EndTurn: OnEndTurn(); break;
                        case Button::Shot: OnShot(); break;
                        case Button::Pass: OnPass(); break;
                        case Button::Attack: OnAttack(); break;
                        case Button::None: OnCell(HitCell(px, py)); break;
                        default: break;
                    }
                }
                break;
        }
        busy_ = false;
        Redraw();
    }

    // Timer: if it is the bot's turn, it chooses an action in a thread of its own.
    void OnTimer() {
        if (busy_ || thinking_ || panel_.replay || game_.phase == Phase::Finished || !panel_.bot[game_.teamToMove])
            return;
        thinking_ = true;
        const int team = game_.teamToMove;
        std::shared_ptr<bot::Bot> b = bots_[team];
        const GameState state = game_;
        const uint64_t ticket = ++ticket_;
        HWND hwnd = hwnd_;
        panel_.note = std::string(TeamName(team)) + " bot: thinking...";
        Redraw();
        std::thread([b, state, ticket, hwnd] {
            auto* result = new BotResult{ticket, b->NextAction(state), b->Note()};
            if (!PostMessage(hwnd, kMsgBotAction, 0, reinterpret_cast<LPARAM>(result))) delete result;
        }).detach();
    }

    // The bot's action arrives; it is dropped if the game has changed meanwhile.
    void OnBotAction(LPARAM data) {
        std::unique_ptr<BotResult> result(reinterpret_cast<BotResult*>(data));
        if (result->ticket != ticket_) return;   // a new game, a replay or a new bot since
        thinking_ = false;
        if (busy_ || panel_.replay || game_.phase == Phase::Finished || !panel_.bot[game_.teamToMove]) return;
        busy_ = true;
        const int team = game_.teamToMove;
        if (!result->note.empty()) panel_.note = std::string(TeamName(team)) + " bot: " + result->note;
        Perform(result->action, true);
        busy_ = false;
        Redraw();
    }

private:
    struct BotResult {
        uint64_t ticket;
        Action action;
        std::string note;
    };

    // New bots; a search still running finishes on its own and its result is dropped.
    void ResetBots() {
        for (int t = 0; t < kNumTeams; ++t) {
            if (panel_.botName[t].empty()) panel_.botName[t] = bot::DefaultBotName();
            bots_[t] = MakeBot(panel_.botName[t]);
        }
        ++ticket_;
        thinking_ = false;
    }

    // The team's player: human -> each bot of the collection in turn -> human.
    void NextPlayer(int team) {
        const std::vector<bot::BotPreset>& presets = bot::BotPresets();
        int index = -1;
        if (panel_.bot[team])
            for (std::size_t i = 0; i < presets.size(); ++i)
                if (presets[i].name == panel_.botName[team]) index = static_cast<int>(i);
        ++index;
        if (index >= static_cast<int>(presets.size())) {
            panel_.bot[team] = false;
            panel_.note = std::string(TeamName(team)) + ": human";
        } else {
            panel_.bot[team] = true;
            panel_.botName[team] = presets[index].name;
            bots_[team] = MakeBot(presets[index].name);
            panel_.note = std::string(TeamName(team)) + ": bot " + presets[index].name + " - " + presets[index].description;
        }
        ++ticket_;   // a search running for this team (or the other) is dropped and started again
        thinking_ = false;
    }

    std::string PlayerName(int team) const { return panel_.bot[team] ? panel_.botName[team] : "human"; }

    void Redraw() {
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }

    void Show(const Event& e) { MessageBoxA(hwnd_, EventText(e).c_str(), EventTitle(e), MB_OK); }
    void Warn(const char* text) { MessageBoxA(hwnd_, text, "Warning", MB_OK); }

    void NewGame() {
        seed_ = static_cast<uint64_t>(std::time(nullptr));
        game_ = NewMatch(seed_);
        turnStart_ = game_;
        record_ = GameRecord{};
        record_.SetTag("Seed", std::to_string(seed_));
        turnStartRecord_ = 0;
        ui_ = UiState{};
        panel_.replay = false;
        panel_.lastActions.clear();
        panel_.events.clear();
        panel_.note.clear();
        ResetBots();
        WriteLiveRecord();
    }

    // Applies an action; on success records it, redraws and shows what happened.
    bool Perform(const Action& a, bool byBot) {
        EventLog events;
        Outcome outcome;
        const GameState before = game_;
        ActionError err = Apply(game_, a, &events, &outcome);
        if (err != ActionError::None) {
            if (!byBot) Warn(ErrorText(a.type, err));
            return false;
        }
        record_.actions.push_back({a, outcome, true});
        UpdateTurnText(before, a, outcome);
        if (game_.turnId != turnStart_.turnId) {   // a new turn (or a restart) has begun
            turnStart_ = game_;
            turnStartRecord_ = record_.actions.size();
            ui_ = UiState{};
        }
        WriteLiveRecord();
        std::string happened;
        for (const Event& e : events)
            if (e.type != EventType::TurnStarted && e.type != EventType::LineupStarted)
                happened += (happened.empty() ? "" : "; ") + EventText(e);
        if (!happened.empty()) panel_.events = happened;
        Redraw();
        for (const Event& e : events) {
            const bool start = e.type == EventType::TurnStarted || e.type == EventType::LineupStarted;
            if (start && panel_.bot[e.team]) continue;      // the bot does not need to be told
            if (byBot && !start && !Important(e.type)) continue;
            Show(e);
        }
        return true;
    }

    // The status line with the current turn in the game notation.
    void UpdateTurnText(const GameState& before, const Action& a, const Outcome& o) {
        if (before.turnId != lastTextTurn_) {
            lastTextTurn_ = before.turnId;
            panel_.lastActions = (before.phase == Phase::Lineup ? std::string("L.")
                                                                : std::to_string(before.halfTurns + 1) + ".") +
                                 " " + (before.teamToMove == kWhite ? "W" : "B");
        }
        if (a.type == ActionType::EndTurn) {
            if (o.kind != OutcomeKind::Certain && Outcomes(before, a).size() > 1) panel_.lastActions += " | " + FormatOutcome(o);
        } else {
            panel_.lastActions += " " + FormatAction(before, a) + (Outcomes(before, a).size() > 1 ? FormatOutcome(o) : "");
        }
    }

    void OnTakeBack() {
        if (game_.committed) return;   // a pass, shot or attack can not be taken back
        game_ = turnStart_;
        record_.actions.resize(turnStartRecord_);
        lastTextTurn_ = -1;
        panel_.lastActions.clear();
        ui_ = UiState{};
        WriteLiveRecord();
    }

    void OnEndTurn() {
        ui_ = UiState{};
        Perform(Action::EndTurn(), false);
    }

    void OnShot() {
        if (game_.phase != Phase::Play || ui_.selected == kNoPlayer || ui_.selected != game_.ball.holder) {
            Warn("This Player can not shoot!");
            return;
        }
        ui_.passMode = false;
        Perform(Action::Shot(), false);
    }

    void OnPass() {
        ActionError err = Validate(game_, Action::Shot());   // same conditions: own ball, not played yet
        if (err != ActionError::None) {
            Warn(ErrorText(ActionType::Pass, err));
            return;
        }
        ui_.passMode = true;
        ui_.selected = game_.ball.holder;
        Redraw();
        MessageBoxA(hwnd_, "Select target for pass or shot", "Advise", MB_OK);
    }

    void OnAttack() {
        if (ui_.selected == kNoPlayer) {
            Warn("Attacking player is not selected");
            return;
        }
        if (!Perform(Action::Attack(ui_.selected), false)) ui_.selected = kNoPlayer;
    }

    void OnCell(Cell c) {
        if (ui_.passMode) {
            OnPassTarget(c);
            return;
        }
        if (game_.phase == Phase::Finished) return;

        int clicked = InField(c) ? PlayerAt(game_, c) : kNoPlayer;
        if (ui_.selected == kNoPlayer || (clicked != kNoPlayer && clicked != ui_.selected)) {
            ui_.selected = clicked;   // select a player (or switch the selection)
            return;
        }
        int player = ui_.selected;
        ui_.selected = kNoPlayer;
        if (game_.players[player].moved) return;   // already moved: just deselect
        if (game_.phase == Phase::Lineup) Perform(Action::Place(player, c), false);
        else Perform(Action::Move(player, c), false);
    }

    void OnPassTarget(Cell c) {
        ui_.passMode = false;
        if (!CanPassTo(game_, c)) {
            Warn("No Pass");
            return;
        }
        ui_.hasPassPreview = true;
        ui_.passPreview = c;
        Redraw();
        const char* question = PassReachesOwnKeeper(game_, c)
            ? "The ball will roll to your own keeper - possession will go to the opponent! Pass anyway?"
            : "Confirm action";
        int answer = MessageBoxA(hwnd_, question, "Are you sure?", MB_OKCANCEL);
        ui_.hasPassPreview = false;
        if (answer == IDOK && Perform(Action::Pass(c), false)) ui_.selected = kNoPlayer;
    }

    // ------------------------------------------------------------- files

    GameRecord CurrentRecord() const {
        GameRecord rec = record_;
        char date[16];
        std::time_t now = std::time(nullptr);
        std::strftime(date, sizeof date, "%Y.%m.%d", std::localtime(&now));
        rec.SetTag("Event", "game");
        rec.SetTag("Date", date);
        rec.SetTag("White", PlayerName(kWhite));
        rec.SetTag("Black", PlayerName(kBlack));
        rec.SetTag("Result", game_.phase == Phase::Finished
                                 ? std::to_string(game_.score[kWhite]) + "-" + std::to_string(game_.score[kBlack])
                                 : "*");
        return rec;
    }

    void WriteLiveRecord() const {
        if (liveRecordPath_.empty()) return;
        const std::string temporary = liveRecordPath_ + ".tmp";
        {
            std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << WriteRecord(CurrentRecord());
            if (!f) return;
        }
        MoveFileExA(temporary.c_str(), liveRecordPath_.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    bool FileDialog(bool save, std::string& path) {
        char file[MAX_PATH] = "";
        if (save) std::snprintf(file, sizeof file, "game.ssn");
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = "Strategic Soccer games (*.ssn)\0*.ssn\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = sizeof file;
        ofn.lpstrDefExt = "ssn";
        ofn.Flags = save ? OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR : OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (!(save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn))) return false;
        path = file;
        return true;
    }

    void OnSave() {
        GameRecord rec = panel_.replay ? replayRecord_ : CurrentRecord();
        std::string path;
        if (!FileDialog(true, path)) return;
        std::ofstream f(path);
        f << WriteRecord(rec);
        if (!f) Warn("Can not write the file");
    }

    void OnOpen() {
        std::string path;
        if (!FileDialog(false, path)) return;
        std::ifstream f(path);
        std::stringstream text;
        text << f.rdbuf();
        GameRecord rec;
        std::string error;
        if (!ReadRecord(text.str(), &rec, &error)) {
            Warn(("Can not read the game: " + error).c_str());
            return;
        }
        replayRecord_ = rec;
        replayStates_.clear();
        GameState s = RecordStart(rec);
        replayStates_.push_back(s);
        for (const RecordedAction& r : rec.actions) {
            ApplyOutcome(s, r.action, r.outcome);
            replayStates_.push_back(s);
        }
        panel_.replay = true;
        panel_.note.clear();
        panel_.events = rec.Tag("White") + " - " + rec.Tag("Black") + ", result " + rec.Tag("Result");
        ui_ = UiState{};
        ReplayGoto(0);
    }

    // ------------------------------------------------------------- replay

    void ReplayGoto(std::size_t index) {
        if (!panel_.replay) return;
        if (index >= replayStates_.size()) index = replayStates_.size() - 1;
        replayIndex_ = index;
        game_ = replayStates_[index];
        panel_.replayPosition = "Replay: action " + std::to_string(index) + " / " + std::to_string(replayStates_.size() - 1);
        // The current turn in the notation, up to this action.
        std::size_t start = TurnStartIndex(index);
        lastTextTurn_ = -1;
        panel_.lastActions.clear();
        for (std::size_t i = start; i < index; ++i) {
            const RecordedAction& r = replayRecord_.actions[i];
            UpdateTurnText(replayStates_[i], r.action, r.outcome);
        }
        if (index > 0 && replayIndex_ < replayStates_.size()) {
            // Show the events of the last action in the status line.
            GameState s = replayStates_[index - 1];
            EventLog events;
            ApplyOutcome(s, replayRecord_.actions[index - 1].action, replayRecord_.actions[index - 1].outcome, &events);
            std::string happened;
            for (const Event& e : events)
                if (e.type != EventType::TurnStarted && e.type != EventType::LineupStarted)
                    happened += (happened.empty() ? "" : "; ") + EventText(e);
            if (!happened.empty()) panel_.events = happened;
        }
    }

    // The first action index of the turn that is current at `index`.
    std::size_t TurnStartIndex(std::size_t index) const {
        std::size_t i = index;
        while (i > 0 && replayStates_[i - 1].turnId == replayStates_[index].turnId) --i;
        return i;
    }

    void ReplayTurn(int direction) {
        if (!panel_.replay) return;
        std::size_t i = replayIndex_;
        if (direction < 0) {
            std::size_t start = TurnStartIndex(i);
            i = start == i && i > 0 ? TurnStartIndex(i - 1) : start;
        } else {
            while (i + 1 < replayStates_.size() && replayStates_[i + 1].turnId == replayStates_[replayIndex_].turnId) ++i;
            if (i + 1 < replayStates_.size()) ++i;
        }
        ReplayGoto(i);
    }

    // Leaves the replay: the game goes on from the shown moment.
    void PlayFromHere() {
        if (!panel_.replay) return;
        std::size_t start = TurnStartIndex(replayIndex_);
        record_ = replayRecord_;
        record_.actions.resize(replayIndex_);
        game_ = replayStates_[replayIndex_];
        turnStart_ = replayStates_[start];
        turnStartRecord_ = start;
        panel_.replay = false;
        ui_ = UiState{};
        ResetBots();
        WriteLiveRecord();
    }

    HWND hwnd_ = nullptr;
    GameState game_;
    GameState turnStart_;   // for Take Back: the state at the start of the current turn
    UiState ui_;
    PanelState panel_;
    bool busy_ = false;     // an action is being performed (a message box may be open)
    uint64_t seed_ = 0;
    std::string liveRecordPath_;
    std::shared_ptr<bot::Bot> bots_[kNumTeams];
    bool thinking_ = false;   // a bot thread is choosing an action
    uint64_t ticket_ = 0;     // of the current bot request
    GameRecord record_;
    std::size_t turnStartRecord_ = 0;
    int lastTextTurn_ = -1;
    GameRecord replayRecord_;
    std::vector<GameState> replayStates_;
    std::size_t replayIndex_ = 0;
};

App g_app;

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            g_app.Start(hwnd);
            PostMessage(hwnd, kMsgShowStart, 0, 0);
            return 0;
        case kMsgShowStart:
            g_app.ShowStart();
            return 0;
        case WM_TIMER:
            if (wParam == kBotTimer) g_app.OnTimer();
            return 0;
        case kMsgBotAction:
            g_app.OnBotAction(lParam);
            return 0;
        case WM_ERASEBKGND:
            return 1;   // everything is painted in WM_PAINT
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            g_app.Paint(dc, client);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            g_app.OnClick(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kBotTimer);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(hwnd, message, wParam, lParam);
}

}  // namespace
}  // namespace ui
}  // namespace soccer

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR commandLine, int showCommand) {
    // The dedicated live-play executable can be opened directly by a desktop launcher.
    char executable[MAX_PATH] = {};
    const DWORD executableLength = GetModuleFileNameA(nullptr, executable, MAX_PATH);
    if (executableLength > 0 && executableLength < MAX_PATH) {
        const std::string path(executable, executableLength);
        const std::size_t separator = path.find_last_of("\\/");
        if (path.substr(separator == std::string::npos ? 0 : separator + 1) == "soccer_live.exe") {
            soccer::ui::g_app.SetBlackBot(soccer::bot::DefaultBotName());
            soccer::ui::g_app.SetLiveRecordPath(path.substr(0, separator == std::string::npos ? 0 : separator + 1) +
                                                "live_game.ssn");
        }
    }
    std::istringstream args(commandLine ? commandLine : "");
    std::string option, preset, unknownBot;
    while (args >> option) {
        if (option == "--black" && args >> preset) {
            if (soccer::bot::FindBotPreset(preset)) soccer::ui::g_app.SetBlackBot(preset);
            else unknownBot = preset;
        } else if (option == "--live-record" && args >> preset)
            soccer::ui::g_app.SetLiveRecordPath(preset);
    }
    const char* className = "StrategicSoccer";
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = soccer::ui::WndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = className;
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);
    if (!RegisterClassExA(&wc)) {
        MessageBoxA(nullptr, "Call to RegisterClassEx failed!", "Strategic Soccer", MB_OK);
        return 1;
    }

    HWND hwnd = CreateWindowExA(0, className, "Strategic Soccer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                soccer::ui::kWindowWidth, soccer::ui::kWindowHeight, nullptr, nullptr, instance,
                                nullptr);
    if (!hwnd) {
        MessageBoxA(nullptr, "Call to CreateWindow failed!", "Strategic Soccer", MB_OK);
        return 1;
    }
    // A bot asked for on the command line that this build does not know (an old
    // soccer.exe, a failed rebuild): say so instead of silently playing another one.
    if (!unknownBot.empty()) {
        const std::string text = "Unknown bot: " + unknownBot +
                                 "\nThis soccer.exe is older than the bot collection (or the last build failed:"
                                 " close the game and run build_msvc.bat again).\nCheck the bot on Black's button before playing.";
        MessageBoxA(nullptr, text.c_str(), "Strategic Soccer", MB_OK | MB_ICONWARNING);
    }
    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}
