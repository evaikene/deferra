#pragma once
#include <string>

namespace jobu_php_test {
/// Exercise production queues within the caller's active Embed request and daemon.
auto test_queues(std::string const& endpoint, std::string const& proxy_endpoint) -> void;
} // namespace jobu_php_test
