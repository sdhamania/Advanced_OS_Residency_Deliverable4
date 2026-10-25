#pragma once

#include "job.hpp"
#include "security.hpp"
#include "parser.hpp"
#include "memory.hpp"
#include "scheduler.hpp"

#include <termios.h>

#include <optional>
#include <string>
#include <vector>

class Shell {
public:
    explicit Shell(const std::filesystem::path& state);
    ~Shell();

    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    int run();

private:
    Security security_;
    bool interactive_{false};
    pid_t shell_process_group_{0};
    termios shell_terminal_modes_{};
    int next_job_id_{1};
    bool should_exit_{false};
    int exit_status_{0};
    int last_status_{0};
    int signal_pipe_[2]{-1, -1};
    std::vector<Job> jobs_;
    PagingMemory paging_memory_;
    simulation::Scheduler scheduler_;

    void initializeJobControl();
    void installSignalHandlers();
    bool readCommandLine(std::string& line);
    void drainSignalPipe();
    void updateJobStatuses();
    bool announceJobChanges();
    void shutdownJobs();

    [[nodiscard]] std::string prompt() const;
    [[nodiscard]] bool isBuiltin(const std::string& name) const;
    int executeBuiltin(const ParsedCommand& command);
    int launchExternal(const ParsedCommand& command);
    int waitForForeground(pid_t pid, pid_t process_group,
                          const std::string& command,
                          std::optional<int> existing_job_id);

    Job* findJob(int job_id);
    const Job* findJob(int job_id) const;
    Job* mostRecentActiveJob();
    void removeJob(int job_id);
    int addJob(pid_t pid, pid_t process_group, const std::string& command,
               JobState state, int wait_status = 0);

    static std::optional<int> parseJobId(const std::string& value);
    static void printError(const std::string& message);

    int builtinCd(const std::vector<std::string>& arguments);
    int builtinPwd(const std::vector<std::string>& arguments) const;
    int builtinExit(const std::vector<std::string>& arguments);
    int builtinEcho(const std::vector<std::string>& arguments) const;
    int builtinClear(const std::vector<std::string>& arguments) const;
    int builtinLs(const std::vector<std::string>& arguments) const;
    int builtinCat(const std::vector<std::string>& arguments) const;
    int builtinMkdir(const std::vector<std::string>& arguments) const;
    int builtinRmdir(const std::vector<std::string>& arguments) const;
    int builtinRm(const std::vector<std::string>& arguments) const;
    int builtinTouch(const std::vector<std::string>& arguments) const;
    int builtinKill(const std::vector<std::string>& arguments) const;
    int builtinJobs(const std::vector<std::string>& arguments);
    int builtinFg(const std::vector<std::string>& arguments);
    int builtinBg(const std::vector<std::string>& arguments);
};

