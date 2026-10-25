#include "synchronization.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <semaphore.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr int maxItems = 10000;
constexpr int maxCapacity = 128;
struct Shared {
    int ring[maxCapacity];
    int seen[maxItems];
    int head, tail, count, high_water, produced, consumed, producer_done, errors;
    int producer_waits, consumer_waits;
};
struct Semaphore {
    sem_t* handle{SEM_FAILED};
    explicit Semaphore(const char* suffix, unsigned value) {
        static unsigned sequence = 0;
        const std::string name = "/osh" + std::to_string(getpid()) + "_" + std::to_string(++sequence) + suffix;
        handle = sem_open(name.c_str(), O_CREAT | O_EXCL, 0600, value);
        if (handle == SEM_FAILED) throw std::runtime_error("sem_open: " + std::string(std::strerror(errno)));
        // Existing inherited handles survive unlink; no named object is left behind.
        if (sem_unlink(name.c_str()) != 0) {
            const int saved = errno; sem_close(handle); handle = SEM_FAILED;
            throw std::runtime_error("sem_unlink: " + std::string(std::strerror(saved)));
        }
    }
    ~Semaphore() { if (handle != SEM_FAILED) sem_close(handle); }
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
};
int integer(const std::string& text, int low, int high) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected a decimal integer: " + text);
    const auto value = std::stoull(text);
    if (value < static_cast<unsigned>(low) || value > static_cast<unsigned>(high))
        throw std::invalid_argument("integer outside supported range: " + text);
    return static_cast<int>(value);
}
void take(sem_t* sem) {
    while (sem_wait(sem) != 0) if (errno != EINTR) _exit(4);
}
bool resource(sem_t* sem) {
    while (sem_trywait(sem) != 0) {
        if (errno == EINTR) continue;
        if (errno != EAGAIN) _exit(4);
        take(sem); return true;
    }
    return false;
}
void give(sem_t* sem) { if (sem_post(sem) != 0) _exit(4); }
void put(Shared* s, int item, int capacity, sem_t* mutex, sem_t* empty, sem_t* full, bool quiet) {
    const bool blocked = resource(empty);
    take(mutex);
    if (s->count >= capacity) { ++s->errors; give(mutex); _exit(5); }
    if (blocked && item >= 0) ++s->producer_waits;
    s->ring[s->tail] = item;
    s->tail = (s->tail + 1) % capacity;
    ++s->count;
    if (s->count > s->high_water) s->high_water = s->count;
    if (item >= 0) {
        ++s->produced;
        if (!quiet) dprintf(STDOUT_FILENO, "PRODUCE worker_pid=%ld item=%d occupancy=%d/%d mutex=held\n", static_cast<long>(getpid()), item, s->count, capacity);
    }
    give(mutex); give(full);
}
void worker(Shared* s, int index, bool producer, int producers, int consumers, int items,
            int capacity, int delay, bool quiet, sem_t* mutex, sem_t* empty, sem_t* full) {
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    std::signal(SIGHUP, SIG_DFL);
    // These demonstration workers are owned by this blocking built-in, not its job table.
    // Ignore Ctrl-Z so a stop cannot strand semaphores; Ctrl-C aborts the complete demo.
    std::signal(SIGTSTP, SIG_IGN);
    if (producer) {
        for (int i = 0; i < items; ++i) {
            put(s, index * items + i, capacity, mutex, empty, full, quiet);
            if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
        take(mutex);
        const bool last = ++s->producer_done == producers;
        give(mutex);
        if (last) for (int i = 0; i < consumers; ++i) put(s, -1, capacity, mutex, empty, full, true);
    } else {
        for (;;) {
            const bool blocked = resource(full);
            take(mutex);
            if (s->count <= 0) { ++s->errors; give(mutex); _exit(5); }
            const int item = s->ring[s->head];
            s->head = (s->head + 1) % capacity;
            --s->count;
            if (item >= 0) {
                if (blocked) ++s->consumer_waits;
                ++s->consumed;
                if (item >= producers * items || ++s->seen[item] != 1) ++s->errors;
                if (!quiet) dprintf(STDOUT_FILENO, "CONSUME worker_pid=%ld item=%d occupancy=%d/%d mutex=held\n", static_cast<long>(getpid()), item, s->count, capacity);
            }
            give(mutex); give(empty);
            if (item == -1) break;
            if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
    _exit(0);
}
struct Children {
    std::vector<pid_t> pids;
    ~Children() {
        for (const auto pid : pids) if (pid > 0) kill(pid, SIGKILL);
        for (const auto pid : pids) if (pid > 0) { int status; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} }
    }
};
struct Mapping {
    Shared* data;
    Mapping() {
        void* p = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) throw std::runtime_error("mmap: " + std::string(std::strerror(errno)));
        data = static_cast<Shared*>(p);
        std::memset(data, 0, sizeof(Shared));
    }
    ~Mapping() { munmap(data, sizeof(Shared)); }
};
}
int synchronizationCommand(const std::vector<std::string>& a, std::ostream& out, std::ostream& error) {
    try {
        if (a.size() == 2 && a[1] == "help") {
            out << "sync pc PRODUCERS CONSUMERS ITEMS_PER_PRODUCER CAPACITY [DELAY_MS] [quiet]\n"
                << "Limits: workers 1..8 each, capacity 1..128, total items <=10000, delay 0..20 ms; quiet required above 200 items. Watchdog: 30 s. Ctrl-C aborts; Ctrl-Z ignored by workers.\n";
            return 0;
        }
        if (a.size() < 6 || a.size() > 8 || a[1] != "pc") throw std::invalid_argument("usage: sync help");
        const int producers = integer(a[2], 1, 8), consumers = integer(a[3], 1, 8);
        const int items = integer(a[4], 1, maxItems), capacity = integer(a[5], 1, maxCapacity);
        const int total = producers * items;
        const bool quiet = a.back() == "quiet";
        int delay = 0;
        if (a.size() >= 7 && a[6] != "quiet") delay = integer(a[6], 0, 20);
        if ((a.size() == 8 && !quiet) || (a.size() == 8 && a[6] == "quiet")) throw std::invalid_argument("usage: sync help");
        if (total > maxItems) throw std::invalid_argument("total items must be <=10000");
        if (total > 200 && !quiet) throw std::invalid_argument("use quiet for more than 200 items");
        if (total * delay > 5000) throw std::invalid_argument("total items times delay must be <=5000 ms");
        Mapping mapping;
        Semaphore mutex("m", 1), empty("e", static_cast<unsigned>(capacity)), full("f", 0);
        Children children;
        children.pids.reserve(static_cast<std::size_t>(producers + consumers));
        out << "SYNC producer_consumer producers=" << producers << " consumers=" << consumers
            << " items_per_producer=" << items << " total=" << total << " capacity=" << capacity
            << " delay_ms=" << delay << " synchronization=POSIX_named_semaphores shared=MAP_SHARED watchdog_s=30\n";
        out.flush(); error.flush();
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < producers + consumers; ++i) {
            const pid_t pid = fork();
            if (pid < 0) throw std::runtime_error("fork: " + std::string(std::strerror(errno)));
            if (pid == 0) worker(mapping.data, i < producers ? i : i - producers, i < producers,
                                 producers, consumers, items, capacity, delay, quiet, mutex.handle, empty.handle, full.handle);
            children.pids.push_back(pid);
            out << "WORKER role=" << (i < producers ? "producer" : "consumer") << " pid=" << pid << '\n';
            // Flush before the next fork so child exit cannot duplicate inherited buffered output.
            out.flush();
        }
        std::size_t remaining = children.pids.size();
        while (remaining) {
            for (auto& pid : children.pids) {
                if (pid <= 0) continue;
                int status = 0;
                const auto result = waitpid(pid, &status, WNOHANG);
                if (result < 0) { if (errno == EINTR) continue; throw std::runtime_error("waitpid failed"); }
                if (result == pid) {
                    pid = 0; --remaining;
                    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                        throw std::runtime_error("worker failed or was interrupted; remaining workers terminated and reaped");
                }
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(30))
                throw std::runtime_error("30-second watchdog expired; workers terminated and reaped");
            if (remaining) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        auto* s = mapping.data;
        int missing = 0;
        for (int i = 0; i < total; ++i) if (s->seen[i] != 1) ++missing;
        const bool valid = s->errors == 0 && missing == 0 && s->produced == total && s->consumed == total
                        && s->count == 0 && s->high_water <= capacity && s->producer_done == producers;
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        out << "SYNC_RESULT produced=" << s->produced << " consumed=" << s->consumed
            << " unique=" << total - missing << " expected=" << total << " errors=" << s->errors
            << " occupancy=" << s->count << " max_occupancy=" << s->high_water << " capacity=" << capacity
            << " producer_waits=" << s->producer_waits << " consumer_waits=" << s->consumer_waits
            << " elapsed_ms=" << std::fixed << std::setprecision(3) << elapsed
            << " verified=" << (valid ? "yes" : "NO") << '\n';
        out.unsetf(std::ios::floatfield);
        out.precision(6);
        return valid ? 0 : 1;
    } catch (const std::exception& e) {
        error << "os-shell: sync: " << e.what() << '\n';
        return 1;
    }
}
