// 控制台前端：组合三个游戏模块，提供文字棋盘、键盘输入和主菜单。
// 本文件只负责输入输出与流程控制，规则和 AI 均调用各模块的类。
#include "gomoku/classic/game.hpp"
#include "gomoku/core/account.hpp"
#include "gomoku/core/input.hpp"
#include "gomoku/three_player/game.hpp"
#include "gomoku/three_player/ai.hpp"
#include "gomoku/six_player/game.hpp"
#include "gomoku/six_player/ai.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace {

using gomoku::AiPlayer;
using gomoku::Board;
using gomoku::Game;
using gomoku::Move;
using gomoku::Result;
using gomoku::Stone;
using gomoku::ThreePlayerAi;
using gomoku::ThreePlayerGame;
using gomoku::SixPlayerGame;
using gomoku::SixPlayerAi;

enum class ReadStatus { Line, Escape, EndOfInput };
struct ReadResult {
    ReadStatus status;
    std::string text;
};

#ifdef _WIN32
int readCharacter() {
    const int ch = _getch();
    if (ch == 0 || ch == 224) {
        (void)_getch();
        return 0;
    }
    return ch;
}
#else
class TerminalMode {
public:
    TerminalMode() {
        if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &original_) == 0) {
            termios raw = original_;
            raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            active_ = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
        }
    }

    ~TerminalMode() {
        restore();
    }

    void restore() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_);
            active_ = false;
        }
    }

private:
    termios original_{};
    bool active_ = false;
};

int readCharacter() {
    unsigned char ch = 0;
    const ssize_t bytes = ::read(STDIN_FILENO, &ch, 1);
    if (bytes != 1) return -1;
    return ch;
}
#endif

// 参数：prompt 为提示文字，mask 为 true 时输入密码并显示星号。
// 返回：状态（正常输入/按 Esc/输入结束）及读到的整行文本。
// 实现：Windows 用 _getch 逐字符读取以捕获 Esc；POSIX 用原始终端模式。
// 退格只删除缓冲区末尾字符，回车结束输入，避免行编辑库依赖。
ReadResult readLine(const std::string& prompt, bool mask = false) {
#ifndef _WIN32
    TerminalMode terminalMode;
    (void)terminalMode;
#endif
    std::cout << prompt << std::flush;
    std::string line;
    while (true) {
        const int ch = readCharacter();
        if (ch < 0) return {ReadStatus::EndOfInput, {}};
        if (ch == 27) {
            std::cout << '\n';
            return {ReadStatus::Escape, {}};
        }
        if (ch == '\r' || ch == '\n') {
            std::cout << '\n';
            return {ReadStatus::Line, std::move(line)};
        }
        if (ch == 8 || ch == 127) {
            if (!line.empty()) {
                line.pop_back();
                std::cout << "\b \b" << std::flush;
            }
            continue;
        }
        if (ch >= 32 && ch <= 126) {
            line.push_back(static_cast<char>(ch));
            std::cout << (mask ? '*' : static_cast<char>(ch)) << std::flush;
        }
    }
}

std::string stoneSymbol(Stone stone) {
    if (stone == Stone::Black) return "●";
    if (stone == Stone::White) return "○";
    if (stone == Stone::Red) return "◆";
    return "+";
}

// 参数：board 为待显示棋盘，threePlayer 决定表头是否显示红方。
// 实现：先输出列号，再逐行输出行号和棋子字符；只做展示，不修改棋盘。
void drawBoard(const Board& board, bool threePlayer = false) {
    std::cout << "\n    ";
    for (int col = 1; col <= board.size(); ++col) {
        if (col < 10) std::cout << ' ';
        std::cout << col << ' ';
    }
    std::cout << '\n';
    for (int row = 0; row < board.size(); ++row) {
        if (row + 1 < 10) std::cout << ' ';
        std::cout << row + 1 << "  ";
        for (int col = 0; col < board.size(); ++col) {
            std::cout << stoneSymbol(board.at({row, col})) << "  ";
        }
        std::cout << '\n';
    }
    if (threePlayer) {
        std::cout << "\n符号：黑棋 ●  白棋 ○  红棋 ◆\n";
    } else {
        std::cout << "\n符号：黑棋 ●  白棋 ○\n";
    }
}

