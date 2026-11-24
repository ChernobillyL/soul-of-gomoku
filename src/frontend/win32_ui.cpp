// Win32 图形界面：负责窗口、DPI 缩放、双缓冲绘制、鼠标命中测试与三种棋的
// 交互分派。游戏规则全部委托给 classic/three_player/six_player 模块，本文件
// 不实现任何胜负判定。界面状态集中在 AppState，布局集中在 Layout。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>

#include "gomoku/classic/game.hpp"
#include "gomoku/core/account.hpp"
#include "gomoku/three_player/game.hpp"
#include "gomoku/three_player/ai.hpp"
#include "gomoku/six_player/game.hpp"
#include "gomoku/six_player/ai.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
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
using gomoku::Move;
using gomoku::Result;
using gomoku::Stone;
using gomoku::ThreePlayerAi;
using gomoku::ThreePlayerGame;

struct Rect {
    int left;
    int top;
    int right;
    int bottom;

    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool contains(POINT point) const {
        return point.x >= left && point.x < right && point.y >= top && point.y < bottom;
    }
};

enum class Screen { Login, Menu, Playing, Finished };
enum class MenuPage { GameSelection, ModeSelection, Records };
enum class GameType { Gomoku, ThreePlayer, SixPlayer };
enum class ButtonAction {
    None,
    PvP,
    PvC,
    SelectGomoku,
    SelectThreePlayer,
    SelectSixPlayer,
    Undo,
    Restart,
    MainMenu,
    Exit,
    Login,
    Register,
    Guest,
    Logout,
    Records
};

struct UndoSnapshot {
    Game game;
    ThreePlayerGame threeGame;
    gomoku::SixPlayerGame sixGame;
    bool threePlayer = false;
    bool threePlayerAi = false;
    bool sixPlayer = false;
    std::optional<Move> lastMove;
    int notationScroll = 0;
    std::wstring status;
    Screen screen = Screen::Playing;
};

struct Layout {
    Rect boardCard{};
    Rect boardGrid{};
    Rect sidePanel{};
    Rect notationList{};
    std::array<Rect, 3> buttons{};
    std::array<Rect, 4> menuButtons{};
    Rect menuExit{};
    Rect menuPanel{};
    Rect accountButton{};
    Rect loginPanel{};
    Rect loginUsername{};
    Rect loginPassword{};
    std::array<Rect, 3> loginButtons{};
    Rect loginExit{};
    Rect recordsPanel{};
    Rect recordsBack{};
    UINT dpi = 96;
    int boardSize = gomoku::kBoardSize;
};

// 数据目录放在可执行文件旁边，双击根目录的 棋魂.exe 时与源码目录一致；
// 从其他工作目录启动也不会把账号写到意外位置。
std::filesystem::path executableDataDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) return std::filesystem::path(L"data");
    return std::filesystem::path(buffer).parent_path() / L"data";
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            output.data(), size, nullptr, nullptr);
    }
    return output;
}

std::wstring fromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            output.data(), size);
    }
    return output;
}

std::wstring trimSpaces(const std::wstring& text) {
    const auto isSpace = [](wchar_t ch) { return ch == L' ' || ch == L'\t'; };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && isSpace(text[begin])) ++begin;
    while (end > begin && isSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

// 界面的唯一状态对象：保存当前页面、所选棋种、三个对局对象、三个 AI、
// 账号会话、悔棋快照和棋谱滚动位置。board()/currentPlayer() 等只读访问器
// 按当前模式分派到具体对局对象，绘制代码因此不需要重复写三套分支。
struct AppState {
    Screen screen = Screen::Login;
    MenuPage menuPage = MenuPage::GameSelection;
    GameType selectedGame = GameType::Gomoku;
    bool pvc = false;
    bool threePlayer = false;
    bool threePlayerAi = false;
    bool sixPlayer = false;
    bool resultRecorded = false;
    UINT dpi = 96;
    Game game;
    ThreePlayerGame threeGame;
    gomoku::SixPlayerGame sixGame;
    gomoku::AiPlayer ai;
    ThreePlayerAi threeAi;
    gomoku::SixPlayerAi sixAi;
    gomoku::AccountManager account;
    bool guest = false;
    std::wstring currentUser;
    std::wstring loginName;
    std::wstring loginPassword;
    int loginField = 0;
    std::wstring loginMessage = L"请输入用户名和密码；首次使用请先注册。";
    bool loginError = false;
    std::vector<gomoku::RecordEntry> recordEntries;
    int recordScroll = 0;
    std::optional<Move> lastMove;
    std::vector<UndoSnapshot> undoHistory;
    int notationScroll = 0;
    std::wstring status = L"登录后选择棋种和模式，开始对局。";
    ButtonAction hovered = ButtonAction::None;

    AppState() : account(executableDataDirectory()) {
        if (!account.load()) {
            loginMessage = L"无法创建 data 目录，账号功能不可用；可使用游客进入。";
            loginError = true;
        }
    }

    const Board& board() const {
        return sixPlayer ? sixGame.board() : (threePlayer ? threeGame.board() : game.board());
    }
    Stone currentPlayer() const {
        return sixPlayer ? sixGame.currentPlayer() : (threePlayer ? threeGame.currentPlayer() : game.currentPlayer());
    }
    const std::vector<gomoku::MoveRecord>& moveHistory() const {
        return sixPlayer ? sixGame.moveHistory() : (threePlayer ? threeGame.moveHistory() : game.moveHistory());
    }
    bool finished() const {
        return sixPlayer ? sixGame.finished() :
               (threePlayer ? threeGame.finished() : game.result() != Result::Ongoing);
    }
    int boardSize() const { return sixPlayer ? gomoku::kSixPlayerBoardSize : gomoku::kBoardSize; }
};

int px(int logicalSize, UINT dpi) {
    return MulDiv(logicalSize, static_cast<int>(dpi), 96);
}

Rect makeRect(int left, int top, int width, int height) {
    return {left, top, left + width, top + height};
}

Rect scaledRect(int left, int top, int width, int height, UINT dpi) {
    return makeRect(px(left, dpi), px(top, dpi), px(width, dpi), px(height, dpi));
}

int gridPixel(int origin, int index, int extent, int boardSize) {
    return origin + MulDiv(index, extent, boardSize - 1);
}

void fillRect(HDC dc, Rect rect, COLORREF color) {
    const RECT nativeRect{rect.left, rect.top, rect.right, rect.bottom};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &nativeRect, brush);
    DeleteObject(brush);
}

void drawRoundRect(HDC dc, Rect rect, COLORREF fill, COLORREF outline, int radius, UINT dpi) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, px(1, dpi)), outline);
    const HGDIOBJ oldBrush = SelectObject(dc, brush);
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    const int scaledRadius = px(radius, dpi);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, scaledRadius, scaledRadius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void drawText(HDC dc, const std::wstring& text, Rect rect, int logicalSize, COLORREF color,
              UINT dpi, UINT alignment = DT_LEFT, int weight = FW_NORMAL) {
    const int fontHeight = std::max(1, px(logicalSize, dpi));
    HFONT font = CreateFontW(-fontHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (font == nullptr) return;
    const HGDIOBJ oldFont = SelectObject(dc, font);
    const int oldMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, color);
    const RECT nativeRect{rect.left, rect.top, rect.right, rect.bottom};
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), const_cast<RECT*>(&nativeRect),
              alignment | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldMode);
    SelectObject(dc, oldFont);
    DeleteObject(font);
}

void drawCenteredText(HDC dc, const std::wstring& text, Rect rect, int size, COLORREF color,
                      UINT dpi, int weight = FW_NORMAL) {
    drawText(dc, text, rect, size, color, dpi, DT_CENTER, weight);
}

void drawButton(HDC dc, Rect rect, const std::wstring& label, ButtonAction action,
                ButtonAction hovered, UINT dpi, bool danger = false,
                bool enabled = true, bool highlighted = false) {
    const bool isHovered = enabled && (action == hovered || highlighted);
    COLORREF fill = kPanelLight;
    COLORREF outline = RGB(61, 77, 101);
    COLORREF textColor = kText;
    if (!enabled) {
        fill = kPanel;
        outline = RGB(48, 61, 79);
        textColor = RGB(104, 119, 139);
    } else if (danger) {
        fill = isHovered ? RGB(151, 61, 67) : RGB(116, 50, 59);
        outline = RGB(196, 87, 91);
    } else if (isHovered) {
        fill = RGB(52, 70, 97);
        outline = RGB(116, 134, 158);
    }
    drawRoundRect(dc, rect, fill, outline, 14, dpi);
    drawCenteredText(dc, label, rect, 19, textColor, dpi, FW_SEMIBOLD);
}

