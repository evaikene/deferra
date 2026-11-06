#pragma once

#include <phpcxx/zend.hpp>

namespace jobu_php_test {

/// Register fixture-only bindings; never linked into the production module.
auto register_classes() noexcept -> zend_result;
/// Native count remains readable after request shutdown and owns no Zend state.
auto live_clients() noexcept -> int;

} // namespace jobu_php_test