const char* playerName(Stone stone, bool pvc) {
    if (pvc && stone == Stone::White) return "电脑（白棋）";
    return stone == Stone::Black ? "玩家1（黑棋）" : "玩家2（白棋）";
}

const char* threePlayerName(Stone stone) {
    switch (stone) {
        case Stone::Black: return "玩家1（黑棋）";
        case Stone::White: return "玩家2（白棋）";
        case Stone::Red: return "玩家3（红棋）";
        case Stone::Empty: break;
    }
    return "玩家";
}

const char* sixPlayerName(Stone stone, bool pvc) {
    if (stone == Stone::Black) return pvc ? "你（黑方）" : "黑方";
    return pvc ? "电脑（白方）" : "白方";
}

const char* threePlayerAiName(Stone stone) {
    switch (stone) {
        case Stone::Black: return "你（黑棋）";
        case Stone::White: return "电脑1（白棋）";
        case Stone::Red: return "电脑2（红棋）";
        case Stone::Empty: break;
    }
    return "玩家";
}

// 参数：board 为当前棋盘，move 用于输出解析后的 0 基坐标。
// 返回：读到合法坐标返回 true；按 Esc 或输入结束返回 false。
// 实现：循环调用 readLine，再用 parseMoveInput 区分格式错误、越界和合法输入。
bool getHumanMove(const Board& board, Move& move) {
    const std::string coordinateRange = "1-" + std::to_string(board.size());
    const std::string prompt = "输入行 列（" + coordinateRange + "），如：8 8（按 Esc 退出）：";
    while (true) {
        const ReadResult input = readLine(prompt);
        if (input.status != ReadStatus::Line) return false;
        const gomoku::MoveInputStatus status = gomoku::parseMoveInput(input.text, move, board.size());
        if (status == gomoku::MoveInputStatus::InvalidFormat) {
            std::cout << "输入格式错误，请输入两个整数，例如：8 8。\n";
            continue;
        }
        if (status == gomoku::MoveInputStatus::OutOfRange || !board.inBounds(move)) {
            std::cout << "坐标越界，行和列必须在 1 到 " << board.size() << " 之间。\n";
            continue;
        }
        if (!board.isEmpty(move)) {
            std::cout << "该位置已有棋子，请选择空位。\n";
            continue;
        }
        return true;
    }
}

bool getHumanMove(const Game& game, Move& move) {
    return getHumanMove(game.board(), move);
}

void announceResult(Result result, bool pvc) {
    switch (result) {
        case Result::BlackWins:
            std::cout << (pvc ? "玩家（黑棋）获胜！\n" : "玩家1（黑棋）获胜！\n");
            break;
        case Result::WhiteWins:
            std::cout << (pvc ? "电脑（白棋）获胜！\n" : "玩家2（白棋）获胜！\n");
            break;
        case Result::Draw:
            std::cout << "棋盘下满，平局！\n";
            break;
        case Result::Ongoing:
            break;
    }
}

void announceThreePlayerResult(const ThreePlayerGame& game) {
    const auto winner = game.winner();
    if (winner) {
        std::cout << threePlayerName(*winner) << "获胜！\n";
    } else {
        std::cout << "棋盘下满，平局！\n";
    }
}

void announceThreePlayerAiResult(const ThreePlayerGame& game) {
    const auto winner = game.winner();
    if (!winner) {
        std::cout << "棋盘下满，平局！\n";
        return;
    }
    switch (*winner) {
        case Stone::Black: std::cout << "你赢了！\n"; break;
        case Stone::White: std::cout << "电脑1（白棋）获胜！\n"; break;
        case Stone::Red: std::cout << "电脑2（红棋）获胜！\n"; break;
        case Stone::Empty: break;
    }
}

// 登录状态下累计一局战绩；游客静默跳过。
void saveResult(gomoku::AccountManager& account, gomoku::Outcome outcome) {
    if (account.loggedIn() && !account.recordOutcome(outcome)) {
        std::cout << "警告：战绩写入失败，请检查 data 目录权限。\n";
    }
}