// 参数：客户区宽高与界面状态。
// 返回：按逻辑像素计算、再经 DPI 缩放的各区域矩形。
// 实现：登录页、对局记录页使用独立的居中面板；菜单页垂直居中，棋种页有
// 四个棋种/记录按钮和退出按钮；对局页保证 19 路棋盘与右侧面板不越界。
Layout calculateLayout(int width, int height, const AppState& app) {
    Layout layout;
    const UINT dpi = app.dpi;
    layout.dpi = dpi;
    layout.boardSize = app.boardSize();
    layout.accountButton = makeRect(width - px(190, dpi) - px(26, dpi), px(22, dpi),
                                    px(190, dpi), px(42, dpi));

    if (app.screen == Screen::Login) {
        const int panelWidth = std::min(px(560, dpi), std::max(px(360, dpi), width - px(40, dpi)));
        const int panelHeight = px(510, dpi);
        const int panelLeft = width / 2 - panelWidth / 2;
        const int panelTop = std::max(px(16, dpi), (height - panelHeight) / 2);
        layout.loginPanel = makeRect(panelLeft, panelTop, panelWidth, panelHeight);
        const int fieldWidth = panelWidth - px(96, dpi);
        const int fieldLeft = width / 2 - fieldWidth / 2;
        layout.loginUsername = makeRect(fieldLeft, panelTop + px(132, dpi), fieldWidth, px(50, dpi));
        layout.loginPassword = makeRect(fieldLeft, panelTop + px(202, dpi), fieldWidth, px(50, dpi));
        const int gap = px(14, dpi);
        const int halfWidth = (fieldWidth - gap) / 2;
        layout.loginButtons[0] =
            makeRect(fieldLeft, panelTop + px(302, dpi), halfWidth, px(48, dpi));
        layout.loginButtons[1] =
            makeRect(fieldLeft + halfWidth + gap, panelTop + px(302, dpi), halfWidth, px(48, dpi));
        layout.loginButtons[2] =
            makeRect(fieldLeft, panelTop + px(364, dpi), fieldWidth, px(44, dpi));
        layout.loginExit = makeRect(fieldLeft, panelTop + px(420, dpi), fieldWidth, px(42, dpi));
        return layout;
    }

    if (app.screen == Screen::Menu && app.menuPage == MenuPage::Records) {
        const int panelWidth = std::min(px(760, dpi), std::max(px(420, dpi), width - px(60, dpi)));
        const int panelTop = px(80, dpi);
        const int panelHeight = std::max(px(420, dpi), height - px(150, dpi));
        layout.recordsPanel = makeRect(width / 2 - panelWidth / 2, panelTop, panelWidth,
                                       panelHeight);
        layout.recordsBack = makeRect(width / 2 - px(100, dpi),
                                      layout.recordsPanel.bottom - px(64, dpi), px(200, dpi),
                                      px(44, dpi));
        return layout;
    }

    const bool gamePage = app.menuPage == MenuPage::GameSelection;
    const int menuButtonCount = gamePage ? 4 : 3;
    const int menuRowCount = menuButtonCount + (gamePage ? 1 : 0);
    const int buttonWidth = std::min(px(330, dpi), std::max(px(240, dpi), width / 3));
    const int buttonHeight = px(50, dpi);
    const int menuLeft = width / 2 - buttonWidth / 2;
    const int menuPitch = px(58, dpi);
    const int menuRowsHeight = menuPitch * (menuRowCount - 1) + buttonHeight;
    const int menuTop = (height - menuRowsHeight) / 2;
    for (int i = 0; i < menuButtonCount; ++i) {
        layout.menuButtons[static_cast<std::size_t>(i)] =
            makeRect(menuLeft, menuTop + i * menuPitch, buttonWidth, buttonHeight);
    }
    if (gamePage) {
        layout.menuExit = makeRect(menuLeft, menuTop + menuButtonCount * menuPitch, buttonWidth, buttonHeight);
    }
    const int menuPanelTop = menuTop - px(44, dpi);
    const int menuPanelBottom = (gamePage ? layout.menuExit.bottom : layout.menuButtons[2].bottom) + px(44, dpi);
    layout.menuPanel = makeRect(width / 2 - px(260, dpi), menuPanelTop, px(520, dpi),
                                menuPanelBottom - menuPanelTop);


    const int margin = std::max(px(22, dpi), std::min(width, height) / 30);
    const int headerHeight = px(104, dpi);
    const int availableHeight = std::max(px(300, dpi), height - px(150, dpi));
    const int availableWidth = std::max(px(300, dpi), width - px(390, dpi));
    const int desiredBoardSize = px(36 * (layout.boardSize - 1) + 72, dpi);
    const int boardPixels = std::max(px(300, dpi),
                                     std::min({availableHeight, availableWidth, desiredBoardSize}));
    layout.boardCard = makeRect(margin, headerHeight, boardPixels, boardPixels);
    const int labelMargin = std::max(px(36, dpi), boardPixels / 16);
    layout.boardGrid = {layout.boardCard.left + labelMargin,
                        layout.boardCard.top + labelMargin,
                        layout.boardCard.right - labelMargin,
                        layout.boardCard.bottom - labelMargin};

    const int panelX = layout.boardCard.right + margin;
    const int panelWidth = std::max(px(260, dpi), width - panelX - margin);
    layout.sidePanel = makeRect(panelX, headerHeight, panelWidth, boardPixels);
    const int sideButtonHeight = std::max(px(40, dpi), std::min(px(48, dpi), layout.sidePanel.height() / 11));
    const int gap = px(7, dpi);
    const int buttonY = layout.sidePanel.bottom - sideButtonHeight * 3 - gap * 2 - px(18, dpi);
    layout.notationList = {panelX + px(20, dpi), layout.sidePanel.top + px(116, dpi),
                           panelX + panelWidth - px(20, dpi), buttonY - px(14, dpi)};
    for (int i = 0; i < 3; ++i) {
        layout.buttons[static_cast<std::size_t>(i)] = makeRect(
            panelX + px(18, dpi), buttonY + i * (sideButtonHeight + gap), panelWidth - px(36, dpi), sideButtonHeight);
    }
    return layout;
}

void drawBackground(HDC dc, int width, int height, UINT dpi);

// 菜单与记录页顶部的账号摘要，直接读取内存中的战绩，不需要再次读文件。
std::wstring accountSummary(const AppState& app) {
    if (app.guest) return L"游客模式 ｜ 对局不保存";
    if (!app.account.loggedIn()) return L"未登录";
    std::wstring text = L"用户：" + fromUtf8(app.account.currentUsername());
    if (const auto stats = app.account.currentStats()) {
        text += L" ｜ 胜 " + std::to_wstring(stats->wins) + L" 负 " +
                std::to_wstring(stats->losses) + L" 平 " + std::to_wstring(stats->draws);
    }
    return text;
}

const wchar_t* outcomeLabel(gomoku::Outcome outcome) {
    switch (outcome) {
        case gomoku::Outcome::Win: return L"胜";
        case gomoku::Outcome::Loss: return L"负";
        case gomoku::Outcome::Draw: return L"平";
    }
    return L"平";
}

COLORREF outcomeColor(gomoku::Outcome outcome) {
    switch (outcome) {
        case gomoku::Outcome::Win: return kAccent;
        case gomoku::Outcome::Loss: return RGB(233, 140, 140);
        case gomoku::Outcome::Draw: return kMuted;
    }
    return kMuted;
}

std::wstring maskedPassword(const std::wstring& password) {
    return std::wstring(password.size(), L'*');
}

void drawLoginField(HDC dc, Rect rect, const std::wstring& placeholder,
                    const std::wstring& value, bool active, UINT dpi) {
    drawRoundRect(dc, rect, active ? RGB(43, 58, 80) : kPanelLight,
                  active ? kAccent : RGB(61, 77, 101), 12, dpi);
    std::wstring text = value.empty() ? placeholder : value;
    const COLORREF color = value.empty() ? RGB(118, 134, 154) : kText;
    if (active) text += L"|";
    drawText(dc, text,
             makeRect(rect.left + px(16, dpi), rect.top,
                      rect.width() - px(32, dpi), rect.height()),
             18, color, dpi, DT_LEFT, FW_SEMIBOLD);
}

// 登录/注册页：用户名与密码两个输入框，点击切换焦点；密码以星号显示。
void drawLogin(HDC dc, int width, int height, const AppState& app, const Layout& layout) {
    const UINT dpi = layout.dpi;
    drawBackground(dc, width, height, dpi);
    drawRoundRect(dc, layout.loginPanel, kPanel, RGB(50, 66, 91), 24, dpi);
    const int panelTop = layout.loginPanel.top;
    drawCenteredText(dc, L"棋魂", makeRect(0, panelTop + px(26, dpi), width, px(50, dpi)),
                     40, kText, dpi, FW_BOLD);
    drawCenteredText(dc, L"登录账号可保存战绩与对局记录",
                     makeRect(0, panelTop + px(80, dpi), width, px(26, dpi)), 14, kMuted, dpi);

    drawLoginField(dc, layout.loginUsername, L"用户名", app.loginName, app.loginField == 0, dpi);
    drawLoginField(dc, layout.loginPassword, L"密码", maskedPassword(app.loginPassword),
                   app.loginField == 1, dpi);
    drawCenteredText(dc, app.loginMessage,
                     makeRect(layout.loginPanel.left + px(24, dpi), panelTop + px(262, dpi),
                              layout.loginPanel.width() - px(48, dpi), px(34, dpi)),
                     13, app.loginError ? RGB(236, 128, 128) : kAccent, dpi, FW_SEMIBOLD);
    drawButton(dc, layout.loginButtons[0], L"登录", ButtonAction::Login, app.hovered, dpi);
    drawButton(dc, layout.loginButtons[1], L"注册", ButtonAction::Register, app.hovered, dpi);
    drawButton(dc, layout.loginButtons[2], L"游客进入", ButtonAction::Guest, app.hovered, dpi);
    drawButton(dc, layout.loginExit, L"退出游戏", ButtonAction::Exit, app.hovered, dpi, true);
}

