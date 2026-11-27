// Win32 图形界面：负责窗口、DPI 缩放、双缓冲绘制、按钮表驱动与三种棋的
// 多态分派。对局状态封装在 GameSession，三种棋通过 GameBase 接口统一驱动。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>

#include "gomoku/classic/game.hpp"
#include "gomoku/core/account.hpp"
#include "gomoku/six_player/ai.hpp"
#include "gomoku/six_player/game.hpp"
#include "gomoku/three_player/ai.hpp"
#include "gomoku/three_player/game.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"GomokuNativeWindow";
constexpr wchar_t kWindowTitle[] = L"棋魂";
constexpr COLORREF kBackground{RGB(17, 24, 39)};
constexpr COLORREF kPanel{RGB(29, 39, 57)};
constexpr COLORREF kPanelLight{RGB(38, 51, 72)};
constexpr COLORREF kText{RGB(239, 244, 250)};
constexpr COLORREF kMuted{RGB(157, 173, 194)};
constexpr COLORREF kAccent{RGB(82, 211, 177)};
constexpr COLORREF kAccentDark{RGB(35, 151, 123)};
constexpr COLORREF kBoardWood{RGB(223, 179, 111)};
constexpr COLORREF kGrid{RGB(94, 68, 43)};

using gomoku::Board;
using gomoku::Game;
using gomoku::GameBase;
using gomoku::Move;
using gomoku::SixPlayerAi;
using gomoku::SixPlayerGame;
using gomoku::Stone;
using gomoku::ThreePlayerAi;
using gomoku::ThreePlayerGame;

struct Rect {
    int left = 0, top = 0, right = 0, bottom = 0;
    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool contains(POINT p) const { return p.x >= left && p.x < right && p.y >= top && p.y < bottom; }
};

enum class Screen { Login, Menu, Playing, Finished };
enum class MenuPage { GameSelection, ModeSelection };
enum class GameType { Gomoku, ThreePlayer, SixPlayer };
enum class ButtonAction { None, Login, PvP, PvC, SelectGomoku, SelectThreePlayer, SelectSixPlayer, Undo, Restart, MainMenu, Exit };

struct Layout {
    Rect boardCard{}, boardGrid{}, sidePanel{}, notationList{}, menuExit{}, menuPanel{};
    std::array<Rect, 3> buttons{}, menuButtons{};
    UINT dpi = 96;
    int boardSize = gomoku::kBoardSize;
};

struct ButtonSpec {
    ButtonAction action;
    Rect rect;
    const wchar_t* label;
    bool danger = false;
    bool enabled = true;
};

// 单次对局的完整运行快照：保存与悔棋只需整体拷贝/恢复此结构体。
struct GameSession {
    GameSession() = default;
    Screen screen = Screen::Menu;
    GameType gameType = GameType::Gomoku;
    bool pvc = false;
    Game game;
    ThreePlayerGame threeGame{true};
    SixPlayerGame sixGame;
    std::optional<Move> lastMove;
    int notationScroll = 0;
};

struct AppState {
    MenuPage menuPage = MenuPage::GameSelection;
    GameType selectedGame = GameType::Gomoku;
    UINT dpi = 96;
    GameSession session;
    gomoku::AiPlayer ai;
    ThreePlayerAi threeAi;
    SixPlayerAi sixAi;
    std::vector<GameSession> undoHistory;
    ButtonAction hovered = ButtonAction::None;

    gomoku::AccountManager account;
    std::wstring loginName, loginPass;
    int loginField = 0;
    std::wstring loginMessage = L"新用户名会自动创建账号。";
    bool loginError = false;
    bool resultSaved = false;

    AppState() {
        if (!account.load()) {
            loginMessage = L"无法访问 data 目录。";
            loginError = true;
        }
        session.screen = Screen::Login;
    }

    GameBase& activeGame() {
        if (session.gameType == GameType::SixPlayer) return session.sixGame;
        if (session.gameType == GameType::ThreePlayer) return session.threeGame;
        return session.game;
    }
    const GameBase& activeGame() const { return const_cast<AppState*>(this)->activeGame(); }
    int boardSize() const {
        return session.gameType == GameType::SixPlayer ? gomoku::kSixPlayerBoardSize : gomoku::kBoardSize;
    }
};

std::string toUtf8(const std::wstring& text) {
    char buffer[256]{};
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, buffer, sizeof(buffer), nullptr, nullptr);
    return buffer;
}

std::wstring fromUtf8(const std::string& text) {
    wchar_t buffer[256]{};
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, buffer, 256);
    return buffer;
}

int px(int logicalSize, UINT dpi) { return MulDiv(logicalSize, static_cast<int>(dpi), 96); }
Rect makeRect(int left, int top, int width, int height) { return {left, top, left + width, top + height}; }
int gridPixel(int origin, int index, int extent, int boardSize) { return origin + MulDiv(index, extent, boardSize - 1); }

