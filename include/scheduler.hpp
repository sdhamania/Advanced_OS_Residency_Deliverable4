#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace simulation {
struct ScheduledProcess {
    std::string id;
    int burst;
    int priority;
    int arrival;
};
struct ProcessMetrics {
    std::string id;
    int arrival;
    int burst;
    int priority;
    int first_start;
    int completion;
    int waiting;
    int turnaround;
    int response;
};
struct CpuSlice { std::string id; int start; int end; };
struct ScheduleResult {
    std::vector<ProcessMetrics> processes;
    std::vector<CpuSlice> slices;
    int finish{0};
    int dispatches{0};
    int preemptions{0};
    double elapsed_ms{0};
};
// Scheduled arrivals are admitted at exact simulated timer boundaries.
ScheduleResult runSchedule(const std::vector<ScheduledProcess>& processes,
                           bool round_robin, int quantum, int tick_ms,
                           std::ostream* trace = nullptr);
class Scheduler {
public:
    int command(const std::vector<std::string>& arguments, std::ostream& out,
                std::ostream& error);
private:
    std::vector<ScheduledProcess> processes_;
};
}
