#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gomoku {

// 一局对局相对“当前登录用户”的结果。PvP 中把执黑（玩家1）视为登录用户。
enum class Outcome { Win, Loss, Draw };

// 累计战绩；games() 供界面直接显示总局数。
struct UserStats {
    int wins = 0;
    int losses = 0;
    int draws = 0;

    int games() const { return wins + losses + draws; }
};

// 账号记录。密码只保存“盐 + 迭代 SHA-256”的十六进制摘要，不保存明文。
struct UserAccount {
    std::string username;
    std::string passwordSalt;
    std::string passwordHash;
    UserStats stats;
    std::int64_t createdAt = 0;
};

enum class RegisterStatus { Ok, InvalidUsername, InvalidPassword, AlreadyExists, StorageError };
enum class LoginStatus { Ok, InvalidCredentials, StorageError };

// 返回中文错误说明；空字符串表示合法。控制台与图形界面共用同一套校验。
std::string validateUsername(const std::string& username);
std::string validatePassword(const std::string& password);

// 由盐和明文口令计算存储摘要（4096 次迭代 SHA-256 的十六进制结果）。
// 公开此函数便于测试校验哈希链；账号文件中只保存返回值，不保存明文。
std::string passwordDigest(const std::string& salt, const std::string& password);

// 账号数据文件管理器。
// 文件布局（UTF-8，字段以 '|' 分隔）：
//   data/users.txt ：用户名|盐|哈希|胜|负|平|创建时间
// 修改时先写临时文件再替换，避免中途退出留下半行数据。
class AccountManager {
public:
    explicit AccountManager(std::filesystem::path dataDir = std::filesystem::path("data"));

    // 创建数据目录并读取已有账号；目录不可写时返回 false。
    bool load();
    bool storageReady() const { return storageReady_; }
    const std::filesystem::path& dataDir() const { return dataDir_; }

    // 注册成功后自动登录；重名返回 AlreadyExists。
    RegisterStatus registerUser(const std::string& username, const std::string& password);
    LoginStatus login(const std::string& username, const std::string& password);
    // 用户不存在时自动注册，否则按密码登录。供图形界面使用。
    LoginStatus loginOrRegister(const std::string& username, const std::string& password);
    void logout();
    bool loggedIn() const { return !currentUsername_.empty(); }
    const std::string& currentUsername() const { return currentUsername_; }
    std::optional<UserStats> currentStats() const;
    const std::vector<UserAccount>& users() const { return users_; }

    // 累计当前用户的一局战绩；未登录时返回 false 且不写文件。
    bool recordOutcome(Outcome outcome);

private:
    const UserAccount* findUser(const std::string& username) const;
    UserAccount* findUser(const std::string& username);
    bool saveUsers();
    std::filesystem::path usersFile() const { return dataDir_ / "users.txt"; }

    std::filesystem::path dataDir_;
    std::vector<UserAccount> users_;
    std::string currentUsername_;
    bool storageReady_ = false;
};

}  // namespace gomoku
