#include "shell.hpp"
#include "synchronization.hpp"
#include "pipeline.hpp"

#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <utime.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {

volatile sig_atomic_t signal_write_fd = -1;

void handleChildSignal(int) {
    const int saved_errno = errno;
    const char notification = 'c';
    // Nonblocking write is async-signal-safe. A full pipe already wakes poll.
    if (signal_write_fd >= 0) {
        const ssize_t ignored = write(signal_write_fd, &notification, 1);
        (void)ignored;
    }
    errno = saved_errno;
}

void setSignalDisposition(int signal_number, void (*handler)(int)) {
    struct sigaction action {};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = signal_number == SIGCHLD ? SA_RESTART : 0;
    if (sigaction(signal_number, &action, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "sigaction");
    }
}

void restoreChildSignals() {
    const std::array<int, 7> signals = {
        SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGHUP,
    };
    for (const int signal_number : signals) {
        struct sigaction action {};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        sigaction(signal_number, &action, nullptr);
    }
}

bool parseLong(const std::string& text, long& value) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    value = parsed;
    return true;
}

}  // namespace

Shell::Shell(const std::filesystem::path& state) : security_(state) {
    initializeJobControl();
    installSignalHandlers();
}

Shell::~Shell() {
    shutdownJobs();
    signal_write_fd = -1;
    if (signal_pipe_[0] >= 0) close(signal_pipe_[0]);
    if (signal_pipe_[1] >= 0) close(signal_pipe_[1]);
}

void Shell::installSignalHandlers() {
    if (pipe(signal_pipe_) < 0) {
        throw std::system_error(errno, std::generic_category(), "signal pipe");
    }
    for (const int descriptor : signal_pipe_) {
        if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(descriptor, F_SETFL, O_NONBLOCK) < 0) {
            throw std::system_error(errno, std::generic_category(), "signal pipe flags");
        }
    }
    signal_write_fd = signal_pipe_[1];
    setSignalDisposition(SIGINT, SIG_IGN);
    setSignalDisposition(SIGQUIT, SIG_IGN);
    setSignalDisposition(SIGTSTP, SIG_IGN);
    setSignalDisposition(SIGTTIN, SIG_IGN);
    setSignalDisposition(SIGTTOU, SIG_IGN);
    setSignalDisposition(SIGCHLD, handleChildSignal);
}

void Shell::initializeJobControl() {
    interactive_ = isatty(STDIN_FILENO) != 0;
    shell_process_group_ = getpid();

    if (!interactive_) {
        return;
    }

    // SIGTTIN must still stop us when a parent shell starts us in background.
    setSignalDisposition(SIGTTIN, SIG_DFL);
    while (true) {
        const pid_t foreground = tcgetpgrp(STDIN_FILENO);
        if (foreground < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "reading foreground process group");
        }
        if (foreground == getpgrp()) break;
        if (::kill(-getpgrp(), SIGTTIN) < 0 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(),
                                    "waiting for foreground terminal access");
        }
    }

    if (setpgid(shell_process_group_, shell_process_group_) < 0 &&
        getpgrp() != shell_process_group_) {
        throw std::system_error(errno, std::generic_category(),
                                "creating the shell process group");
    }
    shell_process_group_ = getpgrp();

    setSignalDisposition(SIGTTOU, SIG_IGN);

    if (tcsetpgrp(STDIN_FILENO, shell_process_group_) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "claiming the controlling terminal");
    }
    if (tcgetattr(STDIN_FILENO, &shell_terminal_modes_) < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "reading terminal attributes");
    }
}