// 对局记录页：读取 result 缓存（进入页面或结束对局时刷新），只显示摘要。
void drawRecords(HDC dc, int width, int height, const AppState& app, const Layout& layout) {
    const UINT dpi = layout.dpi;
    drawBackground(dc, width, height, dpi);
    drawText(dc, L"我的对局记录", makeRect(px(32, dpi), px(24, dpi), width - px(360, dpi), px(44, dpi)),
             28, kText, dpi, DT_LEFT, FW_BOLD);
    drawButton(dc, layout.accountButton,
               app.account.loggedIn() ? L"退出登录" : L"切换账号", ButtonAction::Logout,
               app.hovered, dpi);

    const Rect panel = layout.recordsPanel;
    drawRoundRect(dc, panel, kPanel, RGB(50, 66, 91), 20, dpi);
    drawText(dc, accountSummary(app),
             makeRect(panel.left + px(24, dpi), panel.top + px(18, dpi),
                      panel.width() - px(48, dpi), px(30, dpi)),
             15, kMuted, dpi, DT_LEFT, FW_SEMIBOLD);
    const int listTop = panel.top + px(62, dpi);
    const int listBottom = layout.recordsBack.top - px(14, dpi);
    const int lineHeight = px(30, dpi);
    const Rect listRect{panel.left + px(24, dpi), listTop, panel.right - px(24, dpi), listBottom};

    if (!app.account.loggedIn()) {
        drawCenteredText(dc, L"游客模式不保存对局记录，登录后可在这里查看战绩。", listRect,
                         15, kMuted, dpi);
    } else if (app.recordEntries.empty()) {
        drawCenteredText(dc, L"暂无对局记录，完成一局后会自动保存。", listRect, 15, kMuted, dpi);
    } else {
        const int visibleLines = std::max(0, (listBottom - listTop) / lineHeight);
        const int maxScroll =
            std::max(0, static_cast<int>(app.recordEntries.size()) - visibleLines);
        const int scroll = std::clamp(app.recordScroll, 0, maxScroll);
        for (int lineIndex = 0; lineIndex < visibleLines; ++lineIndex) {
            const std::size_t recordIndex =
                static_cast<std::size_t>(scroll + lineIndex);
            if (recordIndex >= app.recordEntries.size()) break;
            const gomoku::RecordEntry& entry = app.recordEntries[recordIndex];
            const int y = listTop + lineIndex * lineHeight;
            std::wstring line = fromUtf8(entry.timestamp) + L"   " + fromUtf8(entry.game) +
                                L" · " + fromUtf8(entry.mode) + L"   " +
                                outcomeLabel(entry.outcome) + L"   " +
                                fromUtf8(entry.detail) + L"   " +
                                std::to_wstring(entry.moveCount) + L" 手";
            drawText(dc, line,
                     makeRect(listRect.left, y, listRect.width(), lineHeight), 14,
                     outcomeColor(entry.outcome), dpi, DT_LEFT, FW_SEMIBOLD);
        }
    }
    drawButton(dc, layout.recordsBack, L"返回", ButtonAction::MainMenu, app.hovered, dpi);
}

void drawBackground(HDC dc, int width, int height, UINT dpi) {
    fillRect(dc, makeRect(0, 0, width, height), kBackground);
    fillRect(dc, makeRect(0, 0, width, px(5, dpi)), kAccentDark);
}

void drawMenu(HDC dc, int width, int height, const AppState& app, const Layout& layout) {
    const UINT dpi = layout.dpi;
    drawBackground(dc, width, height, dpi);
    drawText(dc, accountSummary(app),
             makeRect(px(28, dpi), px(22, dpi), width - px(260, dpi), px(42, dpi)), 15, kMuted,
             dpi, DT_LEFT, FW_SEMIBOLD);
    drawButton(dc, layout.accountButton,
               app.account.loggedIn() ? L"退出登录" : L"切换账号", ButtonAction::Logout,
               app.hovered, dpi);
    drawCenteredText(dc, L"棋魂", makeRect(0, height / 2 - px(270, dpi), width, px(58, dpi)),
                     42, kText, dpi, FW_BOLD);

    drawRoundRect(dc, layout.menuPanel, kPanel, RGB(50, 66, 91), 22, dpi);
    if (app.menuPage == MenuPage::GameSelection) {
        drawButton(dc, layout.menuButtons[0], L"五子棋", ButtonAction::SelectGomoku, app.hovered, dpi);
        drawButton(dc, layout.menuButtons[1], L"三人四子棋", ButtonAction::SelectThreePlayer, app.hovered, dpi);
        drawButton(dc, layout.menuButtons[2], L"六子棋", ButtonAction::SelectSixPlayer, app.hovered, dpi);
        drawButton(dc, layout.menuButtons[3], L"我的对局记录", ButtonAction::Records, app.hovered, dpi);
        drawButton(dc, layout.menuExit, L"退出游戏", ButtonAction::Exit, app.hovered, dpi, true);
    } else {
        drawButton(dc, layout.menuButtons[0], L"玩家对战  ·  PvP", ButtonAction::PvP, app.hovered, dpi);
        drawButton(dc, layout.menuButtons[1], L"人机对战  ·  PvC", ButtonAction::PvC, app.hovered, dpi);
        drawButton(dc, layout.menuButtons[2], L"返回主菜单", ButtonAction::MainMenu, app.hovered, dpi, true);
    }
}

const wchar_t* stoneLabel(Stone stone) {
    switch (stone) {
        case Stone::Black: return L"黑";
        case Stone::White: return L"白";
        case Stone::Red: return L"红";
        case Stone::Empty: break;
    }
    return L"";
}

const wchar_t* threePlayerLabel(Stone stone) {
    switch (stone) {
        case Stone::Black: return L"玩家1（黑棋）";
        case Stone::White: return L"玩家2（白棋）";
        case Stone::Red: return L"玩家3（红棋）";
        case Stone::Empty: break;
    }
    return L"玩家";
}

std::wstring playerText(const AppState& app) {
    if (app.threePlayer && !app.sixPlayer) {
        if (app.screen == Screen::Finished) {
            const auto winner = app.threeGame.winner();
            if (!winner) return L"平局，再来一局？";
            if (app.threePlayerAi) {
                switch (*winner) {
                    case Stone::Black: return L"你赢了！";
                    case Stone::White: return L"电脑1（白棋）获胜！";
                    case Stone::Red: return L"电脑2（红棋）获胜！";
                    case Stone::Empty: break;
                }
            }
            return std::wstring(threePlayerLabel(*winner)) + L"获胜！";
        }
        if (app.threePlayerAi) {
            switch (app.threeGame.currentPlayer()) {
                case Stone::Black: return L"轮到你（黑棋）";
                case Stone::White: return L"电脑1思考中（白棋）";
                case Stone::Red: return L"电脑2思考中（红棋）";
                case Stone::Empty: break;
            }
        }
        return std::wstring(L"轮到") + threePlayerLabel(app.threeGame.currentPlayer());
    }
    if (app.screen == Screen::Finished && app.sixPlayer) {
        const auto winner = app.sixGame.winner();
        if (!winner) return L"平局，再来一局？";
        if (app.pvc) return *winner == Stone::Black ? L"你赢了！" : L"电脑获胜！";
        return *winner == Stone::Black ? L"黑方获胜！" : L"白方获胜！";
    }
    if (app.screen == Screen::Finished) {
        switch (app.game.result()) {
            case Result::BlackWins: return app.pvc ? L"你赢了！" : L"黑棋获胜！";
            case Result::WhiteWins: return app.pvc ? L"电脑获胜" : L"白棋获胜！";
            case Result::Draw: return L"平局，再来一局？";
            case Result::Ongoing: break;
        }
    }
    if (app.sixPlayer) {
        const Stone turn = app.sixGame.currentPlayer();
        const std::wstring remaining = L"，本回合还需落 " +
                                      std::to_wstring(app.sixGame.movesRemaining()) + L" 子";
        if (app.pvc) return (turn == Stone::Black ? L"轮到你（黑方）" : L"电脑思考中（白方）") + remaining;
        return (turn == Stone::Black ? L"轮到黑方" : L"轮到白方") + remaining;
    }
    if (app.pvc) return app.game.currentPlayer() == Stone::Black ? L"轮到你（黑棋）" : L"电脑思考中（白棋）";
    return app.game.currentPlayer() == Stone::Black ? L"轮到黑棋" : L"轮到白棋";
}

