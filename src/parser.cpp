#include "parser.hpp"

#include <cctype>

namespace {

enum class QuoteMode {
    None,
    Single,
    Double,
};

void appendToken(std::vector<std::string>& tokens, std::string& token,
                 bool& token_started) {
    if (!token_started) {
        return;
    }
    tokens.push_back(token);
    token.clear();
    token_started = false;
}

}  // namespace

ParseResult CommandParser::parse(const std::string& line) {
    if (line.find('\0') != std::string::npos) {
        return {std::nullopt, "NUL bytes are not valid in command input"};
    }
    ParsedCommand parsed;
    parsed.original = line;

    std::string token;
    bool token_started = false;
    bool escaped = false;
    QuoteMode quote = QuoteMode::None;

    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];

        if (escaped) {
            token.push_back(character);
            token_started = true;
            escaped = false;
            continue;
        }

        if (quote != QuoteMode::Single && character == '\\') {
            // In double quotes, backslash only escapes the POSIX-special
            // characters $, `, ", backslash, and newline; otherwise keep it.
            if (quote == QuoteMode::Double && index + 1 < line.size()) {
                const char next = line[index + 1];
                if (next != '$' && next != '`' && next != '"' &&
                    next != '\\' && next != '\n') {
                    token.push_back(character);
                    token_started = true;
                    continue;
                }
            }
            escaped = true;
            token_started = true;
            continue;
        }

        if (quote == QuoteMode::None && character == '\'') {
            quote = QuoteMode::Single;
            token_started = true;
            continue;
        }
        if (quote == QuoteMode::Single && character == '\'') {
            quote = QuoteMode::None;
            continue;
        }
        if (quote == QuoteMode::None && character == '"') {
            quote = QuoteMode::Double;
            token_started = true;
            continue;
        }
        if (quote == QuoteMode::Double && character == '"') {
            quote = QuoteMode::None;
            continue;
        }

        if (quote == QuoteMode::None && std::isspace(static_cast<unsigned char>(character))) {
            appendToken(parsed.arguments, token, token_started);
            continue;
        }

        if (quote == QuoteMode::None && character == '&') {
            appendToken(parsed.arguments, token, token_started);
            for (std::size_t rest = index + 1; rest < line.size(); ++rest) {
                if (!std::isspace(static_cast<unsigned char>(line[rest]))) {
                    return {std::nullopt,
                            "the background operator '&' must be the final token"};
                }
            }
            parsed.background = true;
            break;
        }

        if (quote == QuoteMode::None && character == '|') {
            appendToken(parsed.arguments, token, token_started);
            if (parsed.arguments.empty()) return {std::nullopt, "empty pipeline stage"};
            if (parsed.pipeline.size() >= 15) return {std::nullopt, "pipeline limit is 16 stages"};
            parsed.pipeline.push_back(parsed.arguments);
            parsed.arguments.clear();
            continue;
        }
        if (quote == QuoteMode::None &&
            (character == '<' || character == '>' ||
             character == ';')) {
            return {std::nullopt,
                    std::string("unsupported operator '") + character +
                        "' (redirection and command lists are not supported)"};
        }

        token.push_back(character);
        token_started = true;
    }

    if (escaped) {
        return {std::nullopt, "unfinished escape sequence"};
    }
    if (quote != QuoteMode::None) {
        return {std::nullopt, "unmatched quote"};
    }

    appendToken(parsed.arguments, token, token_started);
    if (!parsed.pipeline.empty() && parsed.arguments.empty()) return {std::nullopt, "empty pipeline stage"};
    if (parsed.arguments.empty()) {
        if (parsed.background) {
            return {std::nullopt, "missing command before '&'"};
        }
        return {std::nullopt, {}};
    }

    if (!parsed.pipeline.empty()) {
        parsed.pipeline.push_back(parsed.arguments);
        parsed.arguments = parsed.pipeline.front();
    }
    return {parsed, {}};
}