int Shell::run() {
    if (!security_.login()) return 1;
    std::string line;

    while (!should_exit_) {
        updateJobStatuses();
        announceJobChanges();
        std::cout.flush();

        if (interactive_) {
            std::cout << prompt() << std::flush;
        }

        if (!readCommandLine(line)) {
            if (interactive_) {
                std::cout << '\n';
            }
            break;
        }

        ParseResult result = CommandParser::parse(line);
        if (!result.ok()) {
            printError(result.error);
            last_status_ = 2;
            continue;
        }
        if (!result.command.has_value()) {
            continue;
        }

        const ParsedCommand& command = *result.command;
        try {
            std::string denial;
            bool authorized = true;
            const auto stages = command.pipeline.empty()
                                    ? std::vector<std::vector<std::string>>{command.arguments}
                                    : command.pipeline;
            for (const auto& stage : stages) {
                if (!security_.authorize(stage, isBuiltin(stage.front()), denial)) {
                    authorized = false;
                    break;
                }
                if ((!command.pipeline.empty() || command.background) &&
                    (stage.front() == "sched" || stage.front() == "memory" ||
                     stage.front() == "sync" || stage.front() == "chmod" ||
                     stage.front() == "chown" || stage.front() == "write" ||
                     stage.front() == "mkdir" || stage.front() == "rm" ||
                     stage.front() == "rmdir" || stage.front() == "touch" ||
                     stage.front() == "fg" || stage.front() == "bg")) {
                    denial = stage.front() +
                             ": stateful commands require a standalone foreground command";
                    authorized = false;
                    break;
                }
            }
            if (!authorized) {
                printError(denial);
                last_status_ = 1;
                continue;
            }
            if (command.pipeline.empty() && isBuiltin(command.arguments.front()) &&
                !command.background) {
                last_status_ = executeBuiltin(command);
            } else {
                last_status_ = launchExternal(command);
            }
        } catch (const std::exception& e) {
            printError(e.what());
            last_status_ = 1;
        }
    }

    return should_exit_ ? exit_status_ : last_status_;
}

void Shell::drainSignalPipe() {
    char notifications[128];
    while (read(signal_pipe_[0], notifications, sizeof(notifications)) > 0) {
    }
}

bool Shell::readCommandLine(std::string& line) {
    line.clear();
    // Read from the descriptor rather than mixing poll with buffered getline.
    while (true) {
        pollfd descriptors[2] = {{STDIN_FILENO, POLLIN, 0}, {signal_pipe_[0], POLLIN, 0}};
        const int result = poll(descriptors, 2, -1);
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::system_error(errno, std::generic_category(), "poll input");
        }
        if (descriptors[1].revents & POLLIN) {
            drainSignalPipe();
            updateJobStatuses();
            const bool changed = std::any_of(jobs_.begin(), jobs_.end(),
                [](const Job& job) {
                    return !job.announced && job.state != JobState::Running;
                });
            if (interactive_ && changed) {
                // Do not consume terminal input while delivering child notices.
                std::cout << '\n';
                announceJobChanges();
                std::cout << prompt() << std::flush;
            } else if (!interactive_) {
                announceJobChanges();
            }
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP)) {
            char character;
            const ssize_t count = read(STDIN_FILENO, &character, 1);
            if (count < 0) {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(), "read input");
            }
            if (count == 0) return !line.empty();
            if (character == '\n') return true;
            line.push_back(character);
        }
        if (descriptors[0].revents & (POLLERR | POLLNVAL)) return false;
    }
}

std::string Shell::prompt() const {
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::current_path(error);
    return "os-shell:" + security_.user() + "@" + (error ? std::string("?") : directory.string()) + "$ ";
}

bool Shell::isBuiltin(const std::string& name) const {
    static const std::array<const char*, 24> names = {
        "cd",    "pwd",   "exit", "echo", "clear", "ls",  "cat",
        "mkdir", "rmdir", "rm",   "touch", "kill",  "jobs", "fg", "bg", "memory", "sync", "sched", "whoami", "perm", "access", "write", "chmod", "chown",
    };
    return std::any_of(names.begin(), names.end(),
                       [&name](const char* builtin) { return name == builtin; });
}