void fillRect(HDC dc, Rect r, COLORREF color) {
    const RECT native{r.left, r.top, r.right, r.bottom};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &native, brush);
    DeleteObject(brush);
}

template <typename DrawFn>
void drawShape(HDC dc, COLORREF fill, COLORREF outline, UINT dpi, DrawFn fn) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, px(1, dpi)), outline);
    const HGDIOBJ oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    fn();
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void drawRoundRect(HDC dc, Rect r, COLORREF fill, COLORREF outline, int radius, UINT dpi) {
    const int sr = px(radius, dpi);
    drawShape(dc, fill, outline, dpi, [&] { RoundRect(dc, r.left, r.top, r.right, r.bottom, sr, sr); });
}

void drawCircle(HDC dc, int x, int y, int r, COLORREF fill, COLORREF outline, UINT dpi) {
    drawShape(dc, fill, outline, dpi, [&] { Ellipse(dc, x - r, y - r, x + r + 1, y + r + 1); });
}

void drawText(HDC dc, const std::wstring& text, Rect r, int logicalSize, COLORREF color,
              UINT dpi, UINT alignment = DT_LEFT, int weight = FW_NORMAL) {
    HFONT font = CreateFontW(-std::max(1, px(logicalSize, dpi)), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (font == nullptr) return;
    const HGDIOBJ oldFont = SelectObject(dc, font);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);
    RECT native{r.left, r.top, r.right, r.bottom};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &native,
              alignment | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

void drawButton(HDC dc, const ButtonSpec& b, ButtonAction hovered, UINT dpi) {
    const bool isHovered = b.enabled && b.action == hovered;
    COLORREF fill = kPanelLight, outline = RGB(61, 77, 101), textColor = kText;
    if (!b.enabled) {
        fill = kPanel;
        outline = RGB(48, 61, 79);
        textColor = RGB(104, 119, 139);
    } else if (b.danger) {
        fill = isHovered ? RGB(151, 61, 67) : RGB(116, 50, 59);
        outline = RGB(196, 87, 91);
    } else if (isHovered) {
        fill = RGB(52, 70, 97);
        outline = RGB(116, 134, 158);
    }
    drawRoundRect(dc, b.rect, fill, outline, 14, dpi);
    drawText(dc, b.label, b.rect, 19, textColor, dpi, DT_CENTER, FW_SEMIBOLD);
}

void drawTextField(HDC dc, Rect r, const wchar_t* hint, const std::wstring& value,
                   bool active, UINT dpi) {
    drawRoundRect(dc, r, active ? RGB(43, 58, 80) : kPanelLight,
                  active ? kAccent : RGB(61, 77, 101), 12, dpi);
    std::wstring text = value.empty() ? std::wstring(hint) : value;
    if (active) text += L"|";
    drawText(dc, text, {r.left + px(16, dpi), r.top, r.right - px(16, dpi), r.bottom},
             18, value.empty() ? RGB(118, 134, 154) : kText, dpi, DT_LEFT, FW_SEMIBOLD);
}

// 计算当前页面所有可交互按钮，统一供绘制、命中测试与局部刷新使用。
std::vector<ButtonSpec> activeButtons(const AppState& app, const Layout& layout) {
    if (app.session.screen == Screen::Login) {
        return {{ButtonAction::Login, layout.menuButtons[2], L"进入游戏"},
                {ButtonAction::Exit, layout.menuExit, L"退出游戏", true}};
    }
    if (app.session.screen == Screen::Menu) {
        if (app.menuPage == MenuPage::GameSelection) {
            return {{ButtonAction::SelectGomoku, layout.menuButtons[0], L"五子棋"},
                    {ButtonAction::SelectThreePlayer, layout.menuButtons[1], L"三人四子棋"},
                    {ButtonAction::SelectSixPlayer, layout.menuButtons[2], L"六子棋"},
                    {ButtonAction::Exit, layout.menuExit, L"退出游戏", true}};
        }
        return {{ButtonAction::PvP, layout.menuButtons[0], L"玩家对战  ·  PvP"},
                {ButtonAction::PvC, layout.menuButtons[1], L"人机对战  ·  PvC"},
                {ButtonAction::MainMenu, layout.menuButtons[2], L"返回主菜单", true}};
    }
    return {{ButtonAction::Undo, layout.buttons[0], L"悔棋", false, !app.undoHistory.empty()},
            {ButtonAction::Restart, layout.buttons[1], app.session.screen == Screen::Finished ? L"再来一局" : L"重新开始"},
            {ButtonAction::MainMenu, layout.buttons[2], L"返回主菜单", true}};
}

ButtonAction hitButton(const AppState& app, const Layout& layout, POINT point) {
    for (const ButtonSpec& btn : activeButtons(app, layout)) {
        if (btn.enabled && btn.rect.contains(point)) return btn.action;
    }
    return ButtonAction::None;
}

// 参数：客户区宽高、当前 DPI、棋盘路数、当前菜单页。
// 返回：按逻辑像素计算、再经 DPI 缩放的各区域矩形。
Layout calculateLayout(int width, int height, UINT dpi, int boardSize, MenuPage menuPage) {
    Layout l;
    l.dpi = dpi;
    l.boardSize = boardSize;
    const bool gamePage = menuPage == MenuPage::GameSelection;
    const int btnW = std::min(px(330, dpi), std::max(px(240, dpi), width / 3)), btnH = px(50, dpi);
    const int menuLeft = width / 2 - btnW / 2, pitch = px(58, dpi);
    const int menuTop = (height - (pitch * (gamePage ? 3 : 2) + btnH)) / 2;
    for (int i = 0; i < 3; ++i) {
        l.menuButtons[static_cast<std::size_t>(i)] = makeRect(menuLeft, menuTop + i * pitch, btnW, btnH);
    }
    if (gamePage) l.menuExit = makeRect(menuLeft, menuTop + 3 * pitch, btnW, btnH);
    const int panelTop = menuTop - px(44, dpi);
    const int panelBottom = (gamePage ? l.menuExit.bottom : l.menuButtons[2].bottom) + px(44, dpi);
    l.menuPanel = makeRect(width / 2 - px(260, dpi), panelTop, px(520, dpi), panelBottom - panelTop);

    const int margin = std::max(px(22, dpi), std::min(width, height) / 30), headerH = px(104, dpi);
    const int boardPx = std::max(px(300, dpi), std::min({
        std::max(px(300, dpi), height - px(150, dpi)),
        std::max(px(300, dpi), width - px(390, dpi)),
        px(36 * (boardSize - 1) + 72, dpi)}));
    l.boardCard = makeRect(margin, headerH, boardPx, boardPx);
    const int labelM = std::max(px(36, dpi), boardPx / 16);
    l.boardGrid = {l.boardCard.left + labelM, l.boardCard.top + labelM,
                   l.boardCard.right - labelM, l.boardCard.bottom - labelM};

    const int panelX = l.boardCard.right + margin;
    const int panelW = std::max(px(260, dpi), width - panelX - margin);
    l.sidePanel = makeRect(panelX, headerH, panelW, boardPx);
    const int sideBtnH = std::max(px(40, dpi), std::min(px(48, dpi), l.sidePanel.height() / 11));
    const int gap = px(7, dpi), buttonY = l.sidePanel.bottom - sideBtnH * 3 - gap * 2 - px(18, dpi);
    l.notationList = {panelX + px(20, dpi), l.sidePanel.top + px(116, dpi), panelX + panelW - px(20, dpi), buttonY - px(14, dpi)};
    for (int i = 0; i < 3; ++i) {
        l.buttons[static_cast<std::size_t>(i)] =
            makeRect(panelX + px(18, dpi), buttonY + i * (sideBtnH + gap), panelW - px(36, dpi), sideBtnH);
    }
    return l;
}

Layout currentLayout(HWND window, const AppState& app) {
    RECT c{};
    GetClientRect(window, &c);
    return calculateLayout(c.right - c.left, c.bottom - c.top, app.dpi, app.boardSize(), app.menuPage);
}

const wchar_t* stoneLabel(Stone s) {
    return s == Stone::Black ? L"黑" : (s == Stone::White ? L"白" : (s == Stone::Red ? L"红" : L""));
}

const wchar_t* threePlayerLabel(Stone s) {
    return s == Stone::Black ? L"玩家1（黑棋）" : (s == Stone::White ? L"玩家2（白棋）" : L"玩家3（红棋）");
}

std::wstring accountText(const AppState& app) {
    if (!app.account.loggedIn()) return L"";
    const auto stats = app.account.currentStats();
    return L"用户：" + fromUtf8(app.account.currentUsername()) + L" ｜ 胜 " +
           std::to_wstring(stats->wins) + L" 负 " + std::to_wstring(stats->losses) +
           L" 平 " + std::to_wstring(stats->draws);
}

std::wstring playerText(const AppState& app) {
    const GameSession& s = app.session;
    const GameBase& game = app.activeGame();
    const bool three = s.gameType == GameType::ThreePlayer, six = s.gameType == GameType::SixPlayer;
    if (s.screen == Screen::Finished) {
        const auto w = game.winner();
        if (!w) return L"平局，再来一局？";
        if (s.pvc && *w == Stone::Black) return L"你赢了！";
        if (three) return s.pvc ? (*w == Stone::White ? L"电脑1（白棋）获胜！" : L"电脑2（红棋）获胜！")
                                : std::wstring(threePlayerLabel(*w)) + L"获胜！";
        if (s.pvc) return six ? L"电脑获胜！" : L"电脑获胜";
        return *w == Stone::Black ? (six ? L"黑方获胜！" : L"黑棋获胜！") : (six ? L"白方获胜！" : L"白棋获胜！");
    }
    const Stone turn = game.currentPlayer();
    if (three) {
        return s.pvc ? (turn == Stone::Black ? L"轮到你（黑棋）" : (turn == Stone::White ? L"电脑1思考中（白棋）" : L"电脑2思考中（红棋）"))
                     : std::wstring(L"轮到") + threePlayerLabel(turn);
    }
    if (six) {
        const std::wstring base = s.pvc ? (turn == Stone::Black ? L"轮到你（黑方）" : L"电脑思考中（白方）")
                                        : (turn == Stone::Black ? L"轮到黑方" : L"轮到白方");
        return base + L"，本回合还需落 " + std::to_wstring(s.sixGame.movesRemaining()) + L" 子";
    }
    return s.pvc ? (turn == Stone::Black ? L"轮到你（黑棋）" : L"电脑思考中（白棋）")
                 : (turn == Stone::Black ? L"轮到黑棋" : L"轮到白棋");
}

void drawBoard(HDC dc, const AppState& app, const Layout& l) {
    const UINT dpi = l.dpi;
    drawRoundRect(dc, l.boardCard, kBoardWood, RGB(160, 119, 70), 18, dpi);
    const int left = l.boardGrid.left, top = l.boardGrid.top, right = l.boardGrid.right, bottom = l.boardGrid.bottom;
    const int w = l.boardGrid.width(), h = l.boardGrid.height();
    const int radius = std::max(px(4, dpi), static_cast<int>((w / (l.boardSize - 1)) * 0.38));

    HPEN gridPen = CreatePen(PS_SOLID, std::max(1, px(1, dpi)), kGrid);
    const HGDIOBJ oldPen = SelectObject(dc, gridPen);
    for (int i = 0; i < l.boardSize; ++i) {
        const int x = gridPixel(left, i, w, l.boardSize), y = gridPixel(top, i, h, l.boardSize);
        MoveToEx(dc, left, y, nullptr);
        LineTo(dc, right + 1, y);
        MoveToEx(dc, x, top, nullptr);
        LineTo(dc, x, bottom + 1);
        const wchar_t colLabel[] = {static_cast<wchar_t>(L'A' + i), L'\0'};
        drawText(dc, colLabel, {x - px(12, dpi), l.boardCard.top + px(8, dpi), x + px(12, dpi), top - px(2, dpi)},
                 11, RGB(109, 78, 47), dpi, DT_CENTER, FW_SEMIBOLD);
        drawText(dc, std::to_wstring(i + 1), {l.boardCard.left + px(4, dpi), y - px(9, dpi), left - px(5, dpi), y + px(10, dpi)},
                 11, RGB(109, 78, 47), dpi, DT_CENTER, FW_SEMIBOLD);
    }
    SelectObject(dc, oldPen);
    DeleteObject(gridPen);

    const std::array<int, 3> stars = l.boardSize == 19 ? std::array<int, 3>{{3, 9, 15}} : std::array<int, 3>{{3, 7, 11}};
    for (int row : stars) {
        for (int col : stars) {
            drawCircle(dc, gridPixel(left, col, w, l.boardSize), gridPixel(top, row, h, l.boardSize),
                       std::max(2, px(3, dpi)), kGrid, kGrid, dpi);
        }
    }

    const Board& board = app.activeGame().board();
    for (int row = 0; row < l.boardSize; ++row) {
        for (int col = 0; col < l.boardSize; ++col) {
            const Move move{row, col};
            const Stone stone = board.at(move);
            if (stone == Stone::Empty) continue;
            const int x = gridPixel(left, col, w, l.boardSize), y = gridPixel(top, row, h, l.boardSize);
            if (app.session.lastMove && *app.session.lastMove == move) {
                drawCircle(dc, x, y, radius + px(5, dpi), kAccentDark, kAccentDark, dpi);
            }
            const COLORREF fill = stone == Stone::Black ? RGB(25, 31, 41) : (stone == Stone::Red ? RGB(198, 64, 64) : RGB(247, 247, 239));
            const COLORREF out = stone == Stone::Black ? RGB(8, 12, 18) : (stone == Stone::Red ? RGB(122, 32, 36) : RGB(185, 186, 177));
            drawCircle(dc, x, y, radius, fill, out, dpi);
        }
    }
}

int notationVisibleLines(const Layout& layout) {
    const int lineHeight = std::max(1, px(24, layout.dpi));
    return std::max(0, (layout.notationList.height() - px(46, layout.dpi) - px(8, layout.dpi)) / lineHeight);
}

void drawSidePanel(HDC dc, const AppState& app, const Layout& l) {
    const UINT dpi = l.dpi;
    const int left = l.sidePanel.left, top = l.sidePanel.top, width = l.sidePanel.width();
    drawRoundRect(dc, l.sidePanel, kPanel, RGB(48, 64, 87), 20, dpi);
    drawText(dc, L"当前对局", makeRect(left + px(22, dpi), top + px(18, dpi), width - px(44, dpi), px(28, dpi)),
             15, kMuted, dpi, DT_LEFT, FW_SEMIBOLD);
    drawText(dc, playerText(app), makeRect(left + px(22, dpi), top + px(54, dpi), width - px(44, dpi), px(48, dpi)),
             23, app.session.screen == Screen::Finished ? kAccent : kText, dpi, DT_LEFT, FW_BOLD);

    drawRoundRect(dc, l.notationList, kPanelLight, RGB(61, 77, 101), 14, dpi);
    drawText(dc, L"落子记录", makeRect(l.notationList.left + px(14, dpi), l.notationList.top + px(8, dpi),
                                       l.notationList.width() - px(28, dpi), px(26, dpi)),
             14, kMuted, dpi, DT_LEFT, FW_SEMIBOLD);

    const int lineH = std::max(1, px(24, dpi)), listTop = l.notationList.top + px(38, dpi), visible = notationVisibleLines(l);
    const auto& records = app.activeGame().moveHistory();
    const int scroll = std::clamp(app.session.notationScroll, 0, std::max(0, static_cast<int>(records.size()) - visible));
    if (records.empty()) {
        drawText(dc, L"尚未落子", makeRect(l.notationList.left + px(14, dpi), listTop, l.notationList.width() - px(28, dpi), lineH),
                 12, kMuted, dpi);
    } else {
        for (int i = 0; i < visible && scroll + i < static_cast<int>(records.size()); ++i) {
            const int idx = scroll + i;
            const auto& rec = records[static_cast<std::size_t>(idx)];
            std::wstring line = (idx + 1 < 10 ? L"0" : L"") + std::to_wstring(idx + 1) + L"  " + stoneLabel(rec.stone);
            const bool aiMove = app.session.pvc &&
                                (app.session.gameType == GameType::ThreePlayer ? rec.stone != Stone::Black : rec.stone == Stone::White);
            if (aiMove) line += L"(电脑)";
            line += L"  ";
            line.push_back(static_cast<wchar_t>(L'A' + rec.move.col));
            line += std::to_wstring(rec.move.row + 1);
            const int y = listTop + i * lineH;
            drawText(dc, line, {l.notationList.left + px(14, dpi), y, l.notationList.right - px(14, dpi), y + lineH},
                     12, idx + 1 == static_cast<int>(records.size()) ? kAccent : kText, dpi);
        }
    }
    for (const ButtonSpec& btn : activeButtons(app, l)) drawButton(dc, btn, app.hovered, dpi);
}

void paintScene(HDC dc, int width, int height, const AppState& app, const Layout& layout) {
    const UINT dpi = layout.dpi;
    fillRect(dc, makeRect(0, 0, width, height), kBackground);
    fillRect(dc, makeRect(0, 0, width, px(5, dpi)), kAccentDark);
    if (app.session.screen == Screen::Login) {
        drawText(dc, L"棋魂", makeRect(0, height / 2 - px(270, dpi), width, px(58, dpi)), 42, kText, dpi, DT_CENTER, FW_BOLD);
        drawText(dc, app.loginMessage, makeRect(0, height / 2 - px(204, dpi), width, px(26, dpi)), 13,
                 app.loginError ? RGB(236, 128, 128) : kMuted, dpi, DT_CENTER, FW_SEMIBOLD);
        drawRoundRect(dc, layout.menuPanel, kPanel, RGB(50, 66, 91), 22, dpi);
        drawTextField(dc, layout.menuButtons[0], L"用户名", app.loginName, app.loginField == 0, dpi);
        drawTextField(dc, layout.menuButtons[1], L"密码", std::wstring(app.loginPass.size(), L'*'), app.loginField == 1, dpi);
        for (const ButtonSpec& btn : activeButtons(app, layout)) drawButton(dc, btn, app.hovered, dpi);
    } else if (app.session.screen == Screen::Menu) {
        if (app.account.loggedIn()) {
            drawText(dc, accountText(app), makeRect(px(28, dpi), px(22, dpi), px(620, dpi), px(42, dpi)),
                     15, kMuted, dpi, DT_LEFT, FW_SEMIBOLD);
        }
        drawText(dc, L"棋魂", makeRect(0, height / 2 - px(270, dpi), width, px(58, dpi)), 42, kText, dpi, DT_CENTER, FW_BOLD);
        drawRoundRect(dc, layout.menuPanel, kPanel, RGB(50, 66, 91), 22, dpi);
        for (const ButtonSpec& btn : activeButtons(app, layout)) drawButton(dc, btn, app.hovered, dpi);
    } else {
        const wchar_t* title = app.session.gameType == GameType::SixPlayer ? L"六子棋"
                             : (app.session.gameType == GameType::ThreePlayer ? L"三人四子棋" : L"五子棋");
        drawText(dc, title, makeRect(px(28, dpi), px(22, dpi), px(180, dpi), px(48, dpi)), 27, kText, dpi, DT_LEFT, FW_BOLD);
        drawBoard(dc, app, layout);
        drawSidePanel(dc, app, layout);
    }
}

// 双缓冲绘制：先画到等大内存位图再 BitBlt 到窗口，避免逐元素绘制闪烁。
void paintWindow(HWND window, AppState& app) {
    PAINTSTRUCT paint{};
    HDC targetDc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left, height = client.bottom - client.top;
    if (width > 0 && height > 0) {
        const Layout layout = calculateLayout(width, height, app.dpi, app.boardSize(), app.menuPage);
        HDC memoryDc = CreateCompatibleDC(targetDc);
        HBITMAP bitmap = memoryDc ? CreateCompatibleBitmap(targetDc, width, height) : nullptr;
        HGDIOBJ prev = (memoryDc && bitmap) ? SelectObject(memoryDc, bitmap) : nullptr;
        if (prev && prev != HGDI_ERROR) {
            paintScene(memoryDc, width, height, app, layout);
            const int dw = paint.rcPaint.right - paint.rcPaint.left, dh = paint.rcPaint.bottom - paint.rcPaint.top;
            if (dw > 0 && dh > 0) BitBlt(targetDc, paint.rcPaint.left, paint.rcPaint.top, dw, dh, memoryDc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
            SelectObject(memoryDc, prev);
        } else {
            paintScene(targetDc, width, height, app, layout);
        }
        if (bitmap) DeleteObject(bitmap);
        if (memoryDc) DeleteDC(memoryDc);
    }
    EndPaint(window, &paint);
}

void invalidateRect(HWND window, Rect rect) {
    if (rect.width() > 0 && rect.height() > 0) {
        const RECT dirty{rect.left - 2, rect.top - 2, rect.right + 2, rect.bottom + 2};
        InvalidateRect(window, &dirty, FALSE);
    }
}

void invalidateButton(HWND window, ButtonAction action, const AppState& app, const Layout& layout) {
    for (const ButtonSpec& btn : activeButtons(app, layout)) {
        if (btn.action == action) { invalidateRect(window, btn.rect); return; }
    }
}

void startSession(AppState& app, GameType type, bool pvc) {
    app.session = GameSession();
    app.session.screen = Screen::Playing;
    app.session.gameType = type;
    app.session.pvc = pvc;
    app.undoHistory.clear();
    app.resultSaved = false;
    if (type == GameType::ThreePlayer) app.session.threeGame = ThreePlayerGame(!pvc);
}

void returnToMainMenu(AppState& app) {
    app.session = GameSession();
    app.menuPage = MenuPage::GameSelection;
    app.undoHistory.clear();
    app.resultSaved = false;
}

void logoutToLogin(AppState& app) {
    app.account.logout();
    returnToMainMenu(app);
    app.session.screen = Screen::Login;
    app.loginPass.clear();
    app.loginField = 0;
    app.loginMessage = L"已退出登录。";
    app.loginError = false;
}

void submitLogin(AppState& app) {
    const std::string name = toUtf8(app.loginName);
    const std::string pass = toUtf8(app.loginPass);
    std::string error = gomoku::validateUsername(name);
    if (error.empty()) error = gomoku::validatePassword(pass);
    if (error.empty()) {
        switch (app.account.loginOrRegister(name, pass)) {
            case gomoku::LoginStatus::Ok:
                app.loginError = false;
                app.loginPass.clear();
                app.loginField = 0;
                returnToMainMenu(app);
                return;
            case gomoku::LoginStatus::InvalidCredentials: error = "密码错误"; break;
            case gomoku::LoginStatus::StorageError: error = "账号文件读写失败"; break;
        }
    }
    app.loginMessage = fromUtf8(error);
    app.loginError = true;
}

void handleLoginChar(AppState& app, wchar_t ch) {
    std::wstring& field = app.loginField == 0 ? app.loginName : app.loginPass;
    if (ch == L'\r') {
        submitLogin(app);
    } else if (ch == 8 || ch == 127) {
        if (!field.empty()) field.pop_back();
    } else if (ch >= 32) {
        field.push_back(ch);
        if (toUtf8(field).size() > (app.loginField == 0 ? 24u : 64u)) field.pop_back();
    }
}

void recordResult(AppState& app) {
    if (app.resultSaved || app.session.screen != Screen::Finished) return;
    app.resultSaved = true;
    if (!app.account.loggedIn()) return;
    const auto winner = app.activeGame().winner();
    app.account.recordOutcome(!winner ? gomoku::Outcome::Draw
                                      : (*winner == Stone::Black ? gomoku::Outcome::Win
                                                                 : gomoku::Outcome::Loss));
}

void scrollNotationToLatest(HWND window, AppState& app) {
    const int visible = notationVisibleLines(currentLayout(window, app));
    app.session.notationScroll = std::max(0, static_cast<int>(app.activeGame().moveHistory().size()) - visible);
}

int nearestGridIndex(int coordinate, int origin, int extent, int boardSize) {
    int bestIndex = 0, bestDist = (std::numeric_limits<int>::max)();
    for (int i = 0; i < boardSize; ++i) {
        const int d = std::abs(coordinate - gridPixel(origin, i, extent, boardSize));
        if (d < bestDist) { bestDist = d; bestIndex = i; }
    }
    return bestDist <= extent / (boardSize - 1) / 2 ? bestIndex : -1;
}

// 玩家落子后、AI 开始计算前，强制把当前棋盘和“电脑思考中”状态刷到屏幕。
void runAiTurn(HWND window, AppState& app, const Layout& layout) {
    invalidateRect(window, layout.boardCard);
    invalidateRect(window, layout.sidePanel);
    UpdateWindow(window);
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514)));

    GameSession& s = app.session;
    if (s.gameType == GameType::ThreePlayer) {
        while (!s.threeGame.finished() && s.threeGame.currentPlayer() != Stone::Black) {
            const auto m = app.threeAi.chooseMove(s.threeGame.board(), s.threeGame.currentPlayer());
            if (m && s.threeGame.play(*m)) { s.lastMove = *m; continue; }
            const auto fallback = s.threeGame.board().emptyMoves();
            if (fallback.empty() || !s.threeGame.play(fallback.front())) break;
            s.lastMove = fallback.front();
        }
    } else if (s.gameType == GameType::SixPlayer) {
        const int aiMoves = s.sixGame.movesRemaining();
        for (int i = 0; i < aiMoves && !s.sixGame.finished(); ++i) {
            const auto m = app.sixAi.chooseMove(s.sixGame.board(), Stone::White, s.sixGame.movesRemaining());
            if (!m || !s.sixGame.play(*m)) break;
            s.lastMove = *m;
        }
    } else {
        if (const auto m = app.ai.chooseMove(s.game.board(), Stone::White); m && s.game.play(*m)) s.lastMove = *m;
    }
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
}

