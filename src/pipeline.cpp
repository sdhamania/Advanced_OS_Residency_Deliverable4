#include "pipeline.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <iostream>

int executePipeline(const std::vector<std::vector<std::string>>& stages,
                    const std::function<int(const std::vector<std::string>&)>& run) {
    std::vector<std::array<int, 2>> pipes;
    std::vector<pid_t> children;
    auto closePipes = [&]() {
        for (auto& p : pipes) {
            close(p[0]);
            close(p[1]);
        }
    };
    for (std::size_t i = 1; i < stages.size(); ++i) {
        std::array<int, 2> p{};
        if (pipe(p.data()) < 0) {
            closePipes();
            std::cerr << "pipeline: pipe failed\n";
            return 1;
        }
        pipes.push_back(p);
    }
    // Keep the coordinator alive to reap stages after terminal interrupts/hangup.
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    std::cout.flush();
    std::cerr.flush();
    for (std::size_t i = 0; i < stages.size(); ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            closePipes();
            for (auto child : children) kill(child, SIGKILL);
            for (auto child : children)
                while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
                }
            std::cerr << "pipeline: fork failed\n";
            return 1;
        }
        if (pid == 0) {
            // Inherit the coordinator's process group: terminal signals reach every stage.
            signal(SIGPIPE, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            signal(SIGHUP, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            if ((i > 0 && dup2(pipes[i - 1][0], STDIN_FILENO) < 0) ||
                (i + 1 < stages.size() && dup2(pipes[i][1], STDOUT_FILENO) < 0))
                _exit(126);
            closePipes();
            const int status = run(stages[i]);
            std::cout.flush();
            std::cerr.flush();
            _exit(status);
        }
        children.push_back(pid);
    }
    closePipes();  // Essential: otherwise EOF can never reach the consumer.
    int last = 1;
    for (std::size_t i = 0; i < children.size(); ++i) {
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(children[i], &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (i + 1 == children.size() && waited > 0)
            last = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    return last;  // Traditional last-stage status; no implicit pipefail.
}
