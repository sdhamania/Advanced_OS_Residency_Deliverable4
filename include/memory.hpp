#pragma once
#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

class PagingMemory {
public:
    int command(const std::vector<std::string>& args, std::ostream& out, std::ostream& error);
private:
    struct Process { std::uint64_t pages; std::uint64_t references{0}; std::uint64_t faults{0}; };
    struct Frame { int pid{0}; std::uint64_t page{0}; std::uint64_t loaded{0}; std::uint64_t used{0}; };
    std::vector<Frame> frames_;
    std::map<int, Process> processes_;
    std::uint64_t page_bytes_{0}, clock_{0}, references_{0}, faults_{0}, replacements_{0};
    bool lru_{false};
    void initialize(std::size_t frames, std::uint64_t bytes, bool lru);
    void allocate(int pid, std::uint64_t pages, std::ostream& out);
    void access(int pid, std::uint64_t page, std::ostream& out);
    void release(int pid, std::ostream& out);
    void statistics(std::ostream& out) const;
};