void placeHumanMove(HWND window, AppState& app, const Layout& layout, POINT point) {
    GameSession& s = app.session;
    GameBase& game = app.activeGame();
    if (s.screen != Screen::Playing || (s.pvc && game.currentPlayer() != Stone::Black)) return;

    const int col = nearestGridIndex(point.x, layout.boardGrid.left, layout.boardGrid.width(), app.boardSize());
    const int row = nearestGridIndex(point.y, layout.boardGrid.top, layout.boardGrid.height(), app.boardSize());
    if (row < 0 || col < 0) return;
    const Move move{row, col};

    if (!game.board().isEmpty(move) || (s.gameType == GameType::ThreePlayer && !s.pvc && s.threeGame.violatesOpenThreeRule(move))) {
        invalidateRect(window, layout.sidePanel);
        return;
    }

    // 六子棋 PvC 悔棋回到玩家本回合起点：第二子不再单独保存快照。
    const bool saveSnap = !(s.gameType == GameType::SixPlayer && s.pvc &&
                            s.sixGame.movesRemaining() == 1 && s.sixGame.board().moveCount() > 0);
    if (saveSnap) app.undoHistory.push_back(s);
    if (!game.play(move)) { if (saveSnap) app.undoHistory.pop_back(); return; }

    s.lastMove = move;
    scrollNotationToLatest(window, app);
    if (s.pvc && !game.finished() && game.currentPlayer() != Stone::Black) {
        runAiTurn(window, app, layout);
        scrollNotationToLatest(window, app);
    }
    if (game.finished()) {
        s.screen = Screen::Finished;
        recordResult(app);
    }
    invalidateRect(window, layout.boardCard);
    invalidateRect(window, layout.sidePanel);
}

