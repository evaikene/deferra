#include "lifecycle-fixture.hpp"

#include <phpcxx/error.hpp>
#include <phpcxx/exception.hpp>

#include <cstdio>
#include <string_view>

// Only this dedicated process deliberately bails out. The jump region contains
// no automatic C++ object requiring destruction; constructing such an object to
// demonstrate skipped RAII would itself violate C++ longjmp requirements.
auto main(int argc, char** argv) -> int
{
    if (argc != 2 || (std::string_view(argv[1]) != "bailout" && std::string_view(argv[1]) != "fatal")) {
        return 2;
    }
    bool const       fatal = std::string_view(argv[1]) == "fatal";
    php::Error const error{.kind = php::ErrorKind::Runtime, .message = "php-cxx deliberate frameless error"};
    bool             caught = false;
    {
        phpcxx_test::Runtime runtime;
        if (!phpcxx_test::exercise_request()) {
            return 3;
        }

        zend_try
        {
            if (fatal) {
                php::raise(error);
            }
            else {
                zend_bailout();
            }
        }
        zend_catch
        {
            caught = true;
        }
        zend_end_try();
        // No PHP work resumes here. Attempt the host's normal shutdown only;
        // this narrow successful cleanup does not guarantee arbitrary recovery.
    }
    if (!caught || !phpcxx_test::cleanup_balanced()) {
        return 4;
    }
    std::puts("php-cxx isolated bailout observed; shutdown completed");
    return 0;
}