int Shell::executeBuiltin(const ParsedCommand& command) {
    const auto& arguments = command.arguments;
    const std::string& name = arguments.front();
    if(name=="whoami"||name=="perm"||name=="access"||name=="write"||name=="chmod"||name=="chown")return security_.command(arguments);
    if(name=="touch"||name=="mkdir"||name=="rm"||name=="rmdir") {
        int status=name=="touch"?builtinTouch(arguments):name=="mkdir"?builtinMkdir(arguments):name=="rm"?builtinRm(arguments):builtinRmdir(arguments);
        security_.created(arguments);return status;
    }
    if (name == "sched") return scheduler_.command(arguments, std::cout, std::cerr);
    if (name == "memory") return paging_memory_.command(arguments, std::cout, std::cerr);
    if (name == "sync") return synchronizationCommand(arguments, std::cout, std::cerr);
    if (name == "cd") return builtinCd(arguments);
    if (name == "pwd") return builtinPwd(arguments);
    if (name == "exit") return builtinExit(arguments);
    if (name == "echo") return builtinEcho(arguments);
    if (name == "clear") return builtinClear(arguments);
    if (name == "ls") return builtinLs(arguments);
    if (name == "cat") return builtinCat(arguments);
    if (name == "mkdir") return builtinMkdir(arguments);
    if (name == "rmdir") return builtinRmdir(arguments);
    if (name == "rm") return builtinRm(arguments);
    if (name == "touch") return builtinTouch(arguments);
    if (name == "kill") return builtinKill(arguments);
    if (name == "jobs") return builtinJobs(arguments);
    if (name == "fg") return builtinFg(arguments);
    if (name == "bg") return builtinBg(arguments);
    return 1;
}

