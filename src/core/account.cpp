// 账号与数据文件实现：用户注册/登录、加盐哈希和战绩累计。
// 不依赖第三方库；密码摘要使用自行实现的 SHA-256，并通过随机盐与迭代提高
// 离线暴力破解成本。账号文件采用 UTF-8 文本，字段以 '|' 分隔。
#include "gomoku/core/account.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <random>
#include <system_error>
#include <utility>

namespace gomoku {
namespace {

// SHA-256 轮常量（FIPS 180-4）。
constexpr std::uint32_t kSha256RoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

std::uint32_t rotateRight(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

// 参数：任意字节串。
// 返回：32 字节 SHA-256 摘要。按 FIPS 180-4 先填充长度字段，再逐 64 字节块压缩。
std::array<std::uint8_t, 32> sha256(const std::string& message) {
    std::array<std::uint32_t, 64> schedule{};
    std::array<std::uint32_t, 8> state{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                       0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    std::string padded = message;
    const std::uint64_t bitLength = static_cast<std::uint64_t>(padded.size()) * 8u;
    padded.push_back(static_cast<char>(0x80));
    while (padded.size() % 64u != 56u) padded.push_back('\0');
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<char>((bitLength >> shift) & 0xFFu));
    }

    for (std::size_t chunk = 0; chunk < padded.size(); chunk += 64u) {
        for (std::size_t index = 0; index < 16u; ++index) {
            const std::size_t offset = chunk + index * 4u;
            const auto byte = [&padded, offset](std::size_t position) {
                return static_cast<std::uint32_t>(
                    static_cast<std::uint8_t>(padded[offset + position]));
            };
            schedule[index] = (byte(0) << 24u) | (byte(1) << 16u) | (byte(2) << 8u) | byte(3);
        }
        for (std::size_t index = 16; index < 64u; ++index) {
            const std::uint32_t s0 = rotateRight(schedule[index - 15], 7) ^
                                     rotateRight(schedule[index - 15], 18) ^
                                     (schedule[index - 15] >> 3u);
            const std::uint32_t s1 = rotateRight(schedule[index - 2], 17) ^
                                     rotateRight(schedule[index - 2], 19) ^
                                     (schedule[index - 2] >> 10u);
            schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
        }

        std::uint32_t a = state[0];
        std::uint32_t b = state[1];
        std::uint32_t c = state[2];
        std::uint32_t d = state[3];
        std::uint32_t e = state[4];
        std::uint32_t f = state[5];
        std::uint32_t g = state[6];
        std::uint32_t h = state[7];
        for (std::size_t index = 0; index < 64u; ++index) {
            const std::uint32_t s1 =
                rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + choose + kSha256RoundConstants[index] +
                                        schedule[index];
            const std::uint32_t s0 =
                rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < 8u; ++index) {
        digest[index * 4u] = static_cast<std::uint8_t>((state[index] >> 24u) & 0xFFu);
        digest[index * 4u + 1u] = static_cast<std::uint8_t>((state[index] >> 16u) & 0xFFu);
        digest[index * 4u + 2u] = static_cast<std::uint8_t>((state[index] >> 8u) & 0xFFu);
        digest[index * 4u + 3u] = static_cast<std::uint8_t>(state[index] & 0xFFu);
    }
    return digest;
}

std::string toHex(const std::array<std::uint8_t, 32>& digest) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2u);
    for (const std::uint8_t byte : digest) {
        text.push_back(digits[byte >> 4u]);
        text.push_back(digits[byte & 0x0Fu]);
    }
    return text;
}

std::string sha256Hex(const std::string& text) {
    return toHex(sha256(text));
}

// 参数：随机盐与用户口令。
// 返回：迭代 4096 次的 SHA-256 十六进制摘要。每次迭代把上一轮十六进制摘要
// 作为下一轮输入，使暴力破解需要重复完整哈希链。
std::string hashPassword(const std::string& salt, const std::string& password) {
    std::string material = salt;
    material.push_back(':');
    material += password;
    for (int round = 0; round < 4096; ++round) {
        material = sha256Hex(material);
    }
    return material;
}

// 定长逐字节比较，避免提前 return 泄露匹配前缀长度。
bool constantTimeEquals(const std::string& lhs, const std::string& rhs) {
    if (lhs.size() != rhs.size()) return false;
    unsigned char difference = 0;
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        difference |= static_cast<unsigned char>(lhs[index]) ^
                      static_cast<unsigned char>(rhs[index]);
    }
    return difference == 0;
}

std::string randomHex(std::size_t byteCount) {
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<int> distribution(0, 255);
    static const char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(byteCount * 2u);
    for (std::size_t index = 0; index < byteCount; ++index) {
        const unsigned value = static_cast<unsigned>(distribution(generator));
        text.push_back(digits[value >> 4u]);
        text.push_back(digits[value & 0x0Fu]);
    }
    return text;
}

std::vector<std::string> splitFields(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t position = line.find('|', start);
        if (position == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, position - start));
        start = position + 1;
    }
    return fields;
}

bool parseInt(const std::string& text, int& value) {
    if (text.empty()) return false;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parseLongLong(const std::string& text, std::int64_t& value) {
    if (text.empty()) return false;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

}  // namespace

std::string passwordDigest(const std::string& salt, const std::string& password) {
    return hashPassword(salt, password);
}

std::string validateUsername(const std::string& username) {
    if (username.empty()) return "用户名不能为空";
    if (username.size() > 24u) return "用户名最长 24 字节（约 8 个汉字）";
    for (const char raw : username) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        if (ch < 32u || ch == 127u) return "用户名不能包含控制字符";
        if (ch == ' ') return "用户名不能包含空格";
        switch (ch) {
            case '|':
            case '/':
            case '\\':
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
                return "用户名不能包含 | / \\ : * ? \" < > 等符号";
            default:
                break;
        }
    }
    return {};
}