void drawHeader(HDC dc, const AppState& app) {
    const UINT dpi = app.dpi;
    const wchar_t* gameTitle = app.sixPlayer ? L"六子棋" : (app.threePlayer ? L"三人四子棋" : L"五子棋");
    drawText(dc, gameTitle, scaledRect(28, 22, 180, 48, dpi), 27, kText, dpi, DT_LEFT, FW_BOLD);
}

void drawStone(HDC dc, int x, int y, int radius, Stone stone, bool isLast, UINT dpi) {
    if (isLast) {
        HBRUSH markerBrush = CreateSolidBrush(kAccentDark);
        HPEN markerPen = CreatePen(PS_SOLID, std::max(1, px(1, dpi)), kAccentDark);
        const HGDIOBJ oldBrush = SelectObject(dc, markerBrush);
        const HGDIOBJ oldPen = SelectObject(dc, markerPen);
        const int markerRadius = radius + px(5, dpi);
        Ellipse(dc, x - markerRadius, y - markerRadius, x + markerRadius + 1, y + markerRadius + 1);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(markerPen);
        DeleteObject(markerBrush);
    }

    COLORREF stoneColor = RGB(247, 247, 239);
    COLORREF outlineColor = RGB(185, 186, 177);
    if (stone == Stone::Black) {
        stoneColor = RGB(25, 31, 41);
        outlineColor = RGB(8, 12, 18);
    } else if (stone == Stone::Red) {
        stoneColor = RGB(198, 64, 64);
        outlineColor = RGB(122, 32, 36);
    }
    HBRUSH brush = CreateSolidBrush(stoneColor);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, px(1, dpi)), outlineColor);
    const HGDIOBJ oldBrush = SelectObject(dc, brush);
    const HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, x - radius, y - radius, x + radius + 1, y + radius + 1);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void drawBoard(HDC dc, const AppState& app, const Layout& layout) {
    const UINT dpi = layout.dpi;
    drawRoundRect(dc, layout.boardCard, kBoardWood, RGB(160, 119, 70), 18, dpi);
    const int spacing = layout.boardGrid.width() / (layout.boardSize - 1);
    const int radius = std::max(px(4, dpi), static_cast<int>(spacing * 0.38));
    const int left = layout.boardGrid.left;
    const int top = layout.boardGrid.top;
    const int right = layout.boardGrid.right;
    const int bottom = layout.boardGrid.bottom;

    HPEN gridPen = CreatePen(PS_SOLID, std::max(1, px(1, dpi)), kGrid);
    const HGDIOBJ oldPen = SelectObject(dc, gridPen);
    for (int i = 0; i < layout.boardSize; ++i) {
        const int x = gridPixel(left, i, layout.boardGrid.width(), layout.boardSize);
        const int y = gridPixel(top, i, layout.boardGrid.height(), layout.boardSize);
        MoveToEx(dc, left, y, nullptr);
        LineTo(dc, right + 1, y);
        MoveToEx(dc, x, top, nullptr);
        LineTo(dc, x, bottom + 1);

        const wchar_t columnLabel[] = {static_cast<wchar_t>(L'A' + i), L'\0'};
        const Rect colRect{x - px(12, dpi), layout.boardCard.top + px(8, dpi),
                           x + px(12, dpi), top - px(2, dpi)};
        drawCenteredText(dc, columnLabel, colRect, 11, RGB(109, 78, 47), dpi, FW_SEMIBOLD);
        const std::wstring rowLabel = std::to_wstring(i + 1);
        const Rect rowRect{layout.boardCard.left + px(4, dpi), y - px(9, dpi),
                           left - px(5, dpi), y + px(10, dpi)};
        drawCenteredText(dc, rowLabel, rowRect, 11, RGB(109, 78, 47), dpi, FW_SEMIBOLD);
    }
    SelectObject(dc, oldPen);
    DeleteObject(gridPen);

    const std::array<int, 3> starPoints = layout.boardSize == 19
        ? std::array<int, 3>{{3, 9, 15}}
        : std::array<int, 3>{{3, 7, 11}};
    HBRUSH starBrush = CreateSolidBrush(kGrid);
    const HGDIOBJ oldBrush = SelectObject(dc, starBrush);
    const int starRadius = std::max(2, px(3, dpi));
    for (const int row : starPoints) {
        for (const int col : starPoints) {
            const int x = gridPixel(left, col, layout.boardGrid.width(), layout.boardSize);
            const int y = gridPixel(top, row, layout.boardGrid.height(), layout.boardSize);
            Ellipse(dc, x - starRadius, y - starRadius, x + starRadius + 1, y + starRadius + 1);
        }
    }
    SelectObject(dc, oldBrush);
    DeleteObject(starBrush);

    for (int row = 0; row < layout.boardSize; ++row) {
        for (int col = 0; col < layout.boardSize; ++col) {
            const Move move{row, col};
            const Stone stone = app.board().at(move);
            if (stone == Stone::Empty) continue;
            const int x = gridPixel(left, col, layout.boardGrid.width(), layout.boardSize);
            const int y = gridPixel(top, row, layout.boardGrid.height(), layout.boardSize);
            drawStone(dc, x, y, radius, stone, app.lastMove && *app.lastMove == move, dpi);
        }
    }
}

int notationVisibleLines(const Layout& layout) {
    const int lineHeight = std::max(1, px(24, layout.dpi));
    const int contentHeight = layout.notationList.height() - px(46, layout.dpi) - px(8, layout.dpi);
    return std::max(0, contentHeight / lineHeight);
}

void drawSidePanel(HDC dc, const AppState& app, const Layout& layout) {
    const UINT dpi = layout.dpi;
    const int left = layout.sidePanel.left;
    const int top = layout.sidePanel.top;
    const int width = layout.sidePanel.width();
    drawRoundRect(dc, layout.sidePanel, kPanel, RGB(48, 64, 87), 20, dpi);
    drawText(dc, L"当前对局", makeRect(left + px(22, dpi), top + px(18, dpi), width - px(44, dpi), px(28, dpi)),
             15, kMuted, dpi, DT_LEFT, FW_SEMIBOLD);
    drawText(dc, playerText(app), makeRect(left + px(22, dpi), top + px(54, dpi), width - px(44, dpi), px(48, dpi)),
             23, app.screen == Screen::Finished ? kAccent : kText, dpi, DT_LEFT, FW_BOLD);

    drawRoundRect(dc, layout.notationList, kPanelLight, RGB(61, 77, 101), 14, dpi);
    drawText(dc, L"落子记录", makeRect(layout.notationList.left + px(14, dpi),
                                       layout.notationList.top + px(8, dpi),
                                       layout.notationList.width() - px(28, dpi), px(26, dpi)),
             14, kMuted, dpi, DT_LEFT, FW_SEMIBOLD);

    const int lineHeight = std::max(1, px(24, dpi));
    const int listTop = layout.notationList.top + px(38, dpi);
    const int visibleLines = notationVisibleLines(layout);
    const auto& records = app.moveHistory();
    const int maxScroll = std::max(0, static_cast<int>(records.size()) - visibleLines);
    const int scroll = std::clamp(app.notationScroll, 0, maxScroll);
    if (records.empty()) {
        drawText(dc, L"尚未落子", makeRect(layout.notationList.left + px(14, dpi), listTop,
                                            layout.notationList.width() - px(28, dpi), lineHeight),
                 12, kMuted, dpi);
    } else {
        for (int i = 0; i < visibleLines; ++i) {
            const int recordIndex = scroll + i;
            if (recordIndex >= static_cast<int>(records.size())) break;
            const auto& record = records[static_cast<std::size_t>(recordIndex)];
            const wchar_t* stoneName = stoneLabel(record.stone);
            const wchar_t column = static_cast<wchar_t>(L'A' + record.move.col);
            std::wstring line = std::to_wstring(recordIndex + 1);
            if (recordIndex + 1 < 10) line = L"0" + line;
            line += L"  ";
            line += stoneName;
            const bool computerMove = app.threePlayer
                                          ? (app.threePlayerAi && record.stone != Stone::Black)
                                          : (app.pvc && record.stone == Stone::White);
            if (computerMove) line += L"(电脑)";
            line += L"  ";
            line += column;
            line += std::to_wstring(record.move.row + 1);
            const int y = listTop + i * lineHeight;
            const Rect lineRect{layout.notationList.left + px(14, dpi), y,
                                layout.notationList.right - px(14, dpi), y + lineHeight};
            drawText(dc, line, lineRect, 12, recordIndex + 1 == static_cast<int>(records.size())
                                             ? kAccent : kText, dpi);
        }
    }
    drawButton(dc, layout.buttons[0], L"悔棋", ButtonAction::Undo, app.hovered, dpi, false,
               !app.undoHistory.empty(), app.undoHistory.empty() && app.hovered == ButtonAction::Undo);
    drawButton(dc, layout.buttons[1], app.screen == Screen::Finished ? L"再来一局" : L"重新开始",
               ButtonAction::Restart, app.hovered, dpi);
    drawButton(dc, layout.buttons[2], L"返回主菜单", ButtonAction::MainMenu, app.hovered, dpi, true);
}

