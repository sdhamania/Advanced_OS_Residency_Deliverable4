#include "scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <iomanip>
#include <limits>
#include <numeric>
#include <queue>
#include <stdexcept>
#include <thread>

namespace simulation {
namespace {
constexpr int MAX_PROCESSES = 1000;
constexpr int MAX_VALUE = 100000;
constexpr int MAX_WORK = 1000000;
int number(const std::string& text, int minimum, int maximum) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        })) throw std::invalid_argument("expected a decimal integer");
    unsigned long long value;
    try { value = std::stoull(text); }
    catch (...) { throw std::invalid_argument("integer out of range"); }
    if (value < static_cast<unsigned>(minimum) || value > static_cast<unsigned>(maximum))
        throw std::invalid_argument("integer out of range [" + std::to_string(minimum) +
                                    ", " + std::to_string(maximum) + "]");
    return static_cast<int>(value);
}
void validate(const std::vector<ScheduledProcess>& processes) {
    if (processes.empty()) throw std::invalid_argument("no processes; use sched add first");
    if (processes.size() > MAX_PROCESSES) throw std::invalid_argument("at most 1000 processes");
    int total = 0;
    for (std::size_t i = 0; i < processes.size(); ++i) {
        const auto& p = processes[i];
        if (p.id.empty() || p.id.size() > 32 ||
            !std::all_of(p.id.begin(), p.id.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '-';
            })) throw std::invalid_argument("ID must contain 1-32 letters, digits, '_' or '-'");
        if (p.burst < 1 || p.burst > MAX_VALUE || p.arrival < 0 || p.arrival > MAX_VALUE ||
            p.priority < 0 || p.priority > MAX_VALUE)
            throw std::invalid_argument("burst 1-100000; arrival and priority 0-100000");
        total += p.burst;
        if (total > MAX_WORK) throw std::invalid_argument("total burst exceeds 1000000");
        for (std::size_t j = 0; j < i; ++j)
            if (p.id == processes[j].id) throw std::invalid_argument("duplicate process ID: " + p.id);
    }
}
}
ScheduleResult runSchedule(const std::vector<ScheduledProcess>& processes,
                           bool rr, int quantum, int tick_ms, std::ostream* trace) {
    validate(processes);
    if (rr && (quantum < 1 || quantum > MAX_VALUE))
        throw std::invalid_argument("quantum must be 1-100000");
    if (tick_ms < 0 || tick_ms > 1000) throw std::invalid_argument("tick_ms must be 0-1000");
    long long total_burst = 0;
    int latest_arrival = 0;
    for (const auto& p : processes) {
        total_burst += p.burst;
        latest_arrival = std::max(latest_arrival, p.arrival);
    }
    // A conservative bound includes every burst and the latest arrival gap.
    // Host scheduling and output can add overhead; this bounds requested sleeps.
    if ((total_burst + latest_arrival) * tick_ms > 30000)
        throw std::invalid_argument("timer budget exceeds 30000 ms; reduce tick_ms (0 = fast mode)");
    const auto started = std::chrono::steady_clock::now();
    const std::size_t count = processes.size();
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return processes[a].arrival < processes[b].arrival;
    });
    std::vector<int> remaining(count), first(count, -1), completion(count, -1);
    for (std::size_t i = 0; i < count; ++i) remaining[i] = processes[i].burst;
    // Original arrival and insertion order implement FCFS among equal priorities,
    // including a preempted job; it does not lose its original place to peers.
    auto lower = [&](std::size_t a, std::size_t b) {
        if (processes[a].priority != processes[b].priority)
            return processes[a].priority > processes[b].priority;
        if (processes[a].arrival != processes[b].arrival)
            return processes[a].arrival > processes[b].arrival;
        return a > b;
    };
    std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(lower)> heap(lower);
    std::deque<std::size_t> queue;
    std::size_t next = 0, done = 0;
    int now = 0;
    ScheduleResult result;
    const auto event = [&](const std::string& label) {
        if (trace) *trace << "t=" << now << " " << label << '\n';
    };
    const auto put = [&](std::size_t i) { if (rr) queue.push_back(i); else heap.push(i); };
    const auto admit = [&] {
        while (next < count && processes[order[next]].arrival <= now) {
            const auto i = order[next++];
            put(i);
            event("ARRIVE " + processes[i].id + " priority=" + std::to_string(processes[i].priority));
        }
    };
    const auto delay = [&](int units) {
        if (tick_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(units) * tick_ms));
        now += units;
    };
    admit();
    while (done < count) {
        if (rr ? queue.empty() : heap.empty()) {
            const int until = processes[order[next]].arrival;
            event("IDLE until=" + std::to_string(until));
            delay(until - now);
            admit();
        }
        std::size_t current;
        if (rr) { current = queue.front(); queue.pop_front(); }
        else { current = heap.top(); heap.pop(); }
        if (first[current] == -1) first[current] = now;
        ++result.dispatches;
        event("DISPATCH " + processes[current].id + " remaining=" + std::to_string(remaining[current]));
        const int start = now;
        int budget = rr ? std::min(quantum, remaining[current]) : remaining[current];
        while (budget > 0) {
            int step = budget;
            if (next < count) step = std::min(step, processes[order[next]].arrival - now);
            delay(step);
            remaining[current] -= step;
            budget -= step;
            admit();
            if (!rr && remaining[current] > 0 && !heap.empty() &&
                processes[heap.top()].priority < processes[current].priority) {
                event("PREEMPT " + processes[current].id + " by=" + processes[heap.top()].id);
                ++result.preemptions;
                break;
            }
        }
        result.slices.push_back({processes[current].id, start, now});
        if (remaining[current] == 0) {
            completion[current] = now;
            ++done;
            event("COMPLETE " + processes[current].id);
        } else {
            if (rr) event("QUANTUM_EXPIRED " + processes[current].id);
            put(current);
        }
    }
    result.finish = now;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& p = processes[i];
        const int turnaround = completion[i] - p.arrival;
        result.processes.push_back({p.id, p.arrival, p.burst, p.priority, first[i], completion[i],
                                    turnaround - p.burst, turnaround, first[i] - p.arrival});
    }
    result.elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    return result;
}
int Scheduler::command(const std::vector<std::string>& args, std::ostream& out, std::ostream& error) {
    try {
        if (args.size() == 1 || (args.size() == 2 && args[1] == "help")) {
            out << "sched add ID BURST PRIORITY [ARRIVAL]\n"
                   "sched list | sched reset\n"
                   "sched run rr QUANTUM [TICK_MS]\n"
                   "sched run priority [TICK_MS]\n"
                   "Smaller priority numbers run first; arrival defaults to 0.\n"
                   "Times are simulated units; default tick is 10 ms/unit (0 = fast mode).\n";
            return 0;
        }
        if (args[1] == "add" && (args.size() == 5 || args.size() == 6)) {
            ScheduledProcess p{args[2], number(args[3], 1, MAX_VALUE),
                               number(args[4], 0, MAX_VALUE),
                               args.size() == 6 ? number(args[5], 0, MAX_VALUE) : 0};
            auto updated = processes_;
            updated.push_back(p);
            validate(updated);
            processes_ = std::move(updated);
            out << "Added " << p.id << " burst=" << p.burst << " priority=" << p.priority
                << " arrival=" << p.arrival << '\n';
            return 0;
        }
        if (args[1] == "list" && args.size() == 2) {
            out << "ID BURST PRIORITY ARRIVAL\n";
            for (const auto& p : processes_)
                out << p.id << ' ' << p.burst << ' ' << p.priority << ' ' << p.arrival << '\n';
            return 0;
        }
        if (args[1] == "reset" && args.size() == 2) {
            processes_.clear();
            out << "Scheduler workload reset\n";
            return 0;
        }
        bool rr;
        int quantum = 1, tick = 10;
        if (args[1] == "run" && args.size() >= 3 && args[2] == "rr" &&
            (args.size() == 4 || args.size() == 5)) {
            rr = true;
            quantum = number(args[3], 1, MAX_VALUE);
            if (args.size() == 5) tick = number(args[4], 0, 1000);
        } else if (args[1] == "run" && args.size() >= 3 && args[2] == "priority" &&
                   (args.size() == 3 || args.size() == 4)) {
            rr = false;
            if (args.size() == 4) tick = number(args[3], 0, 1000);
        } else throw std::invalid_argument("invalid syntax; use sched help");
        // Validate before printing a run banner so errors never resemble a result.
        validate(processes_);
        out << "ALGORITHM " << (rr ? "RR" : "PRIORITY") << " quantum="
            << (rr ? std::to_string(quantum) : "n/a") << " tick_ms=" << tick << '\n';
        const auto result = runSchedule(processes_, rr, quantum, tick, &out);
        out << "METRICS (simulated units)\nID ARRIVAL BURST PRIORITY FIRST COMPLETION WAIT TURNAROUND RESPONSE\n";
        double waiting = 0, turnaround = 0, response = 0;
        for (const auto& m : result.processes) {
            out << m.id << ' ' << m.arrival << ' ' << m.burst << ' ' << m.priority << ' '
                << m.first_start << ' ' << m.completion << ' ' << m.waiting << ' '
                << m.turnaround << ' ' << m.response << '\n';
            waiting += m.waiting; turnaround += m.turnaround; response += m.response;
        }
        out << std::fixed << std::setprecision(3)
            << "AVERAGE wait=" << waiting / result.processes.size()
            << " turnaround=" << turnaround / result.processes.size()
            << " response=" << response / result.processes.size() << '\n'
            << "SUMMARY finish=" << result.finish << " dispatches=" << result.dispatches
            << " preemptions=" << result.preemptions << " wall_ms=" << result.elapsed_ms << '\n';
        out.unsetf(std::ios::floatfield);
        out << std::setprecision(6);
        return 0;
    } catch (const std::exception& ex) {
        error << "os-shell: sched: " << ex.what() << '\n';
        return 1;
    }
}
}
