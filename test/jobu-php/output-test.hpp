#pragma once
#include <string>

namespace jobu_php_test {
/// Exercise public output operations and controlled transport/capture failures in an active PHP request.
auto test_output(std::string const& endpoint, std::string const& proxy_endpoint) -> void;
} // namespace jobu_php_test
