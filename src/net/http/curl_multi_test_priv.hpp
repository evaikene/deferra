#pragma once

#include <cstdint>

namespace jb::net::http::detail {

/// One-shot private failure points used by deterministic adapter tests.
enum class CurlMultiFailurePoint : std::uint8_t {
    None,
    Initialization,
    TimerRegistration,
    SocketAction,
};

/// Makes the next matching private adapter operation fail on the current thread.
void fail_next_curl_multi_operation_for_testing(CurlMultiFailurePoint point) noexcept;

} // namespace jb::net::http::detail
