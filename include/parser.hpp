#pragma once

#include <optional>
#include <string>
#include <vector>

struct ParsedCommand {
    std::vector<std::string> arguments;
    bool background{false};
    std::vector<std::vector<std::string>> pipeline;
    std::string original;
};

struct ParseResult {
    std::optional<ParsedCommand> command;
    std::string error;

    [[nodiscard]] bool ok() const { return error.empty(); }
};

class CommandParser {
public:
    [[nodiscard]] static ParseResult parse(const std::string& line);
};

