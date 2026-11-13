#pragma once

#include <functional>
#include <string>

namespace jobu_php_test {
/// Exercise production statistics APIs, complete result projections and page failures in one request.
auto test_statistics(std::string const& endpoint, std::string const& proxy_endpoint) -> void;
/// Keep both method-family pages alive across a real daemon restart.
auto test_statistics_restart(std::string const&           endpoint,
                             std::string const&           proxy_endpoint,
                             std::function<void()> const& restart) -> void;
} // namespace jobu_php_test
