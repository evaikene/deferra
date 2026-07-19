#include "jobu_version_priv.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

auto main(int argc, char* argv[]) -> int
{
    if (argc == 2 && std::string_view{argv[1]} == "--version") {
        std::cout << "jobuctl " << jb::jobu::detail::project_version << '\n';
        return EXIT_SUCCESS;
    }

    std::cerr << "jobuctl: not implemented\n";
    return EXIT_FAILURE;
}