int Shell::launchExternal(const ParsedCommand& command) {
    const bool builtin = isBuiltin(command.arguments.front());
    if (builtin && (command.arguments.front() == "sched" || command.arguments.front() == "memory" ||
                    command.arguments.front() == "sync")) {
        printError(command.arguments.front() + ": stateful simulation commands cannot use '&'");
        return 1;
    }
    if (builtin && (command.arguments.front() == "fg" || command.arguments.front() == "bg")) {
        printError(command.arguments.front() + ": job-control commands cannot use '&'");
        return 1;
    }
    std::vector<char*> arguments;
    arguments.reserve(command.arguments.size() + 1);
    for (const std::string& argument : command.arguments) {
        arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    arguments.push_back(nullptr);

    int launch_gate[2];
    if (pipe(launch_gate) < 0) {
        printError(std::string("launch pipe failed: ") + std::strerror(errno));
        return 1;
    }
    int exec_confirmation[2];
    if (pipe(exec_confirmation) < 0) {
        const int saved_errno = errno;
        close(launch_gate[0]);
        close(launch_gate[1]);
        printError(std::string("exec pipe failed: ") + std::strerror(saved_errno));
        return 1;
    }
    if (fcntl(exec_confirmation[1], F_SETFD, FD_CLOEXEC) < 0) {
        const int saved_errno = errno;
        for (const int fd : launch_gate) close(fd);
        for (const int fd : exec_confirmation) close(fd);
        printError(std::string("exec pipe flags: ") + std::strerror(saved_errno));
        return 1;
    }
    std::cout.flush();
    std::cerr.flush();
    const pid_t child = fork();
    if (child < 0) {
        const int saved_errno = errno;
        close(launch_gate[0]);
        close(launch_gate[1]);
        close(exec_confirmation[0]);
        close(exec_confirmation[1]);
        printError(std::string("fork failed: ") + std::strerror(saved_errno));
        return 1;
    }

    if (child == 0) {
        close(launch_gate[1]);
        close(exec_confirmation[0]);
        close(signal_pipe_[0]);
        close(signal_pipe_[1]);
        signal_write_fd = -1;
        if (setpgid(0, 0) < 0) {
            std::cerr << "os-shell: child setpgid failed: " << std::strerror(errno)
                      << '\n';
            _exit(126);
        }
        restoreChildSignals();
        // Wait until the parent has registered the job and assigned the terminal.
        char ignored;
        while (read(launch_gate[0], &ignored, 1) < 0 && errno == EINTR) {}
        close(launch_gate[0]);
        if (command.background && !interactive_) {
            // A batch job must not steal the remaining command script.
            const int null_input = open("/dev/null", O_RDONLY);
            if (null_input < 0 || dup2(null_input, STDIN_FILENO) < 0) {
                std::cerr << "os-shell: background stdin: " << std::strerror(errno)
                          << '\n';
                _exit(126);
            }
            if (null_input != STDIN_FILENO) close(null_input);
        }
        if (!command.pipeline.empty()) {
            close(exec_confirmation[1]);
            const int status=executePipeline(command.pipeline,[this](const std::vector<std::string>& stage){
                if(isBuiltin(stage.front())) {
                    ParsedCommand c;c.arguments=stage;
                    return executeBuiltin(c);
                }
                std::vector<char*> argv;
                for(const auto& a:stage)argv.push_back(const_cast<char*>(a.c_str()));
                argv.push_back(nullptr);
                if(security_.admin())execvp(argv[0],argv.data());
                else {
                    const std::string path=(stage[0]=="sleep"?"/bin/":"/usr/bin/")+stage[0];
                    execv(path.c_str(),argv.data());
                }
                std::cerr<<"os-shell: "<<stage[0]<<": "<<std::strerror(errno)<<'\n';
                return errno==ENOENT?127:126;
            });
            _exit(status);
        }
        if (builtin) {
            close(exec_confirmation[1]);
            ParsedCommand synchronous = command;
            synchronous.background = false;
            const int status = executeBuiltin(synchronous);
            std::cout.flush();
            std::cerr.flush();
            _exit(should_exit_ ? exit_status_ : status);
        }
        if(security_.admin())execvp(arguments[0], arguments.data());
        else {
            const std::string path=(command.arguments[0]=="sleep"?"/bin/":"/usr/bin/")+command.arguments[0];
            execv(path.c_str(),arguments.data());
        }
        const int saved_errno = errno;
        std::cerr << "os-shell: " << command.arguments.front() << ": "
                  << std::strerror(saved_errno) << '\n';
        _exit(saved_errno == ENOENT ? 127 : 126);
    }

    close(launch_gate[0]);
    close(exec_confirmation[1]);
    const auto release_child = [&]() {
        close(launch_gate[1]);
        char ignored;
        while (read(exec_confirmation[0], &ignored, 1) < 0 && errno == EINTR) {
        }
        close(exec_confirmation[0]);
    };
    if (setpgid(child, child) < 0 && errno != EACCES && errno != ESRCH) {
        printError(std::string("parent setpgid failed: ") + std::strerror(errno));
    }

    if (command.background) {
        const int job_id = addJob(child, child, command.original, JobState::Running);
        std::cout << '[' << job_id << "] " << child << '\n' << std::flush;
        release_child();
        return 0;
    }

    if (interactive_ && tcsetpgrp(STDIN_FILENO, child) < 0) {
        printError(std::string("could not give terminal to child: ") + std::strerror(errno));
    }
    release_child();
    return waitForForeground(child, child, command.original, std::nullopt);
}

int Shell::waitForForeground(pid_t pid, pid_t process_group, const std::string& command,
                             std::optional<int> existing_job_id) {
    int status = 0;
    pid_t result;
    while (true) {
        // Collect other children too, even while a long foreground job runs.
        result = waitpid(-1, &status, WUNTRACED | WCONTINUED);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) break;
        if (result == pid) {
            if (WIFCONTINUED(status)) continue;
            break;
        }
        for (Job& job : jobs_) {
            if (job.pid != result) continue;
            job.wait_status = status;
            job.announced = false;
            if (WIFSTOPPED(status)) job.state = JobState::Stopped;
            if (WIFCONTINUED(status)) job.state = JobState::Running;
            if (WIFEXITED(status) || WIFSIGNALED(status)) job.state = JobState::Done;
        }
    }
    drainSignalPipe();

    termios stopped_modes{};
    const bool have_modes = interactive_ && result > 0 && WIFSTOPPED(status) &&
                            tcgetattr(STDIN_FILENO, &stopped_modes) == 0;
    if (interactive_) {
        if (tcsetpgrp(STDIN_FILENO, shell_process_group_) < 0) {
            printError(std::string("could not reclaim terminal: ") +
                       std::strerror(errno));
        }
        tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_terminal_modes_);
    }

    if (result < 0) {
        printError(std::string("waitpid failed: ") + std::strerror(errno));
        return 1;
    }

    if (WIFSTOPPED(status)) {
        int job_id = 0;
        if (existing_job_id.has_value()) {
            job_id = *existing_job_id;
            if (Job* job = findJob(job_id)) {
                job->state = JobState::Stopped;
                job->wait_status = status;
                job->announced = true;
            }
        } else {
            job_id = addJob(pid, process_group, command, JobState::Stopped, status);
            if (Job* job = findJob(job_id)) {
                job->announced = true;
            }
        }
        if (Job* job = findJob(job_id); job && have_modes) {
            job->terminal_modes = stopped_modes;
            job->has_terminal_modes = true;
        }
        std::cout << '[' << job_id << "] Stopped  " << command << '\n';
        return 128 + WSTOPSIG(status);
    }

    if (existing_job_id.has_value()) {
        removeJob(*existing_job_id);
    }
    if (WIFSIGNALED(status)) {
        std::cout << "Terminated by signal " << WTERMSIG(status) << '\n';
        return 128 + WTERMSIG(status);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

void Shell::updateJobStatuses() {
    drainSignalPipe();
    int status = 0;
    while (true) {
        const pid_t changed = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
        if (changed == 0) {
            break;
        }
        if (changed < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        auto iterator = std::find_if(jobs_.begin(), jobs_.end(),
                                     [changed](const Job& job) {
                                         return job.pid == changed;
                                     });
        if (iterator == jobs_.end()) {
            continue;
        }

        iterator->wait_status = status;
        iterator->announced = false;
        if (WIFSTOPPED(status)) {
            iterator->state = JobState::Stopped;
        } else if (WIFCONTINUED(status)) {
            iterator->state = JobState::Running;
        } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
            iterator->state = JobState::Done;
        }
    }
}

bool Shell::announceJobChanges() {
    bool announced = false;
    for (Job& job : jobs_) {
        if (job.announced || job.state == JobState::Running) {
            continue;
        }
        std::cout << '[' << job.id << "] " << jobStateName(job.state) << "  "
                  << job.command << '\n';
        job.announced = true;
        announced = true;
    }
    return announced;
}

void Shell::shutdownJobs() {
    updateJobStatuses();
    for (const Job& job : jobs_) {
        if (job.state == JobState::Done) {
            continue;
        }
        ::kill(-job.process_group, SIGHUP);
        if (job.state == JobState::Stopped) {
            ::kill(-job.process_group, SIGCONT);
        }
    }
    jobs_.clear();
}

Job* Shell::findJob(int job_id) {
    auto iterator = std::find_if(jobs_.begin(), jobs_.end(),
                                 [job_id](const Job& job) {
                                     return job.id == job_id;
                                 });
    return iterator == jobs_.end() ? nullptr : &*iterator;
}

const Job* Shell::findJob(int job_id) const {
    auto iterator = std::find_if(jobs_.begin(), jobs_.end(),
                                 [job_id](const Job& job) {
                                     return job.id == job_id;
                                 });
    return iterator == jobs_.end() ? nullptr : &*iterator;
}

Job* Shell::mostRecentActiveJob() {
    for (auto iterator = jobs_.rbegin(); iterator != jobs_.rend(); ++iterator) {
        if (iterator->state != JobState::Done) {
            return &*iterator;
        }
    }
    return nullptr;
}

void Shell::removeJob(int job_id) {
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [job_id](const Job& job) {
                                   return job.id == job_id;
                               }),
                jobs_.end());
}

