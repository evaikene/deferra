#pragma once
#include <string>

namespace jobu_php_test {
/// Exercise public definition lists within the caller's active request and daemon.
auto test_lists(std::string const& endpoint, std::string const& proxy_endpoint) -> void;
} // namespace jobu_php_test
