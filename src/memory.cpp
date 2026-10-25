#include "memory.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
std::uint64_t number(const std::string& s, std::uint64_t minimum, std::uint64_t maximum) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected a decimal integer: " + s);
    std::size_t count = 0;
    const auto value = std::stoull(s, &count);
    if (count != s.size() || value < minimum || value > maximum)
        throw std::invalid_argument("integer outside supported range: " + s);
    return value;
}
}

void PagingMemory::initialize(std::size_t frames, std::uint64_t bytes, bool lru) {
    frames_.assign(frames, Frame{});
    processes_.clear();
    page_bytes_ = bytes;
    clock_ = references_ = faults_ = replacements_ = 0;
    lru_ = lru;
}
void PagingMemory::allocate(int pid, std::uint64_t pages, std::ostream& out) {
    if (frames_.empty()) throw std::invalid_argument("initialize memory first");
    if (processes_.count(pid)) throw std::invalid_argument("process is already allocated");
    if (processes_.size() >= 4096) throw std::invalid_argument("process limit is 4096");
    processes_.emplace(pid, Process{pages});
    out << "ALLOC pid=" << pid << " pages=" << pages << " logical_bytes=" << pages * page_bytes_
        << " resident_frames=0 (demand paging)\n";
}
void PagingMemory::access(int pid, std::uint64_t page, std::ostream& out) {
    auto proc = processes_.find(pid);
    if (proc == processes_.end()) throw std::invalid_argument("unknown process");
    if (page >= proc->second.pages) throw std::invalid_argument("page is outside process allocation");
    ++clock_; ++references_; ++proc->second.references;
    auto frame = std::find_if(frames_.begin(), frames_.end(), [=](const Frame& f) { return f.pid == pid && f.page == page; });
    if (frame != frames_.end()) {
        frame->used = clock_;
        out << "HIT pid=" << pid << " page=" << page << " frame=" << (frame - frames_.begin()) << '\n';
        return;
    }
    ++faults_; ++proc->second.faults;
    frame = std::find_if(frames_.begin(), frames_.end(), [](const Frame& f) { return f.pid == 0; });
    out << "FAULT pid=" << pid << " page=" << page;
    if (frame == frames_.end()) {
        frame = std::min_element(frames_.begin(), frames_.end(), [this](const Frame& a, const Frame& b) {
            return (lru_ ? a.used : a.loaded) < (lru_ ? b.used : b.loaded);
        });
        ++replacements_;
        out << " EVICT pid=" << frame->pid << " page=" << frame->page;
    }
    out << " frame=" << (frame - frames_.begin()) << '\n';
    *frame = Frame{pid, page, clock_, clock_};
}
void PagingMemory::release(int pid, std::ostream& out) {
    const auto proc = processes_.find(pid);
    if (proc == processes_.end()) throw std::invalid_argument("unknown process");
    std::size_t released = 0;
    for (auto& frame : frames_) if (frame.pid == pid) { frame = Frame{}; ++released; }
    out << "FREE pid=" << pid << " logical_bytes=" << proc->second.pages * page_bytes_
        << " released_frames=" << released << '\n';
    processes_.erase(proc);
}
void PagingMemory::statistics(std::ostream& out) const {
    if (frames_.empty()) throw std::invalid_argument("initialize memory first");
    const auto used = std::count_if(frames_.begin(), frames_.end(), [](const Frame& f) { return f.pid != 0; });
    out << "MEMORY policy=" << (lru_ ? "LRU" : "FIFO") << " page_bytes=" << page_bytes_
        << " frames=" << frames_.size() << " used_frames=" << used
        << " used_bytes=" << static_cast<std::uint64_t>(used) * page_bytes_
        << " capacity_bytes=" << frames_.size() * page_bytes_ << " references=" << references_
        << " faults=" << faults_ << " replacements=" << replacements_ << '\n';
    for (const auto& entry : processes_) {
        const int pid = entry.first;
        const auto resident = std::count_if(frames_.begin(), frames_.end(), [pid](const Frame& f) { return f.pid == pid; });
        out << "PROCESS pid=" << pid << " pages=" << entry.second.pages
            << " logical_bytes=" << entry.second.pages * page_bytes_ << " resident_frames=" << resident
            << " resident_bytes=" << static_cast<std::uint64_t>(resident) * page_bytes_
            << " references=" << entry.second.references << " faults=" << entry.second.faults << '\n';
    }
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        out << "FRAME index=" << i;
        if (!frames_[i].pid) out << " FREE\n";
        else out << " pid=" << frames_[i].pid << " page=" << frames_[i].page << '\n';
    }
}
int PagingMemory::command(const std::vector<std::string>& a, std::ostream& out, std::ostream& error) {
    try {
        if (a.size() == 2 && a[1] == "help") {
            out << "memory init FRAMES PAGE_BYTES fifo|lru\nmemory alloc PID PAGES\nmemory access PID PAGE (zero-based)\nmemory free PID\nmemory stats\nmemory demo fifo|lru\n";
        } else if (a.size() == 5 && a[1] == "init") {
            const auto frames = number(a[2], 1, 1024), bytes = number(a[3], 1, 1048576);
            if (a[4] != "fifo" && a[4] != "lru") throw std::invalid_argument("policy must be fifo or lru");
            initialize(static_cast<std::size_t>(frames), bytes, a[4] == "lru");
            out << "INIT policy=" << (lru_ ? "LRU" : "FIFO") << " frames=" << frames << " page_bytes=" << bytes << " (prior allocations reset)\n";
        } else if (a.size() == 4 && a[1] == "alloc") {
            allocate(static_cast<int>(number(a[2], 1, 2147483647)), number(a[3], 1, 1000000), out);
        } else if (a.size() == 4 && a[1] == "access") {
            access(static_cast<int>(number(a[2], 1, 2147483647)), number(a[3], 0, 999999), out);
        } else if (a.size() == 3 && a[1] == "free") {
            release(static_cast<int>(number(a[2], 1, 2147483647)), out);
        } else if (a.size() == 2 && a[1] == "stats") {
            statistics(out);
        } else if (a.size() == 3 && a[1] == "demo" && (a[2] == "fifo" || a[2] == "lru")) {
            initialize(3, 4096, a[2] == "lru");
            out << "DEMO policy=" << (lru_ ? "LRU" : "FIFO") << " reference=0,1,2,0,1,3,0,3,1,2,1 frames=3 page_bytes=4096\n";
            allocate(1, 4, out);
            for (auto page : {0,1,2,0,1,3,0,3,1,2,1}) access(1, static_cast<std::uint64_t>(page), out);
            statistics(out);
        } else throw std::invalid_argument("usage: memory help");
        return 0;
    } catch (const std::exception& e) {
        error << "os-shell: memory: " << e.what() << '\n';
        return 1;
    }
}