int Shell::addJob(pid_t pid, pid_t process_group, const std::string& command,
                  JobState state, int wait_status) {
    const int id = next_job_id_++;
    jobs_.push_back({id, pid, process_group, command, state, wait_status, false});
    return id;
}

std::optional<int> Shell::parseJobId(const std::string& value) {
    std::string normalized = value;
    if (!normalized.empty() && normalized.front() == '%') {
        normalized.erase(normalized.begin());
    }
    long parsed = 0;
    if (!parseLong(normalized, parsed) || parsed <= 0 ||
        parsed > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

void Shell::printError(const std::string& message) {
    std::cerr << "os-shell: " << message << '\n';
}

int Shell::builtinCd(const std::vector<std::string>& arguments) {
    if (arguments.size() > 2) {
        printError("cd: expected zero or one directory");
        return 1;
    }
    const std::string simulated_home=(security_.workspace()/"home"/security_.user()).string();
    const char* destination = nullptr;
    if (arguments.size() == 1) {
        destination = security_.admin()?std::getenv("HOME"):simulated_home.c_str();
        if (destination == nullptr) {
            printError("cd: HOME is not set");
            return 1;
        }
    } else {
        destination = arguments[1].c_str();
    }
    if (chdir(destination) < 0) {
        printError(std::string("cd: ") + destination + ": " +
                   std::strerror(errno));
        return 1;
    }
    return 0;
}

int Shell::builtinPwd(const std::vector<std::string>& arguments) const {
    if (arguments.size() != 1) {
        printError("pwd: this implementation accepts no arguments");
        return 1;
    }
    std::error_code error;
    const auto directory = std::filesystem::current_path(error);
    if (error) {
        printError("pwd: " + error.message());
        return 1;
    }
    std::cout << directory.string() << '\n';
    return 0;
}

int Shell::builtinExit(const std::vector<std::string>& arguments) {
    if (arguments.size() > 2) {
        printError("exit: expected zero or one numeric status");
        return 1;
    }
    int status = last_status_;
    if (arguments.size() == 2) {
        long parsed = 0;
        if (!parseLong(arguments[1], parsed) || parsed < 0 || parsed > 255) {
            printError("exit: status must be an integer from 0 to 255");
            return 1;
        }
        status = static_cast<int>(parsed);
    }
    exit_status_ = status;
    should_exit_ = true;
    return 0;
}

int Shell::builtinEcho(const std::vector<std::string>& arguments) const {
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (index > 1) std::cout << ' ';
        std::cout << arguments[index];
    }
    std::cout << '\n';
    return 0;
}

int Shell::builtinClear(const std::vector<std::string>& arguments) const {
    if (arguments.size() != 1) {
        printError("clear: this implementation accepts no arguments");
        return 1;
    }
    std::cout << "\033[2J\033[H" << std::flush;
    return 0;
}

int Shell::builtinLs(const std::vector<std::string>& arguments) const {
    if (arguments.size() > 2) {
        printError("ls: expected zero or one path");
        return 1;
    }
    const std::filesystem::path path =
        arguments.size() == 2 ? arguments[1] : ".";
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        printError("ls: " + path.string() + ": " +
                   (error ? error.message() : "No such file or directory"));
        return 1;
    }
    if (!std::filesystem::is_directory(path, error)) {
        std::cout << path.filename().string() << '\n';
        return 0;
    }

    std::vector<std::filesystem::directory_entry> entries;
    for (std::filesystem::directory_iterator iterator(path, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->path().filename().string().front() == '.') {
            continue;
        }
        entries.push_back(*iterator);
    }
    if (error) {
        printError("ls: " + path.string() + ": " + error.message());
        return 1;
    }
    std::sort(entries.begin(), entries.end(),
              [](const auto& left, const auto& right) {
                  return left.path().filename().string() <
                         right.path().filename().string();
              });
    for (const auto& entry : entries) {
        std::error_code type_error;
        const bool directory = entry.is_directory(type_error);
        std::cout << entry.path().filename().string()
                  << (directory && !type_error ? "/" : "") << '\n';
    }
    return 0;
}

