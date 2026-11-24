// 账号与数据文件层测试：校验规则、注册/登录、加盐摘要、战绩累计、
// 对局记录读写、多用户隔离，以及重新加载后的持久化。
#include "gomoku/core/account.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[失败] " << message << '\n';
        ++failures;
    }
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::filesystem::path makeRootDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("gomoku_account_test_" + std::to_string(stamp));
}

gomoku::GameRecord makeRecord(gomoku::Outcome outcome) {
    gomoku::GameRecord record;
    record.game = "五子棋";
    record.mode = "PvC";
    record.outcome = outcome;
    record.detail = outcome == gomoku::Outcome::Win    ? "黑棋获胜"
                    : outcome == gomoku::Outcome::Loss ? "白棋获胜"
                                                       : "平局";
    record.moves.push_back({{7, 7}, gomoku::Stone::Black});
    record.moves.push_back({{7, 8}, gomoku::Stone::White});
    return record;
}

void testPasswordDigest() {
    const std::string digest =
        gomoku::passwordDigest("00112233445566778899aabbccddeeff", "secret");
    check(digest ==
              "33dac63863d5b935bbe4a2f6074b71dcea30e071d25510bece227a8c7acec1e9",
          "加盐迭代 SHA-256 与参考实现一致");
    check(digest.size() == 64, "摘要为 64 位十六进制");
    check(gomoku::passwordDigest("00112233445566778899aabbccddeeff", "Secret") != digest,
          "大小写不同的口令得到不同摘要");
}

void testValidation() {
    check(!gomoku::validateUsername("").empty(), "拒绝空用户名");
    check(!gomoku::validateUsername("bad name").empty(), "拒绝含空格用户名");
    check(!gomoku::validateUsername("bad|name").empty(), "拒绝含分隔符用户名");
    check(!gomoku::validateUsername(std::string(25, 'a')).empty(), "拒绝过长用户名");
    check(gomoku::validateUsername("player_1").empty(), "接受字母数字下划线用户名");
    check(gomoku::validateUsername("棋士").empty(), "接受 UTF-8 中文用户名");

    check(!gomoku::validatePassword("123").empty(), "拒绝过短密码");
    check(!gomoku::validatePassword(std::string(65, 'a')).empty(), "拒绝过长密码");
    check(gomoku::validatePassword("1234").empty(), "接受四字符密码");
    check(gomoku::formatMove({7, 7}) == "H8", "坐标格式化为 H8");
}

void testRegisterAndLogin(const std::filesystem::path& root) {
    const std::filesystem::path directory = root / "register";
    gomoku::AccountManager account(directory);
    check(account.load(), "创建数据目录");
    check(account.users().empty(), "初始没有账号");
    check(!account.loggedIn(), "加载后处于未登录状态");

    check(account.registerUser("alice", "secret") == gomoku::RegisterStatus::Ok,
          "注册 alice 成功");
    check(account.loggedIn(), "注册后自动登录");
    check(account.currentUsername() == "alice", "当前用户为 alice");
    check(account.users().size() == 1, "账号数量为 1");

    const std::string usersText = readTextFile(directory / "users.txt");
    check(usersText.find("alice") != std::string::npos, "users.txt 写入用户名");
    check(usersText.find("secret") == std::string::npos, "users.txt 不保存明文密码");
    check(account.users().front().passwordHash.size() == 64, "密码摘要为 64 位十六进制");

    check(account.registerUser("alice", "another") == gomoku::RegisterStatus::AlreadyExists,
          "拒绝重复注册");
    check(account.registerUser("bad name", "secret") ==
              gomoku::RegisterStatus::InvalidUsername,
          "注册时校验用户名");
    check(account.registerUser("bob", "123") == gomoku::RegisterStatus::InvalidPassword,
          "注册时校验密码");

    check(account.registerUser("bob", "secret") == gomoku::RegisterStatus::Ok,
          "注册 bob 成功");
    check(account.users()[0].passwordHash != account.users()[1].passwordHash,
          "相同密码因随机盐得到不同摘要");

    account.logout();
    check(!account.loggedIn(), "退出登录清空当前用户");
    check(account.login("alice", "wrong") == gomoku::LoginStatus::InvalidCredentials,
          "拒绝错误密码");
    check(!account.loggedIn(), "登录失败不改变登录状态");
    check(account.login("nobody", "secret") == gomoku::LoginStatus::InvalidCredentials,
          "拒绝不存在的用户");
    check(account.login("alice", "secret") == gomoku::LoginStatus::Ok, "正确密码登录成功");
    check(account.currentUsername() == "alice", "登录后当前用户正确");
}

void testChineseUsername(const std::filesystem::path& root) {
    const std::filesystem::path directory = root / "chinese";
    gomoku::AccountManager account(directory);
    check(account.load(), "中文账号：创建目录");
    check(account.registerUser("棋士", "abcd") == gomoku::RegisterStatus::Ok,
          "注册中文用户名");
    account.logout();
    check(account.login("棋士", "abcd") == gomoku::LoginStatus::Ok, "中文用户名登录");
    check(readTextFile(directory / "users.txt").find("棋士") != std::string::npos,
          "users.txt 使用 UTF-8 保存中文用户名");
}

