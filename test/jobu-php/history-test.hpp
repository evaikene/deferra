#pragma once
#include <string>

namespace jobu_php_test {
/// Exercise public history operations and injected failures in an active PHP request.
auto test_history(std::string const& endpoint, std::string const& proxy_endpoint) -> void;
} // namespace jobu_php_test