int Shell::builtinCat(const std::vector<std::string>& arguments) const {
    if (arguments.size() < 2) {
        char buffer[8192];
        while(true){
            const auto count=read(STDIN_FILENO,buffer,sizeof(buffer));
            if(count<0&&errno==EINTR)continue;
            if(count<0){printError("cat: stdin read failed");return 1;}
            if(count==0)break;
            std::cout.write(buffer,count);
        }
        return 0;
    }
    int result = 0;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        std::ifstream file(arguments[index], std::ios::binary);
        if (!file) {
            printError("cat: " + arguments[index] + ": " +
                       std::strerror(errno));
            result = 1;
            continue;
        }
        std::error_code error;
        if (std::filesystem::is_directory(arguments[index], error)) {
            printError("cat: " + arguments[index] + ": Is a directory");
            result = 1;
            continue;
        }
        char buffer[8192];
        while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
            std::cout.write(buffer, file.gcount());
        }
        if (file.bad()) {
            printError("cat: " + arguments[index] + ": read failed");
            result = 1;
        }
    }
    return result;
}

int Shell::builtinMkdir(const std::vector<std::string>& arguments) const {
    if (arguments.size() < 2) {
        printError("mkdir: expected at least one directory");
        return 1;
    }
    int result = 0;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        std::error_code error;
        if (!std::filesystem::create_directory(arguments[index], error)) {
            printError("mkdir: " + arguments[index] + ": " +
                       (error ? error.message() : "File exists"));
            result = 1;
        }
    }
    return result;
}

