#pragma once

#include <functional>
#include <phpcxx/zend.hpp>
#include <string>

namespace jobu_php_test {
/// Register fixture-only Page factories; absent from jobu.so.
auto register_page_classes() noexcept -> zend_result;
/// Run production Page scenarios within the caller's active PHP request.
auto test_pages(std::string const&           endpoint,
                std::string const&           proxy_endpoint,
                std::function<void()> const& restart_daemon) -> void;
} // namespace jobu_php_test