void drawGame(HDC dc, int width, int height, const AppState& app, const Layout& layout) {
    drawBackground(dc, width, height, layout.dpi);
    drawHeader(dc, app);
    drawBoard(dc, app, layout);
    drawSidePanel(dc, app, layout);
}

void paintScene(HDC dc, int width, int height, const AppState& app, const Layout& layout) {
    if (app.screen == Screen::Login) {
        drawLogin(dc, width, height, app, layout);
    } else if (app.screen == Screen::Menu && app.menuPage == MenuPage::Records) {
        drawRecords(dc, width, height, app, layout);
    } else if (app.screen == Screen::Menu) {
        drawMenu(dc, width, height, app, layout);
    } else {
        drawGame(dc, width, height, app, layout);
    }
}

// 参数：window 为窗口句柄，app 为界面状态。
// 实现：创建与客户区等大的内存 DC 和位图，先在内存中绘制完整场景，再用
// BitBlt 一次性拷贝到屏幕，避免逐元素绘制造成的闪烁；最后恢复并删除 GDI 对象。
void paintWindow(HWND window, AppState& app) {
    PAINTSTRUCT paint{};
    HDC targetDc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        EndPaint(window, &paint);
        return;
    }

    const Layout layout = calculateLayout(width, height, app);
    HDC memoryDc = CreateCompatibleDC(targetDc);
    HBITMAP bitmap = memoryDc != nullptr ? CreateCompatibleBitmap(targetDc, width, height) : nullptr;
    if (memoryDc == nullptr || bitmap == nullptr) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memoryDc != nullptr) DeleteDC(memoryDc);
        paintScene(targetDc, width, height, app, layout);
        EndPaint(window, &paint);
        return;
    }

    HGDIOBJ previousBitmap = SelectObject(memoryDc, bitmap);
    if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
        paintScene(targetDc, width, height, app, layout);
        EndPaint(window, &paint);
        return;
    }

    paintScene(memoryDc, width, height, app, layout);
    const int dirtyWidth = paint.rcPaint.right - paint.rcPaint.left;
    const int dirtyHeight = paint.rcPaint.bottom - paint.rcPaint.top;
    if (dirtyWidth > 0 && dirtyHeight > 0) {
        BitBlt(targetDc, paint.rcPaint.left, paint.rcPaint.top, dirtyWidth, dirtyHeight,
               memoryDc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);
    }

    SelectObject(memoryDc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    EndPaint(window, &paint);
}

void startGame(AppState& app, bool pvc) {
    app.screen = Screen::Playing;
    app.pvc = pvc;
    app.sixPlayer = false;
    app.threePlayer = false;
    app.threePlayerAi = false;
    app.resultRecorded = false;
    app.game = Game{};
    app.threeGame = ThreePlayerGame{};
    app.sixGame = gomoku::SixPlayerGame{};
    app.lastMove.reset();
    app.undoHistory.clear();
    app.notationScroll = 0;
    app.status = pvc ? L"你执黑棋先手。" : L"黑棋先手，请先落子。";
}

void startThreePlayerGame(AppState& app, bool threePlayerAi) {
    app.screen = Screen::Playing;
    app.pvc = false;
    app.sixPlayer = false;
    app.threePlayer = true;
    app.threePlayerAi = threePlayerAi;
    app.resultRecorded = false;
    app.game = Game{};
    app.threeGame = ThreePlayerGame(!threePlayerAi);
    app.sixGame = gomoku::SixPlayerGame{};
    app.lastMove.reset();
    app.undoHistory.clear();
    app.notationScroll = 0;
    app.status = threePlayerAi ? L"你执黑棋先手，两台电脑各自为战。"
                               : L"玩家1执黑先手，三人轮流落子。";
}

void startSixPlayerGame(AppState& app, bool pvc) {
    app.screen = Screen::Playing;
    app.pvc = pvc;
    app.threePlayer = false;
    app.threePlayerAi = false;
    app.sixPlayer = true;
    app.resultRecorded = false;
    app.game = Game{};
    app.threeGame = ThreePlayerGame{};
    app.sixGame = gomoku::SixPlayerGame{};
    app.lastMove.reset();
    app.undoHistory.clear();
    app.notationScroll = 0;
    app.status = pvc ? L"黑方先下一子；之后双方每回合各下两子。"
                     : L"黑方开局先下一子；之后双方每回合各下两子。";
}

void saveUndoSnapshot(AppState& app) {
    UndoSnapshot snapshot;
    snapshot.game = app.game;
    snapshot.threeGame = app.threeGame;
    snapshot.sixGame = app.sixGame;
    snapshot.threePlayer = app.threePlayer;
    snapshot.sixPlayer = app.sixPlayer;
    snapshot.threePlayerAi = app.threePlayerAi;
    snapshot.lastMove = app.lastMove;
    snapshot.notationScroll = app.notationScroll;
    snapshot.status = app.status;
    snapshot.screen = app.screen;
    app.undoHistory.push_back(std::move(snapshot));
}

bool undoLastTurn(AppState& app) {
    if (app.undoHistory.empty()) return false;
    UndoSnapshot snapshot = std::move(app.undoHistory.back());
    app.undoHistory.pop_back();
    app.game = std::move(snapshot.game);
    app.threeGame = std::move(snapshot.threeGame);
    app.sixGame = std::move(snapshot.sixGame);
    app.threePlayer = snapshot.threePlayer;
    app.sixPlayer = snapshot.sixPlayer;
    app.threePlayerAi = snapshot.threePlayerAi;
    app.lastMove = snapshot.lastMove;
    app.notationScroll = snapshot.notationScroll;
    app.status = std::move(snapshot.status);
    app.screen = snapshot.screen;
    return true;
}

Rect buttonRect(ButtonAction action, const AppState& app, const Layout& layout) {
    if (app.screen == Screen::Login) {
        switch (action) {
            case ButtonAction::Login: return layout.loginButtons[0];
            case ButtonAction::Register: return layout.loginButtons[1];
            case ButtonAction::Guest: return layout.loginButtons[2];
            case ButtonAction::Exit: return layout.loginExit;
            default: return {};
        }
    }
    if (app.screen == Screen::Menu) {
        if (action == ButtonAction::Logout) return layout.accountButton;
        if (app.menuPage == MenuPage::GameSelection) {
            switch (action) {
                case ButtonAction::SelectGomoku: return layout.menuButtons[0];
                case ButtonAction::SelectThreePlayer: return layout.menuButtons[1];
                case ButtonAction::SelectSixPlayer: return layout.menuButtons[2];
                case ButtonAction::Records: return layout.menuButtons[3];
                case ButtonAction::Exit: return layout.menuExit;
                default: return {};
            }
        }
        if (app.menuPage == MenuPage::Records) {
            if (action == ButtonAction::MainMenu) return layout.recordsBack;
            return {};
        }
        switch (action) {
            case ButtonAction::PvP: return layout.menuButtons[0];
            case ButtonAction::PvC: return layout.menuButtons[1];
            case ButtonAction::MainMenu: return layout.menuButtons[2];
            default: return {};
        }
    }
    switch (action) {
        case ButtonAction::Undo: return layout.buttons[0];
        case ButtonAction::Restart: return layout.buttons[1];
        case ButtonAction::MainMenu: return layout.buttons[2];
        default: return {};
    }
}

void invalidateRect(HWND window, Rect rect) {
    if (rect.width() > 0 && rect.height() > 0) {
        RECT dirty{rect.left - 2, rect.top - 2, rect.right + 2, rect.bottom + 2};
        InvalidateRect(window, &dirty, FALSE);
    }
}

void invalidateButton(HWND window, ButtonAction action, const AppState& app, const Layout& layout) {
    if (action != ButtonAction::None) invalidateRect(window, buttonRect(action, app, layout));
}

// 玩家落子后、AI 开始计算前，强制把当前棋盘和“电脑思考中”状态刷到屏幕。
// 否则 chooseMove 的同步搜索会阻塞消息循环，玩家会看到两手棋同时出现。
void beginAiTurn(HWND window, AppState& app, const Layout& layout) {
    app.status = L"电脑思考中，请稍候…";
    invalidateRect(window, layout.boardCard);
    invalidateRect(window, layout.sidePanel);
    UpdateWindow(window);
    // IDC_WAIT / IDC_ARROW 是资源整数宏；显式用 MAKEINTRESOURCEW 避免字符集不匹配。
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514)));
}

void endAiTurn() {
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
}

