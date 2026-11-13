#pragma once

#include <phpcxx/zend.hpp>

#include <string>

namespace jobu_php_test {
auto register_resource_classes() noexcept -> zend_result;
/// Called with an active Embed request; owns no runtime or daemon lifecycle.
auto test_resource_state(std::string const& endpoint, std::string const& proxy_endpoint) -> void;
} // namespace jobu_php_test
