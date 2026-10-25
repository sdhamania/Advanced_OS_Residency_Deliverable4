#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// All simulated authorization is centralized here; host credentials never change.
class Security {
   public:
    explicit Security(const std::filesystem::path& state);
    ~Security();
    Security(const Security&) = delete;
    Security& operator=(const Security&) = delete;
    bool login();
    bool admin() const { return role_ == "admin"; }
    const std::string& user() const { return user_; }
    const std::string& role() const { return role_; }
    const std::filesystem::path& workspace() const { return root_; }
    bool authorize(const std::vector<std::string>& args, bool builtin, std::string& error) const;
    int command(const std::vector<std::string>& args);
    void created(const std::vector<std::string>& args);

   private:
    struct Entry {
        std::string owner;
        unsigned mode;
    };
    struct User {
        std::string role, salt, hash;
    };
    std::filesystem::path state_, root_;
    std::map<std::string, User> users_;
    std::map<std::string, Entry> acl_;
    std::string user_, role_;
    int lock_fd_{-1};
    std::filesystem::path resolve(const std::string& path) const;
    std::string key(const std::filesystem::path& path) const;
    bool allowed(const std::string& path, unsigned bits) const;
    bool within(const std::filesystem::path& path) const;
    bool protectedSystem(const std::filesystem::path& path) const;
    Entry entry(const std::filesystem::path& path) const;
    void save() const;
};