ButtonAction hitTest(const POINT point, const AppState& app, const Layout& layout) {
    if (app.screen == Screen::Login) {
        if (layout.loginButtons[0].contains(point)) return ButtonAction::Login;
        if (layout.loginButtons[1].contains(point)) return ButtonAction::Register;
        if (layout.loginButtons[2].contains(point)) return ButtonAction::Guest;
        if (layout.loginExit.contains(point)) return ButtonAction::Exit;
        return ButtonAction::None;
    }
    if (app.screen == Screen::Menu) {
        if (layout.accountButton.contains(point)) return ButtonAction::Logout;
        if (app.menuPage == MenuPage::GameSelection) {
            if (layout.menuButtons[0].contains(point)) return ButtonAction::SelectGomoku;
            if (layout.menuButtons[1].contains(point)) return ButtonAction::SelectThreePlayer;
            if (layout.menuButtons[2].contains(point)) return ButtonAction::SelectSixPlayer;
            if (layout.menuButtons[3].contains(point)) return ButtonAction::Records;
            if (layout.menuExit.contains(point)) return ButtonAction::Exit;
            return ButtonAction::None;
        }
        if (app.menuPage == MenuPage::Records) {
            if (layout.recordsBack.contains(point)) return ButtonAction::MainMenu;
            return ButtonAction::None;
        }
        if (layout.menuButtons[0].contains(point)) return ButtonAction::PvP;
        if (layout.menuButtons[1].contains(point)) return ButtonAction::PvC;
        if (layout.menuButtons[2].contains(point)) return ButtonAction::MainMenu;
        return ButtonAction::None;
    }
    for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
        if (i == 0 && app.undoHistory.empty()) continue;
        if (layout.buttons[i].contains(point)) {
            switch (i) {
                case 0: return ButtonAction::Undo;
                case 1: return ButtonAction::Restart;
                default: return ButtonAction::MainMenu;
            }
        }
    }
    return ButtonAction::None;
}

int nearestGridIndex(int coordinate, int origin, int extent, int boardSize) {
    int bestIndex = 0;
    int bestDistance = (std::numeric_limits<int>::max)();
    for (int index = 0; index < boardSize; ++index) {
        const int distance = std::abs(coordinate - gridPixel(origin, index, extent, boardSize));
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = index;
        }
    }
    return bestDistance <= extent / (boardSize - 1) / 2 ? bestIndex : -1;
}

void scrollNotationToLatest(HWND window, AppState& app) {
    RECT client{};
    GetClientRect(window, &client);
    const Layout layout = calculateLayout(client.right - client.left, client.bottom - client.top, app);
    const int visibleLines = notationVisibleLines(layout);
    app.notationScroll = std::max(0, static_cast<int>(app.moveHistory().size()) - visibleLines);
}

// 登录成功后的统一收尾：切回棋种选择页并刷新记录缓存。
void completeLogin(AppState& app, const std::wstring& message) {
    app.currentUser = fromUtf8(app.account.currentUsername());
    app.guest = false;
    app.screen = Screen::Menu;
    app.menuPage = MenuPage::GameSelection;
    app.loginMessage = message;
    app.loginError = false;
    app.loginPassword.clear();
    app.loginField = 0;
    app.status = L"请选择棋种和模式。";
    app.recordEntries = app.account.recentRecords(50);
    app.recordScroll = 0;
}

// 参数：app 为界面状态，registering 选择注册或登录。
// 实现：宽字符输入转 UTF-8 后调用 AccountManager，并映射返回状态到提示文字。
void submitCredentials(AppState& app, bool registering) {
    const std::string username = toUtf8(trimSpaces(app.loginName));
    const std::string password = toUtf8(app.loginPassword);
    app.loginError = true;
    if (registering) {
        switch (app.account.registerUser(username, password)) {
            case gomoku::RegisterStatus::Ok:
                completeLogin(app, L"注册成功，已自动登录。");
                break;
            case gomoku::RegisterStatus::InvalidUsername:
                app.loginMessage = L"用户名格式不正确：1-24 字节，不含空格与保留符号。";
                break;
            case gomoku::RegisterStatus::InvalidPassword:
                app.loginMessage = L"密码至少 4 个字符，最长 64 字节。";
                break;
            case gomoku::RegisterStatus::AlreadyExists:
                app.loginMessage = L"该用户名已存在，请直接登录。";
                break;
            case gomoku::RegisterStatus::StorageError:
                app.loginMessage = L"写入账号文件失败，请检查 data 目录权限。";
                break;
        }
    } else {
        switch (app.account.login(username, password)) {
            case gomoku::LoginStatus::Ok:
                completeLogin(app, L"登录成功，欢迎回来。");
                break;
            case gomoku::LoginStatus::InvalidCredentials:
                app.loginMessage = L"用户名或密码错误。";
                break;
            case gomoku::LoginStatus::StorageError:
                app.loginMessage = L"读取账号数据失败，请检查 data 目录。";
                break;
        }
    }
}

void enterAsGuest(AppState& app) {
    app.account.logout();
    app.guest = true;
    app.currentUser = L"游客";
    app.screen = Screen::Menu;
    app.menuPage = MenuPage::GameSelection;
    app.loginMessage = L"游客模式不会保存战绩。";
    app.loginError = false;
    app.loginPassword.clear();
    app.loginField = 0;
    app.recordEntries.clear();
    app.recordScroll = 0;
    app.status = L"游客模式：请选择棋种和模式。";
}

void logoutSession(AppState& app) {
    app.account.logout();
    app.guest = false;
    app.currentUser.clear();
    app.recordEntries.clear();
    app.recordScroll = 0;
    app.screen = Screen::Login;
    app.menuPage = MenuPage::GameSelection;
    app.loginMessage = L"已退出登录。";
    app.loginError = false;
    app.loginName.clear();
    app.loginPassword.clear();
    app.loginField = 0;
    app.hovered = ButtonAction::None;
    app.status = L"请登录或注册。";
}

// 对局结束时调用一次：按当前棋种把结果写入 data/records.txt。
// resultRecorded 防止同一局因重绘或悔棋后再次结束而重复计数。
void recordFinishedGame(AppState& app) {
    if (app.resultRecorded || !app.finished()) return;
    app.resultRecorded = true;
    if (!app.account.loggedIn()) return;

    gomoku::GameRecord record;
    if (app.sixPlayer) {
        record.game = "六子棋";
        record.mode = app.pvc ? "PvC" : "PvP";
        const auto winner = app.sixGame.winner();
        if (!winner) {
            record.outcome = gomoku::Outcome::Draw;
            record.detail = "平局";
        } else if (*winner == Stone::Black) {
            record.outcome = gomoku::Outcome::Win;
            record.detail = "黑方获胜";
        } else {
            record.outcome = gomoku::Outcome::Loss;
            record.detail = "白方获胜";
        }
        record.moves = app.sixGame.moveHistory();
    } else if (app.threePlayer) {
        record.game = "三人四子棋";
        record.mode = app.threePlayerAi ? "PvC" : "PvP";
        const auto winner = app.threeGame.winner();
        if (!winner) {
            record.outcome = gomoku::Outcome::Draw;
            record.detail = "平局";
        } else if (*winner == Stone::Black) {
            record.outcome = gomoku::Outcome::Win;
            record.detail = "黑棋获胜";
        } else {
            record.outcome = gomoku::Outcome::Loss;
            record.detail = *winner == Stone::White ? "白棋获胜" : "红棋获胜";
        }
        record.moves = app.threeGame.moveHistory();
    } else {
        record.game = "五子棋";
        record.mode = app.pvc ? "PvC" : "PvP";
        switch (app.game.result()) {
            case Result::BlackWins:
                record.outcome = gomoku::Outcome::Win;
                record.detail = "黑棋获胜";
                break;
            case Result::WhiteWins:
                record.outcome = gomoku::Outcome::Loss;
                record.detail = "白棋获胜";
                break;
            case Result::Draw:
            case Result::Ongoing:
                record.outcome = gomoku::Outcome::Draw;
                record.detail = "平局";
                break;
        }
        record.moves = app.game.moveHistory();
    }

    app.account.recordGame(record);
    app.recordEntries = app.account.recentRecords(50);
}

