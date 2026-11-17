#pragma once

#include <string>

namespace jobu_php_test {
/// Exercise wire faults and leave faulted resources alive for request shutdown.
/// The caller owns the active Embed request and the real daemon.
auto test_reliability(std::string const& endpoint, std::string const& proxy_endpoint) -> void;
} // namespace jobu_php_test
