#include "parser.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void testEmptyInput() {
    const ParseResult result = CommandParser::parse("   \t  ");
    check(result.ok(), "blank input is valid");
    check(!result.command.has_value(), "blank input produces no command");
}

void testArgumentsAndQuotes() {
    const ParseResult result =
        CommandParser::parse("echo 'operating systems' \"job control\" escaped\\ value");
    check(result.ok() && result.command.has_value(), "quoted command parses");
    if (!result.command) return;
    check(result.command->arguments.size() == 4, "quoted command has four arguments");
    check(result.command->arguments[1] == "operating systems", "single quotes group words");
    check(result.command->arguments[2] == "job control", "double quotes group words");
    check(result.command->arguments[3] == "escaped value", "backslash escapes spaces");
}

void testBackgroundOperator() {
    const ParseResult result = CommandParser::parse("sleep 10&   ");
    check(result.ok() && result.command.has_value(), "background command parses");
    if (!result.command) return;
    check(result.command->background, "ampersand marks background execution");
    check(result.command->arguments.size() == 2, "ampersand is not an argument");
}

void testQuotedAmpersand() {
    const ParseResult result = CommandParser::parse("echo '&'");
    check(result.ok() && result.command.has_value(), "quoted ampersand parses");
    if (!result.command) return;
    check(!result.command->background, "quoted ampersand is literal");
    check(result.command->arguments[1] == "&", "quoted ampersand is preserved");
}

void testBackslashInsideDoubleQuotes() {
    const ParseResult result = CommandParser::parse(R"(echo "C:\notes" "a\q" "a\"b" "a\\b" "\$literal")");
    check(result.ok() && result.command.has_value(), "double-quoted escapes parse");
    if (!result.command || result.command->arguments.size() != 6) {
        check(false, "double-quoted escape argument count");
        return;
    }
    const auto& args = result.command->arguments;
    check(args[1] == R"(C:\notes)", "ordinary backslash survives double quotes");
    check(args[2] == R"(a\q)", "backslash before q stays literal");
    check(args[3] == "a\"b", "escaped double quote is preserved");
    check(args[4] == R"(a\b)", "escaped backslash becomes one backslash");
    check(args[5] == "$literal", "escaped dollar sign is literal");
}

void testSyntaxErrors() {
    check(!CommandParser::parse("echo 'unfinished").ok(), "unmatched quote is rejected");
    check(!CommandParser::parse("echo unfinished\\").ok(), "unfinished escape is rejected");
    check(!CommandParser::parse("sleep 1 & echo later").ok(), "non-final ampersand is rejected");
    check(CommandParser::parse("ls | cat").ok(), "pipeline parses");
    for(const auto& input : {"| cat", "ls |", "ls || cat", "ls | &"})
        check(!CommandParser::parse(input).ok(), "empty pipeline stage rejected");
    auto pipeline=CommandParser::parse("echo 'a|b'|grep b|sort &");
    check(pipeline.command && pipeline.command->pipeline.size()==3 && pipeline.command->background, "three stages and background");
    check(pipeline.command && pipeline.command->pipeline[0][1]=="a|b", "quoted pipe literal");
    check(!CommandParser::parse("&").ok(), "ampersand without a command is rejected");
    check(!CommandParser::parse(std::string("echo a\0b", 8)).ok(), "embedded NUL is rejected");
}

}  // namespace

int main() {
    testEmptyInput();
    testArgumentsAndQuotes();
    testBackgroundOperator();
    testQuotedAmpersand();
    testBackslashInsideDoubleQuotes();
    testSyntaxErrors();

    if (failures != 0) {
        std::cerr << failures << " parser test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All parser tests passed\n";
    return EXIT_SUCCESS;
}

