#include "security.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonKeyDerivation.h>
#else
#include <openssl/evp.h>
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
bool input(std::string& text, bool secret) {
    termios old{};
    bool changed = false;
    if (secret && isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &old) < 0)
            throw std::runtime_error("cannot read password terminal mode");
        auto mode = old;
        mode.c_lflag &= ~ECHO;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &mode) < 0)
            throw std::runtime_error("cannot hide password");
        changed = true;
    }
    text.clear();
    bool overflow = false, got = false;
    char c;
    while (true) {
        auto n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got = true;
        if (c == '\n') break;
        if (text.size() < 128)
            text += c;
        else
            overflow = true;
    }
    if (changed) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        std::cout << '\n';
    }
    return got && !overflow && text.find('\0') == std::string::npos;
}
std::vector<unsigned char> unhex(const std::string& value) {
    if (value.size() % 2) throw std::runtime_error("malformed credential database");
    std::vector<unsigned char> result;
    for (std::size_t i = 0; i < value.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            throw std::runtime_error("malformed credential hex");
        };
        result.push_back(static_cast<unsigned char>((nib(value[i]) << 4) | nib(value[i + 1])));
    }
    return result;
}
bool verify(const std::string& password, const std::string& saltHex, const std::string& hashHex) {
    auto salt = unhex(saltHex), expected = unhex(hashHex);
    std::array<unsigned char, 32> derived{};
    if (salt.size() != 16 || expected.size() != 32)
        throw std::runtime_error("malformed credential lengths");
#ifdef __APPLE__
    if (CCKeyDerivationPBKDF(kCCPBKDF2, password.data(), password.size(), salt.data(), salt.size(),
                             kCCPRFHmacAlgSHA256, 600000, derived.data(), derived.size()) != 0)
#else
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(),
                          static_cast<int>(salt.size()), 600000, EVP_sha256(),
                          static_cast<int>(derived.size()), derived.data()) != 1)
