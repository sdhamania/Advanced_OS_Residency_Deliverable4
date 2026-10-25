#pragma once

#include <sys/types.h>
#include <termios.h>

#include <string>

enum class JobState {
    Running,
    Stopped,
    Done,
};

struct Job {
    int id;
    pid_t pid;
    pid_t process_group;
    std::string command;
    JobState state;
    int wait_status{0};
    bool announced{false};
    termios terminal_modes{};
    bool has_terminal_modes{false};
};

inline const char* jobStateName(JobState state) {
    switch (state) {
        case JobState::Running:
            return "Running";
        case JobState::Stopped:
            return "Stopped";
        case JobState::Done:
            return "Done";
    }
    return "Unknown";
}

