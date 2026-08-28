#pragma once

#include <cstddef>
#include <cstdint>

namespace jb::net::http::detail {

class CurlMultiAdapter;

/// One-shot private failure points used by deterministic adapter tests.
enum class CurlMultiFailurePoint : std::uint8_t {
    None,
    Initialization,
    TimerRegistration,
    SocketAction,
};

/// Makes the next matching private adapter operation fail on the current thread.
void fail_next_curl_multi_operation_for_testing(CurlMultiFailurePoint point) noexcept;

/// Socket interests accepted by the deterministic multi-adapter test seam.
enum class CurlMultiSocketInterest : std::uint8_t {
    Remove,
    Read,
    Write,
    ReadWrite,
};

/// Observable resource and reconciliation state exposed only to private tests.
struct CurlMultiAdapterTestState {
    std::size_t watch_count{0};
    std::size_t pending_watch_update_count{0};
    bool        timer_armed{false};
    bool        timer_update_pending{false};
    bool        reconcile_queued{false};
};

/// Drives and observes adapter reconciliation without depending on libcurl callback timing.
struct CurlMultiAdapterTestAccess {
    [[nodiscard]] static auto record_socket_update(CurlMultiAdapter& adapter, int fd, CurlMultiSocketInterest interest)
        -> bool;
    static void               record_timer_update(CurlMultiAdapter& adapter, long timeout_ms) noexcept;
    [[nodiscard]] static auto refresh_socket_watch_after_readiness(CurlMultiAdapter& adapter, int fd) -> bool;
    [[nodiscard]] static auto schedule_reconcile(CurlMultiAdapter& adapter) -> bool;
    [[nodiscard]] static auto state(CurlMultiAdapter const& adapter) noexcept -> CurlMultiAdapterTestState;
    static void               shutdown(CurlMultiAdapter& adapter) noexcept;
};

} // namespace jb::net::http::detail
