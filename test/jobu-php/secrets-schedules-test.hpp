#pragma once

#include <string>

namespace jobu_php_test {
/// Exercise production secret/cron APIs and fault injection within one PHP request.
auto test_secrets_schedules(std::string const& endpoint, std::string const& proxy_endpoint) -> void;
} // namespace jobu_php_test