bool playGame(bool pvc, gomoku::AccountManager& account) {
    Game game;
    AiPlayer ai;
    std::cout << "\n=== " << (pvc ? "PvC 人机对战" : "PvP 玩家对战") << " ===\n"
              << "黑棋先手；输入行 列落子。等待输入时按 Esc 可立即退出程序。\n";
    drawBoard(game.board());

    while (game.result() == Result::Ongoing) {
        const Stone turn = game.currentPlayer();
        std::cout << "轮到：" << playerName(turn, pvc) << '\n';
        Move move;
        if (pvc && turn == Stone::White) {
            const auto choice = ai.chooseMove(game.board(), Stone::White);
            if (!choice || !game.play(*choice)) {
                std::cerr << "AI 无法生成合法落子，游戏已安全结束。\n";
                return false;
            }
            std::cout << "电脑落子：" << choice->row + 1 << ' ' << choice->col + 1 << '\n';
        } else {
            if (!getHumanMove(game, move)) return false;
            if (!game.play(move)) {
                // The board is single-threaded; this is a defensive fallback.
                std::cout << "落子失败，请重试。\n";
                continue;
            }
        }
        drawBoard(game.board());
    }

    const Result result = game.result();
    announceResult(result, pvc);
    gomoku::Outcome outcome = gomoku::Outcome::Draw;
    if (result == Result::BlackWins) outcome = gomoku::Outcome::Win;
    else if (result == Result::WhiteWins) outcome = gomoku::Outcome::Loss;
    saveResult(account, outcome);
    return true;
}

bool playSixPlayerGame(bool pvc, gomoku::AccountManager& account) {
    SixPlayerGame game;
    SixPlayerAi ai;
    std::cout << "\n=== 六子棋 · " << (pvc ? "PvC" : "PvP") << " ===\n"
              << "黑方先手只下一子；此后双方每回合各下两子，连成六子及以上获胜。\n";
    drawBoard(game.board());

    while (!game.finished()) {
        const Stone turn = game.currentPlayer();
        if (pvc && turn == Stone::White) {
            const auto move = ai.chooseMove(game.board(), Stone::White, game.movesRemaining());
            if (!move || !game.play(*move)) {
                std::cerr << "AI 无法生成合法落子，游戏已安全结束。\n";
                return false;
            }
            std::cout << "电脑落子：" << move->row + 1 << ' ' << move->col + 1 << '\n';
        } else {
            std::cout << "轮到：" << sixPlayerName(turn, pvc) << "，本回合还需落 "
                      << game.movesRemaining() << " 子。\n";
            Move move;
            if (!getHumanMove(game.board(), move)) return false;
            if (!game.play(move)) {
                std::cout << "落子失败，请重试。\n";
                continue;
            }
        }
        drawBoard(game.board());
    }

    const auto winner = game.winner();
    gomoku::Outcome outcome = gomoku::Outcome::Draw;
    if (winner) {
        std::cout << sixPlayerName(*winner, pvc) << "获胜！\n";
        outcome = *winner == Stone::Black ? gomoku::Outcome::Win : gomoku::Outcome::Loss;
    } else {
        std::cout << "棋盘下满，平局！\n";
    }
    saveResult(account, outcome);
    return true;
}

int readMenuChoice(const std::string& prompt, int minimum, int maximum);

bool runSixPlayerMode(bool pvc, gomoku::AccountManager& account) {
    while (true) {
        if (!playSixPlayerGame(pvc, account)) return false;
        const int choice = readMenuChoice("本局结束：1 重新开始  2 返回主菜单  3 退出（Esc 退出）：", 1, 3);
        if (choice == 0 || choice == 3) return false;
        if (choice == 2) return true;
    }
}

bool playThreePlayerGame(gomoku::AccountManager& account) {
    ThreePlayerGame game(true);
    std::cout << "\n=== 三人四子棋 · PvP（同屏） ===\n"
              << "黑棋先手，三人轮流落子；横、竖、斜任意方向连成四子及以上获胜。\n"
              << "同屏规则：若其他玩家已有死三或活三，本手不能形成新的活三"
                 "（直接成四获胜的手除外）。\n";
    drawBoard(game.board(), true);

    while (!game.finished()) {
        const Stone turn = game.currentPlayer();
        std::cout << "轮到：" << threePlayerName(turn) << '\n';
        Move move;
        if (!getHumanMove(game.board(), move)) return false;
        if (game.violatesOpenThreeRule(move)) {
            std::cout << "其他玩家已有死三或活三，本手不能形成活三。\n";
            continue;
        }
        if (!game.play(move)) {
            std::cout << "落子失败，请重试。\n";
            continue;
        }
        drawBoard(game.board(), true);
    }

    announceThreePlayerResult(game);
    const auto winner = game.winner();
    gomoku::Outcome outcome = gomoku::Outcome::Draw;
    if (winner == Stone::Black) outcome = gomoku::Outcome::Win;
    else if (winner) outcome = gomoku::Outcome::Loss;
    saveResult(account, outcome);
    return true;
}