bool placeThreePlayerMove(HWND window, AppState& app, const Layout& layout, POINT point) {
    if (app.threePlayerAi && app.threeGame.currentPlayer() != Stone::Black) return false;
    const int col = nearestGridIndex(point.x, layout.boardGrid.left, layout.boardGrid.width(), app.boardSize());
    const int row = nearestGridIndex(point.y, layout.boardGrid.top, layout.boardGrid.height(), app.boardSize());
    if (row < 0 || col < 0) return false;
    const Move move{row, col};
    if (!app.threeGame.board().isEmpty(move)) {
        app.status = L"这个交叉点已有棋子，请选择空位。";
        invalidateRect(window, layout.sidePanel);
        return true;
    }
    if (!app.threePlayerAi && app.threeGame.violatesOpenThreeRule(move)) {
        app.status = L"其他玩家已有死三或活三，本手不能形成活三。";
        invalidateRect(window, layout.sidePanel);
        return true;
    }
    saveUndoSnapshot(app);
    if (!app.threeGame.play(move)) {
        app.undoHistory.pop_back();
        return true;
    }
    app.lastMove = move;
    if (app.threePlayerAi) {
        beginAiTurn(window, app, layout);
        while (!app.threeGame.finished() && app.threeGame.currentPlayer() != Stone::Black) {
            const Stone aiStone = app.threeGame.currentPlayer();
            const auto aiMove = app.threeAi.chooseMove(app.threeGame.board(), aiStone);
            if (aiMove && app.threeGame.play(*aiMove)) {
                app.lastMove = *aiMove;
                continue;
            }
            const std::vector<Move> fallback = app.threeGame.board().emptyMoves();
            if (fallback.empty() || !app.threeGame.play(fallback.front())) break;
            app.lastMove = fallback.front();
        }
        endAiTurn();
    }
    scrollNotationToLatest(window, app);
    if (app.threeGame.finished()) {
        app.screen = Screen::Finished;
        app.status = app.threeGame.winner() ? L"对局结束。" : L"棋盘下满，平局。";
    } else if (app.threePlayerAi) {
        app.status = L"电脑已落子，轮到你了。";
    } else {
        app.status = L"落子成功。";
    }
    recordFinishedGame(app);
    invalidateRect(window, layout.boardCard);
    invalidateRect(window, layout.sidePanel);
    return true;
}

bool placeSixPlayerMove(HWND window, AppState& app, const Layout& layout, POINT point) {
    if (app.pvc && app.sixGame.currentPlayer() != Stone::Black) return false;
    const int col = nearestGridIndex(point.x, layout.boardGrid.left, layout.boardGrid.width(), app.boardSize());
    const int row = nearestGridIndex(point.y, layout.boardGrid.top, layout.boardGrid.height(), app.boardSize());
    if (row < 0 || col < 0) return false;
    const Move move{row, col};
    if (!app.sixGame.board().isEmpty(move)) {
        app.status = L"这个交叉点已有棋子，请选择空位。";
        invalidateRect(window, layout.sidePanel);
        return true;
    }
    // PvC 的悔棋回到玩家本回合开始处：双子回合的第二子不另存快照。
    const bool saveSnapshot = !app.pvc || app.sixGame.movesRemaining() != 1 ||
                              app.sixGame.board().moveCount() == 0;
    if (saveSnapshot) saveUndoSnapshot(app);
    if (!app.sixGame.play(move)) {
        if (saveSnapshot) app.undoHistory.pop_back();
        return true;
    }
    app.lastMove = move;
    if (app.pvc && !app.sixGame.finished() && app.sixGame.currentPlayer() == Stone::White) {
        beginAiTurn(window, app, layout);
        const int aiMoves = app.sixGame.movesRemaining();
        for (int i = 0; i < aiMoves && !app.sixGame.finished(); ++i) {
            const auto aiMove = app.sixAi.chooseMove(app.sixGame.board(), Stone::White,
                                                      app.sixGame.movesRemaining());
            if (!aiMove || !app.sixGame.play(*aiMove)) {
                app.status = L"电脑未能生成合法落子。";
                break;
            }
            app.lastMove = *aiMove;
        }
        endAiTurn();
    }
    scrollNotationToLatest(window, app);
    if (app.sixGame.finished()) {
        app.screen = Screen::Finished;
        app.status = app.sixGame.winner() ? L"对局结束。" : L"棋盘下满，平局。";
    } else {
        app.status = app.pvc ? L"电脑已完成本回合，轮到你了。" : L"落子成功。";
    }
    recordFinishedGame(app);
    invalidateRect(window, layout.boardCard);
    invalidateRect(window, layout.sidePanel);
    return true;
}

bool placeHumanMove(HWND window, AppState& app, const Layout& layout, POINT point) {
    if (app.screen != Screen::Playing) return false;
    if (app.sixPlayer) return placeSixPlayerMove(window, app, layout, point);
    if (app.threePlayer) return placeThreePlayerMove(window, app, layout, point);
    if (app.pvc && app.currentPlayer() != Stone::Black) return false;
    const int col = nearestGridIndex(point.x, layout.boardGrid.left, layout.boardGrid.width(), app.boardSize());
    const int row = nearestGridIndex(point.y, layout.boardGrid.top, layout.boardGrid.height(), app.boardSize());
    if (row < 0 || col < 0) return false;
    const Move move{row, col};
    if (!app.board().isEmpty(move)) {
        app.status = L"这个交叉点已有棋子，请选择空位。";
        invalidateRect(window, layout.sidePanel);
        return true;
    }
    saveUndoSnapshot(app);
    if (!app.game.play(move)) {
        app.undoHistory.pop_back();
        return true;
    }
    scrollNotationToLatest(window, app);
    app.lastMove = move;
    app.status = L"已落子。";
    if (app.game.result() != Result::Ongoing) {
        app.screen = Screen::Finished;
    } else if (app.pvc) {
        // 先让玩家这一手显示出来，再进入可能持续数百毫秒到数秒的 AI 搜索。
        beginAiTurn(window, app, layout);
        const auto aiMove = app.ai.chooseMove(app.game.board(), Stone::White);
        if (aiMove && app.game.play(*aiMove)) {
            scrollNotationToLatest(window, app);
            app.lastMove = *aiMove;
            app.status = L"电脑已落子。轮到你了。";
        } else {
            app.status = L"电脑没有可用落点。";
        }
        if (app.game.result() != Result::Ongoing) app.screen = Screen::Finished;
        endAiTurn();
    } else {
        app.status = L"落子成功。";
    }
    recordFinishedGame(app);
    invalidateRect(window, layout.boardCard);
    invalidateRect(window, layout.sidePanel);
    return true;
}

// 参数：window 为窗口句柄，app 为界面状态，action 为按钮动作，layout 为当前布局。
// 实现：所有按钮只在此处集中分派；选择棋种/模式负责切页与初始化，Undo 恢复
// 快照，Restart 按当前模式重开，MainMenu 清空状态，Exit 发送关闭消息。
void performAction(HWND window, AppState& app, ButtonAction action, const Layout& layout) {
    switch (action) {
        case ButtonAction::Login:
            submitCredentials(app, false);
            break;
        case ButtonAction::Register:
            submitCredentials(app, true);
            break;
        case ButtonAction::Guest:
            enterAsGuest(app);
            break;
        case ButtonAction::Logout:
            logoutSession(app);
            break;
        case ButtonAction::Records:
            app.menuPage = MenuPage::Records;
            app.recordEntries = app.account.recentRecords(50);
            app.recordScroll = 0;
            app.status = L"查看对局记录。";
            break;
        case ButtonAction::SelectGomoku:
            app.selectedGame = GameType::Gomoku;
            app.menuPage = MenuPage::ModeSelection;
            app.status = L"已选择五子棋，请选择 PvP 或 PvC。";
            break;
        case ButtonAction::SelectThreePlayer:
            app.selectedGame = GameType::ThreePlayer;
            app.menuPage = MenuPage::ModeSelection;
            app.status = L"已选择三人四子棋，请选择 PvP 或 PvC。";
            break;
        case ButtonAction::SelectSixPlayer:
            app.selectedGame = GameType::SixPlayer;
            app.menuPage = MenuPage::ModeSelection;
            app.status = L"已选择六子棋，请选择 PvP 或 PvC。";
            break;
        case ButtonAction::PvP:
            if (app.selectedGame == GameType::Gomoku) startGame(app, false);
            else if (app.selectedGame == GameType::ThreePlayer) startThreePlayerGame(app, false);
            else startSixPlayerGame(app, false);
            break;
        case ButtonAction::PvC:
            if (app.selectedGame == GameType::Gomoku) startGame(app, true);
            else if (app.selectedGame == GameType::ThreePlayer) startThreePlayerGame(app, true);
            else startSixPlayerGame(app, true);
            break;
        case ButtonAction::Undo: {
            if (!undoLastTurn(app)) {
                app.status = L"当前没有可悔的棋。";
                invalidateRect(window, layout.sidePanel);
                return;
            }
            invalidateRect(window, layout.boardCard);
            invalidateRect(window, layout.sidePanel);
            return;
        }
        case ButtonAction::Restart:
            if (app.sixPlayer) startSixPlayerGame(app, app.pvc);
            else if (app.threePlayer) startThreePlayerGame(app, app.threePlayerAi);
            else startGame(app, app.pvc);
            break;
        case ButtonAction::MainMenu:
            app.screen = Screen::Menu;
            app.menuPage = MenuPage::GameSelection;
            app.pvc = false;
            app.sixPlayer = false;
            app.threePlayer = false;
            app.threePlayerAi = false;
            app.resultRecorded = false;
            app.game = Game{};
            app.threeGame = ThreePlayerGame{};
            app.sixGame = gomoku::SixPlayerGame{};
            app.lastMove.reset();
            app.undoHistory.clear();
            app.status = L"请先选择游戏种类。";
            break;
        case ButtonAction::Exit:
            DestroyWindow(window);
            return;
        case ButtonAction::None:
            return;
    }
    InvalidateRect(window, nullptr, FALSE);
}

