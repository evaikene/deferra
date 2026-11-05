#pragma once

#include <phpcxx/zend.hpp>

// Shared by the CLI consumer module and the Embed test process.
auto phpcxx_test_functions() noexcept -> zend_function_entry const*;