void testStatsAndRecords(const std::filesystem::path& root) {
    const std::filesystem::path directory = root / "records";
    gomoku::AccountManager account(directory);
    check(account.load(), "对局记录：创建目录");
    check(account.registerUser("dave", "pass1234") == gomoku::RegisterStatus::Ok,
          "对局记录：注册用户");

    check(account.recordGame(makeRecord(gomoku::Outcome::Win)), "保存胜局");
    check(account.recordGame(makeRecord(gomoku::Outcome::Win)), "保存第二场胜局");
    check(account.recordGame(makeRecord(gomoku::Outcome::Loss)), "保存负局");
    check(account.recordGame(makeRecord(gomoku::Outcome::Draw)), "保存平局");

    const auto stats = account.currentStats();
    check(stats.has_value(), "登录用户有战绩");
    if (stats) {
        check(stats->wins == 2, "胜场累计为 2");
        check(stats->losses == 1, "负场累计为 1");
        check(stats->draws == 1, "平局累计为 1");
        check(stats->games() == 4, "总局数为 4");
    }

    const std::string recordsText = readTextFile(directory / "records.txt");
    check(recordsText.find("dave") != std::string::npos, "records.txt 写入用户名");
    check(recordsText.find("H8") != std::string::npos, "records.txt 写入棋谱坐标");
    check(recordsText.find("win") != std::string::npos, "records.txt 写入结果");

    const auto entries = account.recentRecords(10);
    check(entries.size() == 4, "读取到四条记录");
    check(account.recentRecords(2).size() == 2, "限制返回条数");
    if (!entries.empty()) {
        check(entries.front().outcome == gomoku::Outcome::Draw, "最近一条记录在前");
        check(entries.front().moveCount == 2, "记录手数正确");
        check(entries.front().game == "五子棋", "记录棋种正确");
        check(entries.front().mode == "PvC", "记录模式正确");
    }
}

void testPersistence(const std::filesystem::path& root) {
    const std::filesystem::path directory = root / "persist";
    {
        gomoku::AccountManager first(directory);
        check(first.load(), "持久化：创建目录");
        check(first.registerUser("carol", "pw1234") == gomoku::RegisterStatus::Ok,
              "持久化：注册");
        check(first.recordGame(makeRecord(gomoku::Outcome::Win)), "持久化：保存对局");
    }

    gomoku::AccountManager second(directory);
    check(second.load(), "重新加载数据目录");
    check(second.users().size() == 1, "重新加载后账号仍在");
    check(second.login("carol", "pw1234") == gomoku::LoginStatus::Ok, "重新登录成功");
    const auto stats = second.currentStats();
    check(stats.has_value() && stats->wins == 1, "重新加载后战绩保留");
    check(second.recentRecords().size() == 1, "重新加载后对局记录保留");
}

void testMultipleUsers(const std::filesystem::path& root) {
    const std::filesystem::path directory = root / "multi";
    gomoku::AccountManager account(directory);
    check(account.load(), "多用户：创建目录");

    check(account.registerUser("eve", "evepass") == gomoku::RegisterStatus::Ok, "注册 eve");
    check(account.recordGame(makeRecord(gomoku::Outcome::Win)), "eve 保存一胜");
    account.logout();

    check(account.registerUser("frank", "frankpass") == gomoku::RegisterStatus::Ok,
          "注册 frank");
    check(account.recordGame(makeRecord(gomoku::Outcome::Loss)), "frank 保存一负");
    check(account.recentRecords().size() == 1, "frank 只看到自己的记录");
    account.logout();

    check(account.login("eve", "evepass") == gomoku::LoginStatus::Ok, "eve 重新登录");
    const auto stats = account.currentStats();
    check(stats.has_value() && stats->wins == 1 && stats->losses == 0,
          "eve 战绩与 frank 隔离");
    check(account.recentRecords().size() == 1, "eve 只看到自己的记录");
    account.logout();
    check(account.recentRecords().empty(), "未登录时没有可读记录");
    check(!account.recordGame(makeRecord(gomoku::Outcome::Win)), "游客状态不写记录");
}

}  // namespace

int main() {
    const std::filesystem::path root = makeRootDirectory();
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        std::cerr << "[失败] 无法创建临时目录：" << root.string() << '\n';
        return EXIT_FAILURE;
    }

    testPasswordDigest();
    testValidation();
    testRegisterAndLogin(root);
    testChineseUsername(root);
    testStatsAndRecords(root);
    testPersistence(root);
    testMultipleUsers(root);

    std::filesystem::remove_all(root, error);
    if (failures == 0) {
        std::cout << "账号与数据文件测试全部通过。\n";
        return EXIT_SUCCESS;
    }
    std::cerr << failures << " 项测试失败。\n";
    return EXIT_FAILURE;
}