// 登录页字符输入：回车登录，Tab/点击切换输入框，退格删除，其余字符追加。
// 用户名与密码分别按 UTF-8 24/64 字节截断，与 AccountManager 的校验一致。
void handleLoginChar(AppState& app, wchar_t ch) {
    std::wstring& field = app.loginField == 0 ? app.loginName : app.loginPassword;
    if (ch == L'\r' || ch == L'\n') {
        submitCredentials(app, false);
        return;
    }
    if (ch == 8 || ch == 127) {
        if (!field.empty()) field.pop_back();
        return;
    }
    if (ch < 32) return;
    const std::size_t limit = app.loginField == 0 ? 24u : 64u;
    field.push_back(ch);
    if (toUtf8(field).size() > limit) field.pop_back();
}

// 参数：标准 Win32 窗口过程参数。
// 返回：已处理消息返回 0，未处理消息交给 DefWindowProc。
// 实现：WM_PAINT 双缓冲绘制；WM_LBUTTONUP 做按钮/棋盘命中测试；
// WM_MOUSEMOVE/WM_MOUSELEAVE 只重绘悬停按钮；WM_MOUSEWHEEL 滚动棋谱；
// WM_KEYDOWN 处理 Esc；WM_DPICHANGED 重新计算布局和窗口尺寸。
LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    AppState* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* createInfo = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        app = static_cast<AppState*>(createInfo->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app == nullptr) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
        case WM_PAINT:
            paintWindow(window, *app);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize.x = MulDiv(900, static_cast<int>(app->dpi), 96);
            limits->ptMinTrackSize.y = MulDiv(650, static_cast<int>(app->dpi), 96);
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
            RECT client{};
            GetClientRect(window, &client);
            const Layout layout = calculateLayout(client.right - client.left, client.bottom - client.top, *app);
            const POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            const ButtonAction hovered = hitTest(point, *app, layout);
            if (hovered != app->hovered) {
                const ButtonAction previous = app->hovered;
                app->hovered = hovered;
                invalidateButton(window, previous, *app, layout);
                invalidateButton(window, hovered, *app, layout);
                if (hovered == ButtonAction::Undo && app->undoHistory.empty()) {
                    invalidateRect(window, layout.buttons[0]);
                }
            }
            TRACKMOUSEEVENT tracking{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
            TrackMouseEvent(&tracking);
            return 0;
        }
        case WM_MOUSELEAVE: {
            const ButtonAction previous = app->hovered;
            app->hovered = ButtonAction::None;
            RECT client{};
            GetClientRect(window, &client);
            const Layout layout = calculateLayout(client.right - client.left, client.bottom - client.top, *app);
            invalidateButton(window, previous, *app, layout);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &screenPoint);
            RECT client{};
            GetClientRect(window, &client);
            const Layout layout = calculateLayout(client.right - client.left, client.bottom - client.top, *app);
            if (app->screen == Screen::Menu && app->menuPage == MenuPage::Records &&
                layout.recordsPanel.contains(screenPoint)) {
                const int lineHeight = std::max(1, px(30, app->dpi));
                const int listTop = layout.recordsPanel.top + px(62, app->dpi);
                const int listBottom = layout.recordsBack.top - px(14, app->dpi);
                const int visibleLines = std::max(0, (listBottom - listTop) / lineHeight);
                const int maxScroll =
                    std::max(0, static_cast<int>(app->recordEntries.size()) - visibleLines);
                const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                const int wheelSteps = std::max(1, std::abs(wheelDelta) / WHEEL_DELTA) *
                                       (wheelDelta > 0 ? 1 : -1);
                app->recordScroll = std::clamp(app->recordScroll - wheelSteps * 3, 0, maxScroll);
                RECT dirty{layout.recordsPanel.left, layout.recordsPanel.top,
                           layout.recordsPanel.right, layout.recordsPanel.bottom};
                InvalidateRect(window, &dirty, FALSE);
                return 0;
            }
            if (app->screen != Screen::Menu && layout.notationList.contains(screenPoint)) {
                const int visibleLines = notationVisibleLines(layout);
                const int maxScroll = std::max(0, static_cast<int>(app->moveHistory().size()) - visibleLines);
                const int wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                const int wheelSteps = std::max(1, std::abs(wheelDelta) / WHEEL_DELTA) *
                                       (wheelDelta > 0 ? 1 : -1);
                app->notationScroll = std::clamp(app->notationScroll - wheelSteps * 3, 0, maxScroll);
                RECT dirty{layout.notationList.left, layout.notationList.top,
                           layout.notationList.right, layout.notationList.bottom};
                InvalidateRect(window, &dirty, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            RECT client{};
            GetClientRect(window, &client);
            const Layout layout = calculateLayout(client.right - client.left, client.bottom - client.top, *app);
            const POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            if (app->screen == Screen::Login) {
                if (layout.loginUsername.contains(point)) {
                    app->loginField = 0;
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
                if (layout.loginPassword.contains(point)) {
                    app->loginField = 1;
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
            }
            const ButtonAction action = hitTest(point, *app, layout);
            if (action != ButtonAction::None) {
                performAction(window, *app, action, layout);
            } else if (app->screen == Screen::Playing) {
                (void)placeHumanMove(window, *app, layout, point);
            }
            return 0;
        }
        case WM_CHAR:
            if (app->screen != Screen::Login) return 0;
            handleLoginChar(*app, static_cast<wchar_t>(wParam));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (app->screen == Screen::Login && wParam == VK_TAB) {
                // Tab 在 WM_KEYDOWN 中处理；随后的 WM_CHAR('\t') 会被忽略，
                // 避免一次按键切换两次焦点。
                app->loginField = app->loginField == 0 ? 1 : 0;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                if (app->screen == Screen::Login) {
                    DestroyWindow(window);
                } else if (app->screen == Screen::Menu) {
                    if (app->menuPage == MenuPage::GameSelection) {
                        DestroyWindow(window);
                    } else {
                        app->menuPage = MenuPage::GameSelection;
                        app->status = L"请先选择游戏种类。";
                        InvalidateRect(window, nullptr, FALSE);
                    }
                } else {
                    app->screen = Screen::Menu;
                    app->menuPage = MenuPage::GameSelection;
                    app->pvc = false;
                    app->threePlayer = false;
                    app->threePlayerAi = false;
                    app->sixPlayer = false;
                    app->resultRecorded = false;
                    app->game = Game{};
                    app->threeGame = ThreePlayerGame{};
                    app->sixGame = gomoku::SixPlayerGame{};
                    app->lastMove.reset();
                    app->undoHistory.clear();
                    app->status = L"请先选择游戏种类。";
                    InvalidateRect(window, nullptr, FALSE);
                }
                return 0;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

// 程序入口：注册窗口类、按 DPI 调整初始窗口大小、创建窗口并进入消息循环。
// 返回：消息循环结束时 GetMessage 的返回值（通常为 0）。
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const WNDCLASSEXW windowClass{
        sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, windowProcedure, 0, 0, instance,
        LoadIconW(nullptr, MAKEINTRESOURCEW(32512)), LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
        CreateSolidBrush(kBackground), nullptr, kWindowClass, LoadIconW(nullptr, MAKEINTRESOURCEW(32512))};
    if (RegisterClassExW(&windowClass) == 0) return 1;

    const UINT dpi = GetDpiForSystem();
    RECT workArea{0, 0, MulDiv(1220, static_cast<int>(dpi), 96),
                  MulDiv(850, static_cast<int>(dpi), 96)};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) == FALSE) {
        workArea = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    // 高 DPI 下按逻辑尺寸放大的窗口可能超出屏幕，这里按工作区裁剪并居中，
    // 保证登录页底部的“游客进入 / 退出游戏”始终可见。
    RECT windowRect{0, 0, MulDiv(1220, static_cast<int>(dpi), 96),
                    MulDiv(850, static_cast<int>(dpi), 96)};
    AdjustWindowRectExForDpi(&windowRect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int workWidth = workArea.right - workArea.left;
    const int workHeight = workArea.bottom - workArea.top;
    const int windowWidth = std::min<int>(windowRect.right - windowRect.left,
                                          std::max(px(700, dpi), workWidth - px(24, dpi)));
    const int windowHeight = std::min<int>(windowRect.bottom - windowRect.top,
                                           std::max(px(560, dpi), workHeight - px(24, dpi)));
    const int windowX = workArea.left + (workWidth - windowWidth) / 2;
    const int windowY = workArea.top + (workHeight - windowHeight) / 2;
    AppState app;
    app.dpi = dpi;
    HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                  windowX, windowY, windowWidth, windowHeight,
                                  nullptr, nullptr, instance, &app);
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