void performAction(HWND window, AppState& app, ButtonAction action, const Layout& layout) {
    auto selectType = [&](GameType type) {
        app.selectedGame = type;
        app.menuPage = MenuPage::ModeSelection;
    };
    switch (action) {
        case ButtonAction::Login: submitLogin(app); break;
        case ButtonAction::SelectGomoku: selectType(GameType::Gomoku); break;
        case ButtonAction::SelectThreePlayer: selectType(GameType::ThreePlayer); break;
        case ButtonAction::SelectSixPlayer: selectType(GameType::SixPlayer); break;
        case ButtonAction::PvP:
        case ButtonAction::PvC: startSession(app, app.selectedGame, action == ButtonAction::PvC); break;
        case ButtonAction::Undo:
            if (app.undoHistory.empty()) { invalidateRect(window, layout.sidePanel); return; }
            app.session = std::move(app.undoHistory.back());
            app.undoHistory.pop_back();
            invalidateRect(window, layout.boardCard);
            invalidateRect(window, layout.sidePanel);
            return;
        case ButtonAction::Restart: startSession(app, app.session.gameType, app.session.pvc); break;
        case ButtonAction::MainMenu: returnToMainMenu(app); break;
        case ButtonAction::Exit: DestroyWindow(window); return;
        case ButtonAction::None: return;
    }
    InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    AppState* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<AppState*>(reinterpret_cast<const CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app == nullptr) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
        case WM_PAINT: paintWindow(window, *app); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = {px(900, app->dpi), px(650, app->dpi)};
            return 0;
        }
        case WM_DPICHANGED: {
            app->dpi = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const Layout layout = currentLayout(window, *app);
            const POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            const ButtonAction hovered = hitButton(*app, layout, point);
            if (hovered != app->hovered) {
                const ButtonAction prev = app->hovered;
                app->hovered = hovered;
                invalidateButton(window, prev, *app, layout);
                invalidateButton(window, hovered, *app, layout);
            }
            TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            return 0;
        }
        case WM_MOUSELEAVE: {
            const ButtonAction prev = app->hovered;
            app->hovered = ButtonAction::None;
            invalidateButton(window, prev, *app, currentLayout(window, *app));
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            const Layout layout = currentLayout(window, *app);
            if ((app->session.screen == Screen::Playing || app->session.screen == Screen::Finished) &&
                layout.notationList.contains(point)) {
                const int maxScroll = std::max(0, static_cast<int>(app->activeGame().moveHistory().size()) -
                                                      notationVisibleLines(layout));
                const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                const int steps = std::max(1, std::abs(delta) / WHEEL_DELTA) * (delta > 0 ? 1 : -1);
                app->session.notationScroll = std::clamp(app->session.notationScroll - steps * 3, 0, maxScroll);
                invalidateRect(window, layout.notationList);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const Layout layout = currentLayout(window, *app);
            const POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            if (app->session.screen == Screen::Login) {
                if (layout.menuButtons[0].contains(point)) {
                    app->loginField = 0;
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (layout.menuButtons[1].contains(point)) {
                    app->loginField = 1;
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
            }
            if (const ButtonAction action = hitButton(*app, layout, point); action != ButtonAction::None) {
                performAction(window, *app, action, layout);
            } else {
                placeHumanMove(window, *app, layout, point);
            }
            return 0;
        }
        case WM_CHAR:
            if (app->session.screen == Screen::Login) {
                handleLoginChar(*app, static_cast<wchar_t>(wParam));
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_KEYDOWN:
            if (app->session.screen == Screen::Login && wParam == VK_TAB) {
                app->loginField ^= 1;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                if (app->session.screen == Screen::Login) {
                    DestroyWindow(window);
                } else if (app->session.screen == Screen::Menu && app->menuPage == MenuPage::GameSelection) {
                    logoutToLogin(*app);
                    InvalidateRect(window, nullptr, FALSE);
                } else {
                    returnToMainMenu(*app);
                    InvalidateRect(window, nullptr, FALSE);
                }
            }
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        default: return DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

// 程序入口：注册窗口类、按 DPI 调整初始窗口大小、创建窗口并进入消息循环。
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const WNDCLASSEXW wc{
        sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, windowProcedure, 0, 0, instance,
        LoadIconW(nullptr, MAKEINTRESOURCEW(32512)), LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
        CreateSolidBrush(kBackground), nullptr, kWindowClass, LoadIconW(nullptr, MAKEINTRESOURCEW(32512))};
    if (RegisterClassExW(&wc) == 0) return 1;

    const UINT dpi = GetDpiForSystem();
    RECT rc{0, 0, px(1220, dpi), px(850, dpi)};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    AppState app;
    app.dpi = dpi;
    HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance, &app);
    if (window == nullptr) return 1;
    app.dpi = GetDpiForWindow(window);

    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