#endif
        throw std::runtime_error("password derivation failed");
    unsigned difference = 0;
    for (std::size_t i = 0; i < derived.size(); ++i) difference |= derived[i] ^ expected[i];
    return difference == 0;
}
}  // namespace
Security::Security(const fs::path& state) {
    state_ = fs::canonical(state);
    root_ = fs::canonical(state_ / "workspace");
    for (const auto& path : {state_, state_ / "users.db", state_ / "acl.db"}) {
        struct stat info{};
        if (stat(path.c_str(), &info) < 0 || info.st_uid != geteuid() || (info.st_mode & 0077))
            throw std::runtime_error(
                "security state and databases must belong to the host user and exclude group/other "
                "access");
    }
    lock_fd_ = open((state_ / "session.lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_fd_ < 0 || fcntl(lock_fd_, F_SETFD, FD_CLOEXEC) < 0 ||
        flock(lock_fd_, LOCK_EX | LOCK_NB) < 0) {
        if (lock_fd_ >= 0) close(lock_fd_);
        lock_fd_ = -1;
        throw std::runtime_error(
            "security state already in use or cannot lock it; use one session per state");
    }
    std::ifstream users(state_ / "users.db");
    std::string line;
    while (std::getline(users, line)) {
        std::istringstream record(line);
        std::string name, extra;
        User u;
        if (!(record >> name >> u.role >> u.salt >> u.hash) || (record >> extra) ||
            name.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
                std::string::npos ||
            (u.role != "admin" && u.role != "standard") || users_.count(name))
            throw std::runtime_error("invalid user database");
        if (unhex(u.salt).size() != 16 || unhex(u.hash).size() != 32)
            throw std::runtime_error("invalid password record");
        users_[name] = u;
    }
    if (!users.eof() || users_.empty())
        throw std::runtime_error("cannot load user database; run scripts/setup_security.py");
    std::ifstream permissions(state_ / "acl.db");
    while (std::getline(permissions, line)) {
        std::istringstream record(line);
        std::string path, owner, extra;
        unsigned mode;
        if (!(record >> std::quoted(path) >> std::quoted(owner) >> std::oct >> mode) ||
            (record >> extra) || mode > 0777 || !users_.count(owner) || acl_.count(path) ||
            !within(resolve((root_ / path).string())))
            throw std::runtime_error("invalid permission database");
        acl_[path] = {owner, mode};
    }
    if (!permissions.eof() || !acl_.count("."))
        throw std::runtime_error("cannot load permission database");
}
Security::~Security() {
    if (lock_fd_ >= 0) close(lock_fd_);
}
bool Security::login() {
    user_.clear();
    role_.clear();
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string name, password;
        std::cout << "Username: " << std::flush;
        if (!input(name, false)) return false;
        std::cout << "Password: " << std::flush;
        if (!input(password, true)) return false;
        auto it = users_.find(name);
        // Unknown users still perform the same work, with an existing salt/hash.
        const auto& record = it == users_.end() ? users_.begin()->second : it->second;
        bool valid = verify(password, record.salt, record.hash);
        std::fill(password.begin(), password.end(), '\0');
        if (valid && it != users_.end()) {
            user_ = name;
            role_ = record.role;
            if (!admin()) fs::current_path(root_ / "home" / user_);
            std::cout << "Authenticated user=" << user_ << " role=" << role_ << '\n';
            return true;
        }
        std::cerr << "Authentication failed\n";
    }
    return false;
}
fs::path Security::resolve(const std::string& path) const {
    return fs::weakly_canonical(fs::absolute(path));
}
bool Security::within(const fs::path& p) const {
    auto a = root_.begin(), b = p.begin();
    for (; a != root_.end(); ++a, ++b)
        if (b == p.end() || *a != *b) return false;
    return true;
}
std::string Security::key(const fs::path& p) const {
    return p.lexically_relative(root_).generic_string();
}
Security::Entry Security::entry(const fs::path& p) const {
    auto it = acl_.find(key(p));
    return it == acl_.end() ? Entry{"admin", 0} : it->second;
}
bool Security::protectedSystem(const fs::path& p) const {
    auto k = key(p);
    return k == "system" || k.rfind("system/", 0) == 0;
}
bool Security::allowed(const std::string& path, unsigned bits) const {
    auto p = resolve(path);
    if (!fs::exists(p)) return false;
    if (admin()) return true;
    if (!within(p) || ((bits & 2) && protectedSystem(p))) return false;
    auto permitted = [&](const fs::path& f, unsigned wanted) {
        auto e = entry(f);
        auto have = (e.owner == user_ ? e.mode >> 6 : e.mode) & 7;
        return (have & wanted) == wanted;
    };
    auto parent = p.parent_path();
    while (within(parent)) {
        if (!permitted(parent, 1)) return false;
        if (parent == root_) break;
        parent = parent.parent_path();
    }
    return permitted(p, bits);
}
void Security::save() const {
    auto temp = state_ / "acl.db.tmp";
    std::ofstream out(temp);
    if (!out) throw std::runtime_error("cannot save permissions");
    for (const auto& pair : acl_)
        out << std::quoted(pair.first) << ' ' << std::quoted(pair.second.owner) << ' ' << std::oct
            << pair.second.mode << '\n';
    out.close();
    if (!out) throw std::runtime_error("permission write failed");
    ::chmod(temp.c_str(), 0600);
    fs::rename(temp, state_ / "acl.db");
}
bool Security::authorize(const std::vector<std::string>& a, bool builtin,
                         std::string& error) const {
    if (admin()) return true;
    auto deny = [&]() {
        error = "permission denied for " + user_ + ": " + a[0];
        return false;
    };
    auto check = [&](const std::string& p, unsigned bits) { return allowed(p, bits); };
    const auto& n = a[0];
    if (!builtin) {
        // Small, explicit streaming-filter interface. No option/path interpretation leaks.
        if (n == "grep") {
            if (a.size() < 2) return deny();
            if (!a[1].empty() && a[1][0] == '-') return deny();
            for (std::size_t i = 2; i < a.size(); ++i)
                if ((!a[i].empty() && a[i][0] == '-') || !check(a[i], 4)) return deny();
            return true;
        }
        if (n == "sort" || n == "wc") {
            for (std::size_t i = 1; i < a.size(); ++i)
                if ((!a[i].empty() && a[i][0] == '-') || !check(a[i], 4)) return deny();
            return true;
        }
        if (n == "sleep") {
            if (a.size() != 2 || a[1].empty() ||
                a[1].find_first_not_of("0123456789.") != std::string::npos)
                return deny();
            return true;
        }
        return deny();
    }
    if (n == "kill") return deny();  // Host PIDs are outside simulated ownership.
    if (n == "cd")
        return check(a.size() < 2 ? (root_ / "home" / user_).string() : a[1], 1) ? true : deny();
    if (n == "ls") {
        const std::string path = a.size() == 2 ? a[1] : ".";
        return check(path, fs::is_directory(resolve(path)) ? 5 : 4) ? true : deny();
    }
    if (n == "cat" || n == "write") {
        for (std::size_t i = 1; i < a.size() && (n != "write" || i == 1); ++i)
            if (!check(a[i], n == "cat" ? 4 : 2)) return deny();
    }
    if (n == "touch" || n == "mkdir" || n == "rm" || n == "rmdir") {
        for (std::size_t i = 1; i < a.size(); ++i) {
            auto p = resolve(a[i]);
            if (!within(p) || protectedSystem(p) ||
                fs::is_symlink(fs::symlink_status(fs::absolute(a[i]))))
                return deny();
            // Removal uses directory authority, as in Unix; touching existing files uses file
            // write.
            if (n == "touch" && fs::exists(p)) {
                if (!check(p.string(), 2)) return deny();
            } else if (!check(p.parent_path().string(), 3))
                return deny();
        }
    }
    return true;
}
void Security::created(const std::vector<std::string>& a) {
    if (a[0] != "touch" && a[0] != "mkdir" && a[0] != "rm" && a[0] != "rmdir") return;
    bool changed = false;
    for (std::size_t i = 1; i < a.size(); ++i) {
        auto p = resolve(a[i]);
        if (!within(p)) continue;
        auto k = key(p);
        if (!fs::exists(p))
            changed = acl_.erase(k) || changed;
        else if (!acl_.count(k)) {
            acl_[k] = {user_, static_cast<unsigned>(fs::is_directory(p) ? 0700 : 0600)};
            changed = true;
        }
    }
    if (changed) save();
}
int Security::command(const std::vector<std::string>& a) {
    const auto& n = a[0];
    if (n == "whoami" && a.size() == 1) {
        std::cout << "user=" << user_ << " role=" << role_ << '\n';
        return 0;
    }
    if (n == "perm" && a.size() == 2) {
        auto p = resolve(a[1]);
        if (!admin() && !allowed(p.string(), 4)) {
            std::cerr << "permission denied\n";
            return 1;
        }
        auto e = entry(p);
        std::cout << "owner=" << e.owner << " mode=" << std::oct << std::setw(3)
                  << std::setfill('0') << e.mode << std::dec << std::setfill(' ') << " path=" << p
                  << '\n';
        return 0;
    }
    if (n == "access" && a.size() == 3 && (a[1] == "r" || a[1] == "w" || a[1] == "x")) {
        bool yes = allowed(a[2], a[1] == "r" ? 4 : a[1] == "w" ? 2 : 1);
        std::cout << (yes ? "ALLOW" : "DENY") << " user=" << user_ << " access=" << a[1]
                  << " path=" << a[2] << '\n';
        return yes ? 0 : 1;
    }
    if (n == "write" && a.size() >= 3) {
        if (!fs::is_regular_file(resolve(a[1])) || !allowed(a[1], 2)) {
            std::cerr << "permission denied\n";
            return 1;
        }
        std::ofstream out(a[1], std::ios::app);
        for (std::size_t i = 2; i < a.size(); ++i) out << (i == 2 ? "" : " ") << a[i];
        out << '\n';
        if (!out) {
            std::cerr << "write failed\n";
            return 1;
        }
        return 0;
    }
    if ((n == "chmod" || n == "chown") && a.size() == 3) {
        auto p = resolve(a[2]);
        auto e = entry(p);
        if (!within(p) || !fs::exists(p) ||
            (!admin() &&
             (e.owner != user_ || protectedSystem(p) || !allowed(p.parent_path().string(), 1)))) {
            std::cerr << "permission denied\n";
            return 1;
        }
        if (n == "chown") {
            if (!admin() || !users_.count(a[1])) {
                std::cerr << "chown: admin and known owner required\n";
                return 1;
            }
            e.owner = a[1];
        } else {
            if (a[1].size() != 3 || a[1].find_first_not_of("01234567") != std::string::npos) {
                std::cerr << "chmod: expected three octal digits\n";
                return 1;
            }
            e.mode = std::stoul(a[1], nullptr, 8);
        }
        acl_[key(p)] = e;
        save();
        std::cout << n << " updated " << a[2] << '\n';
        return 0;
    }
    std::cerr << "usage: whoami | perm PATH | access r|w|x PATH | write PATH TEXT | chmod MODE "
                 "PATH | chown USER PATH\n";
    return 2;
}