bool playThreePlayerAiGame(gomoku::AccountManager& account) {
    ThreePlayerGame game;
    ThreePlayerAi ai;
    std::cout << "\n=== 三人四子棋 · PvC ===\n"
              << "你执黑先手，电脑1执白、电脑2执红；两台电脑各自为战。\n";
    drawBoard(game.board(), true);

    while (!game.finished()) {
        const Stone turn = game.currentPlayer();
        if (turn == Stone::Black) {
            std::cout << "轮到你（黑棋）\n";
            Move move;
            if (!getHumanMove(game.board(), move)) return false;
            if (!game.play(move)) {
                std::cout << "落子失败，请重试。\n";
                continue;
            }
        } else {
            std::cout << threePlayerAiName(turn) << "思考中...\n";
            const auto choice = ai.chooseMove(game.board(), turn);
            if (!choice || !game.play(*choice)) {
                std::cerr << "AI 无法生成合法落子，游戏已安全结束。\n";
                return false;
            }
            std::cout << threePlayerAiName(turn) << "落子："
                      << choice->row + 1 << ' ' << choice->col + 1 << '\n';
        }
        drawBoard(game.board(), true);
    }

    announceThreePlayerAiResult(game);
    const auto winner = game.winner();
    gomoku::Outcome outcome = gomoku::Outcome::Draw;
    if (winner == Stone::Black) outcome = gomoku::Outcome::Win;
    else if (winner) outcome = gomoku::Outcome::Loss;
    saveResult(account, outcome);
    return true;
}

int readMenuChoice(const std::string& prompt, int minimum, int maximum) {
    while (true) {
        const ReadResult input = readLine(prompt);
        if (input.status != ReadStatus::Line) return 0;
        int choice = 0;
        if (gomoku::parseMenuInput(input.text, choice) && choice >= minimum && choice <= maximum) return choice;
        std::cout << "选项无效，请输入 " << minimum << " 到 " << maximum << " 之间的单个数字。\n";
    }
}

bool runMode(bool pvc, gomoku::AccountManager& account) {
    while (true) {
        if (!playGame(pvc, account)) return false;
        const int choice = readMenuChoice("本局结束：1 重新开始  2 返回主菜单  3 退出（Esc 退出）：", 1, 3);
        if (choice == 0 || choice == 3) return false;
        if (choice == 2) return true;
    }
}

bool runThreePlayerMode(gomoku::AccountManager& account) {
    while (true) {
        if (!playThreePlayerGame(account)) return false;
        const int choice = readMenuChoice("本局结束：1 重新开始  2 返回主菜单  3 退出（Esc 退出）：", 1, 3);
        if (choice == 0 || choice == 3) return false;
        if (choice == 2) return true;
    }
}

bool runThreePlayerAiMode(gomoku::AccountManager& account) {
    while (true) {
        if (!playThreePlayerAiGame(account)) return false;
        const int choice = readMenuChoice("本局结束：1 重新开始  2 返回主菜单  3 退出（Esc 退出）：", 1, 3);
        if (choice == 0 || choice == 3) return false;
        if (choice == 2) return true;
    }
}

// 参数：prompt 为提示文字，mask 为密码输入控制。
// 返回：读到整行返回文本；按 Esc 或输入结束返回空（调用方回到登录菜单）。
std::optional<std::string> readField(const std::string& prompt, bool mask) {
    const ReadResult input = readLine(prompt, mask);
    if (input.status != ReadStatus::Line) return std::nullopt;
    return input.text;
}