int Shell::builtinRmdir(const std::vector<std::string>& arguments) const {
    if (arguments.size() < 2) {
        printError("rmdir: expected at least one directory");
        return 1;
    }
    int result = 0;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (::rmdir(arguments[index].c_str()) < 0) {
            printError("rmdir: " + arguments[index] + ": " + std::strerror(errno));
            result = 1;
        }
    }
    return result;
}

int Shell::builtinRm(const std::vector<std::string>& arguments) const {
    if (arguments.size() < 2) {
        printError("rm: expected at least one filename");
        return 1;
    }
    int result = 0;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::filesystem::path path(arguments[index]);
        std::error_code error;
        if (std::filesystem::is_directory(std::filesystem::symlink_status(path, error))) {
            printError("rm: " + path.string() + ": Is a directory");
            result = 1;
            continue;
        }
        if (!std::filesystem::remove(path, error)) {
            printError("rm: " + path.string() + ": " +
                       (error ? error.message() : "No such file"));
            result = 1;
        }
    }
    return result;
}

int Shell::builtinTouch(const std::vector<std::string>& arguments) const {
    if (arguments.size() < 2) {
        printError("touch: expected at least one filename");
        return 1;
    }
    int result = 0;
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::filesystem::path path(arguments[index]);
        if (utime(path.c_str(), nullptr) == 0) continue;
        if (errno != ENOENT) {
            printError("touch: " + path.string() + ": " +
                       std::strerror(errno));
            result = 1;
            continue;
        }
        // O_CREAT without O_TRUNC preserves data if another process creates it.
        const int descriptor = open(path.c_str(), O_CREAT | O_WRONLY | O_NONBLOCK, 0666);
        if (descriptor < 0) {
            printError("touch: " + path.string() + ": " + std::strerror(errno));
            result = 1;
        } else {
            close(descriptor);
        }
    }
    return result;
}