std::string validatePassword(const std::string& password) {
    if (password.empty()) return "密码不能为空";
    if (password.size() < 4u) return "密码至少 4 个字符";
    if (password.size() > 64u) return "密码最长 64 字节";
    for (const char raw : password) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        if (ch < 32u || ch == 127u) return "密码不能包含控制字符";
    }
    return {};
}

AccountManager::AccountManager(std::filesystem::path dataDir) : dataDir_(std::move(dataDir)) {}

bool AccountManager::load() {
    std::error_code error;
    std::filesystem::create_directories(dataDir_, error);
    if (error) {
        storageReady_ = false;
        return false;
    }
    storageReady_ = true;
    users_.clear();

    std::ifstream input(usersFile(), std::ios::binary);
    if (!input) return true;  // 首次运行：文件尚不存在。
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::vector<std::string> fields = splitFields(line);
        if (fields.size() < 7u) continue;
        if (fields[0].empty() || fields[1].empty() || fields[2].empty()) continue;
        UserAccount user;
        user.username = fields[0];
        user.passwordSalt = fields[1];
        user.passwordHash = fields[2];
        if (!parseInt(fields[3], user.stats.wins) ||
            !parseInt(fields[4], user.stats.losses) ||
            !parseInt(fields[5], user.stats.draws)) {
            continue;
        }
        if (user.stats.wins < 0 || user.stats.losses < 0 || user.stats.draws < 0) continue;
        if (!parseLongLong(fields[6], user.createdAt)) user.createdAt = 0;
        if (findUser(user.username) != nullptr) continue;  // 忽略重复行。
        users_.push_back(std::move(user));
    }
    return true;
}

RegisterStatus AccountManager::registerUser(const std::string& username,
                                            const std::string& password) {
    if (!validateUsername(username).empty()) return RegisterStatus::InvalidUsername;
    if (!validatePassword(password).empty()) return RegisterStatus::InvalidPassword;
    if (findUser(username) != nullptr) return RegisterStatus::AlreadyExists;

    UserAccount user;
    user.username = username;
    user.passwordSalt = randomHex(16);
    user.passwordHash = hashPassword(user.passwordSalt, password);
    user.createdAt = static_cast<std::int64_t>(std::time(nullptr));
    users_.push_back(user);
    if (!saveUsers()) {
        users_.pop_back();
        return RegisterStatus::StorageError;
    }
    currentUsername_ = username;
    return RegisterStatus::Ok;
}

LoginStatus AccountManager::login(const std::string& username, const std::string& password) {
    if (!storageReady_) return LoginStatus::StorageError;
    const UserAccount* user = findUser(username);
    if (user == nullptr) return LoginStatus::InvalidCredentials;
    if (!constantTimeEquals(user->passwordHash, hashPassword(user->passwordSalt, password))) {
        return LoginStatus::InvalidCredentials;
    }
    currentUsername_ = username;
    return LoginStatus::Ok;
}

LoginStatus AccountManager::loginOrRegister(const std::string& username,
                                            const std::string& password) {
    if (!validateUsername(username).empty() || !validatePassword(password).empty()) {
        return LoginStatus::InvalidCredentials;
    }
    if (findUser(username) == nullptr) {
        return registerUser(username, password) == RegisterStatus::Ok
                   ? LoginStatus::Ok
                   : LoginStatus::StorageError;
    }
    return login(username, password);
}

void AccountManager::logout() {
    currentUsername_.clear();
}

std::optional<UserStats> AccountManager::currentStats() const {
    const UserAccount* user = findUser(currentUsername_);
    if (user == nullptr) return std::nullopt;
    return user->stats;
}

bool AccountManager::recordOutcome(Outcome outcome) {
    if (!loggedIn()) return false;
    UserAccount* user = findUser(currentUsername_);
    if (user == nullptr) return false;

    const auto adjust = [user](Outcome result, int delta) {
        if (result == Outcome::Win) user->stats.wins += delta;
        else if (result == Outcome::Loss) user->stats.losses += delta;
        else user->stats.draws += delta;
    };
    adjust(outcome, 1);
    if (!saveUsers()) {
        adjust(outcome, -1);  // 回滚内存统计，保持与文件一致。
        return false;
    }
    return true;
}

const UserAccount* AccountManager::findUser(const std::string& username) const {
    for (const UserAccount& user : users_) {
        if (user.username == username) return &user;
    }
    return nullptr;
}

UserAccount* AccountManager::findUser(const std::string& username) {
    for (UserAccount& user : users_) {
        if (user.username == username) return &user;
    }
    return nullptr;
}

bool AccountManager::saveUsers() {
    std::error_code error;
    std::filesystem::create_directories(dataDir_, error);
    if (error) {
        storageReady_ = false;
        return false;
    }

    std::filesystem::path temporary = usersFile();
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        for (const UserAccount& user : users_) {
            output << user.username << '|' << user.passwordSalt << '|' << user.passwordHash
                   << '|' << user.stats.wins << '|' << user.stats.losses << '|'
                   << user.stats.draws << '|' << user.createdAt << '\n';
        }
        output.flush();
        if (!output) return false;
    }

    std::filesystem::rename(temporary, usersFile(), error);
    if (error) {
        // Windows 不允许 rename 覆盖已存在文件，退化为“删除后重命名”。
        std::error_code ignored;
        std::filesystem::remove(usersFile(), ignored);
        error.clear();
        std::filesystem::rename(temporary, usersFile(), error);
        if (error) {
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    storageReady_ = true;
    return true;
}

}  // namespace gomoku