// 参数：account 为账号管理器。
// 返回：登录或选择游客后返回 true；用户要求退出程序时返回 false。
// 实现：登录、注册、游客三种入口循环显示；密码输入以 * 回显。
bool loginFlow(gomoku::AccountManager& account) {
    while (true) {
        std::cout << "\n========== 用户登录 ==========\n"
                  << "1：登录\n"
                  << "2：注册新账号\n"
                  << "3：游客进入（不保存战绩）\n"
                  << "4：退出游戏\n";
        const int choice = readMenuChoice("请选择（按 Esc 退出）：", 1, 4);
        if (choice == 0 || choice == 4) return false;
        if (choice == 3) {
            account.logout();
            std::cout << "已以游客身份进入，对局不会保存。\n";
            return true;
        }
        if (!account.storageReady()) {
            std::cout << "警告：data 目录不可写，账号功能暂不可用。\n";
        }
        const auto username = readField(choice == 2
                                            ? "设置用户名（1-24 字节，不含空格）："
                                            : "用户名（按 Esc 返回）：",
                                        false);
        if (!username) continue;
        const auto password = readField(choice == 2 ? "设置密码（至少 4 个字符）：" : "密码：",
                                        true);
        if (!password) continue;

        if (choice == 1) {
            switch (account.login(*username, *password)) {
                case gomoku::LoginStatus::Ok:
                    std::cout << "登录成功，欢迎 " << account.currentUsername() << "！\n";
                    return true;
                case gomoku::LoginStatus::InvalidCredentials:
                    std::cout << "用户名或密码错误。\n";
                    break;
                case gomoku::LoginStatus::StorageError:
                    std::cout << "读取账号数据失败，请检查 data 目录。\n";
                    break;
            }
        } else {
            switch (account.registerUser(*username, *password)) {
                case gomoku::RegisterStatus::Ok:
                    std::cout << "注册成功，已自动登录：" << account.currentUsername() << "。\n";
                    return true;
                case gomoku::RegisterStatus::InvalidUsername:
                    std::cout << "用户名无效：" << gomoku::validateUsername(*username) << "。\n";
                    break;
                case gomoku::RegisterStatus::InvalidPassword:
                    std::cout << "密码无效：" << gomoku::validatePassword(*password) << "。\n";
                    break;
                case gomoku::RegisterStatus::AlreadyExists:
                    std::cout << "该用户名已存在，请直接登录。\n";
                    break;
                case gomoku::RegisterStatus::StorageError:
                    std::cout << "写入账号文件失败，请检查 data 目录权限。\n";
                    break;
            }
        }
    }
}

// 参数：account 为已登录或游客会话。
// 返回：false 表示退出程序；true 表示退出登录并回到登录界面。
bool runMainMenu(gomoku::AccountManager& account) {
    while (true) {
        std::cout << "\n========== 主菜单 ==========\n";
        if (account.loggedIn()) {
            std::cout << "当前用户：" << account.currentUsername();
            if (const auto stats = account.currentStats()) {
                std::cout << "（胜 " << stats->wins << "  负 " << stats->losses
                          << "  平 " << stats->draws << "）";
            }
            std::cout << '\n';
        } else {
            std::cout << "当前用户：游客（对局不保存）\n";
        }
        std::cout << "1：五子棋 PvP\n"
                  << "2：五子棋 PvC（玩家执黑先手）\n"
                  << "3：三人四子棋 PvP（三人同屏）\n"
                  << "4：三人四子棋 PvC（1 人 + 2 电脑）\n"
                  << "5：六子棋 PvP\n"
                  << "6：六子棋 PvC（玩家执黑先手）\n"
                  << "7：退出登录（切换账号）\n"
                  << "8：退出游戏\n";
        const int choice = readMenuChoice("请选择（按 Esc 退出）：", 1, 8);
        if (choice == 0 || choice == 8) return false;
        if (choice == 7) {
            account.logout();
            std::cout << "已退出登录。\n";
            return true;
        }
        if (choice == 3) {
            if (!runThreePlayerMode(account)) return false;
            continue;
        }
        if (choice == 4) {
            if (!runThreePlayerAiMode(account)) return false;
            continue;
        }
        if (choice == 5 || choice == 6) {
            if (!runSixPlayerMode(choice == 6, account)) return false;
            continue;
        }
        if (!runMode(choice == 2, account)) return false;
    }
}

}  // namespace

// 程序入口：先进入登录/注册界面，登录后在主菜单选择棋种与模式。
// 对局结束后累计当前用户战绩。
int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::cout << "欢迎来到五子棋！\n";
    gomoku::AccountManager account("data");
    if (!account.load()) {
        std::cerr << "警告：无法创建 data 目录，账号与战绩将不可用；仍可游客游玩。\n";
    }
    while (true) {
        if (!loginFlow(account)) break;
        if (!runMainMenu(account)) break;
    }
    std::cout << "感谢游玩，再见！\n";
    return 0;
}