int Shell::builtinKill(const std::vector<std::string>& arguments) const {
    if (arguments.size() != 2) {
        printError("kill: expected one process ID");
        return 1;
    }
    long parsed = 0;
    if (!parseLong(arguments[1], parsed) || parsed <= 0 ||
        parsed > std::numeric_limits<pid_t>::max()) {
        printError("kill: PID must be a positive integer");
        return 1;
    }
    if (::kill(static_cast<pid_t>(parsed), SIGTERM) < 0) {
        printError("kill: " + arguments[1] + ": " + std::strerror(errno));
        return 1;
    }
    return 0;
}

int Shell::builtinJobs(const std::vector<std::string>& arguments) {
    if (arguments.size() != 1) {
        printError("jobs: this implementation accepts no arguments");
        return 1;
    }
    updateJobStatuses();
    for (const Job& job : jobs_) {
        std::cout << '[' << job.id << "] " << jobStateName(job.state) << "  "
                  << job.process_group << "  " << job.command << '\n';
    }
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                               [](const Job& job) {
                                   return job.state == JobState::Done;
                               }),
                jobs_.end());
    return 0;
}

int Shell::builtinFg(const std::vector<std::string>& arguments) {
    if (arguments.size() > 2) {
        printError("fg: expected zero or one job ID");
        return 1;
    }
    updateJobStatuses();
    Job* job = nullptr;
    if (arguments.size() == 2) {
        const auto id = parseJobId(arguments[1]);
        if (!id.has_value() || (job = findJob(*id)) == nullptr) {
            printError("fg: no such job: " + arguments[1]);
            return 1;
        }
    } else {
        job = mostRecentActiveJob();
        if (job == nullptr) {
            printError("fg: no current job");
            return 1;
        }
    }
    if (job->state == JobState::Done) {
        const int id = job->id;
        printError("fg: job has already completed");
        removeJob(id);
        return 1;
    }

    const int id = job->id;
    const pid_t pid = job->pid;
    const pid_t group = job->process_group;
    const std::string command = job->command;
    if (interactive_ && tcsetpgrp(STDIN_FILENO, group) < 0) {
        printError(std::string("fg: could not give terminal to job: ") +
                   std::strerror(errno));
        return 1;
    }
    if (interactive_ && job->has_terminal_modes) {
        tcsetattr(STDIN_FILENO, TCSADRAIN, &job->terminal_modes);
    }
    if (job->state == JobState::Stopped && ::kill(-group, SIGCONT) < 0) {
        if (interactive_) tcsetpgrp(STDIN_FILENO, shell_process_group_);
        printError(std::string("fg: SIGCONT failed: ") + std::strerror(errno));
        return 1;
    }
    job->state = JobState::Running;
    job->announced = true;
    std::cout << command << '\n' << std::flush;
    return waitForForeground(pid, group, command, id);
}

int Shell::builtinBg(const std::vector<std::string>& arguments) {
    if (arguments.size() > 2) {
        printError("bg: expected zero or one job ID");
        return 1;
    }
    updateJobStatuses();
    Job* job = nullptr;
    if (arguments.size() == 2) {
        const auto id = parseJobId(arguments[1]);
        if (!id.has_value() || (job = findJob(*id)) == nullptr) {
            printError("bg: no such job: " + arguments[1]);
            return 1;
        }
    } else {
        job = mostRecentActiveJob();
        if (job == nullptr) {
            printError("bg: no current job");
            return 1;
        }
    }
    if (job->state == JobState::Done) {
        const int id = job->id;
        printError("bg: job has already completed");
        removeJob(id);
        return 1;
    }
    if (job->state == JobState::Stopped &&
        ::kill(-job->process_group, SIGCONT) < 0) {
        printError(std::string("bg: SIGCONT failed: ") + std::strerror(errno));
        return 1;
    }
    job->state = JobState::Running;
    job->announced = true;
    std::cout << '[' << job->id << "] " << job->process_group << "  "
              << job->command << '\n';
    return 0;
}

