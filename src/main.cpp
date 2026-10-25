#include <exception>
#include <iostream>

#include "shell.hpp"

int main(int argc, char** argv) {
    try {
        std::filesystem::path state = ".osh-security";
        if (argc == 3 && std::string(argv[1]) == "--state")
            state = argv[2];
        else if (argc != 1) {
            std::cerr << "usage: os-shell [--state DIRECTORY]\n";
            return 2;
        }
        if (!std::filesystem::exists(state)) {
            std::cerr
                << "os-shell: initialize accounts first: python3 scripts/setup_security.py --state "
                << state << '\n';
            return 1;
        }
        Shell shell(state);
        return shell.run();
    } catch (const std::exception& exception) {
        std::cerr << "os-shell: fatal error: " << exception.what() << '\n';
        return 1;
    }
}
