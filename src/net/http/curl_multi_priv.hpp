#pragma once

#include "error.hpp"
#include "event_loop.hpp"
#include "result.hpp"

#include <curl/curl.h>
#include <curl/multi.h>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace jb::net::http::detail {

struct CurlMultiAdapterTestAccess;

/// Owns one libcurl multi handle and adapts its socket/timer callbacks to EventLoop.
class CurlMultiAdapter final {
public:
    using CompletionHandler   = std::function<void(CURL*, CURLcode)>;
    using FailureHandler      = std::function<void(jb::core::Error)>;
    using InitialDriveHandler = std::function<void()>;

    [[nodiscard]] static auto create(jb::core::EventLoop& loop,
                                     CompletionHandler    completion,
                                     FailureHandler       failure,
                                     InitialDriveHandler  initial_drive = {})
        -> jb::core::Result<std::unique_ptr<CurlMultiAdapter>, jb::core::Error>;

    ~CurlMultiAdapter();

    CurlMultiAdapter(CurlMultiAdapter const&)                    = delete;
    CurlMultiAdapter(CurlMultiAdapter&&)                         = delete;
    auto operator=(CurlMultiAdapter const&) -> CurlMultiAdapter& = delete;
    auto operator=(CurlMultiAdapter&&) -> CurlMultiAdapter&      = delete;

    [[nodiscard]] auto add(CURL* easy) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto remove(CURL* easy) -> jb::core::Result<void, jb::core::Error>;
    [[nodiscard]] auto queue_initial_drive() -> jb::core::Result<void, jb::core::Error>;
    void               fail_backend(std::string_view reason);

    [[nodiscard]] auto is_available() const noexcept -> bool;
    [[nodiscard]] auto failure() const -> std::optional<jb::core::Error>;

private:
    friend struct CurlMultiAdapterTestAccess;

    struct CallbackState;
    struct WatchState;
    struct PendingWatchUpdate;

    CurlMultiAdapter(jb::core::EventLoop& loop,
                     CURLM*               multi,
                     CompletionHandler    completion,
                     FailureHandler       failure,
                     InitialDriveHandler  initial_drive);

    static auto
    socket_callback(CURL* easy, curl_socket_t socket, int action, void* context, void* socket_context) noexcept -> int;
    static auto timer_callback(CURLM* multi, long timeout_ms, void* context) noexcept -> int;

    auto record_socket_update(curl_socket_t socket, int action) -> int;
    auto record_timer_update(long timeout_ms) noexcept -> int;
    void handle_socket_ready(int fd, jb::core::FdEvents events);
    void handle_timeout();
    void run_initial_drive();
    void run_reconcile();
    void run_deferred_detach();

    [[nodiscard]] auto drive(curl_socket_t socket, int events) -> bool;
    [[nodiscard]] auto drain_completions() -> bool;
    [[nodiscard]] auto schedule_reconcile() -> bool;
    [[nodiscard]] auto reconcile() -> bool;
    [[nodiscard]] auto reconcile_watches() -> bool;
    [[nodiscard]] auto reconcile_timer() -> bool;
    [[nodiscard]] auto check_callback_failure() -> bool;

    void enter_failed(std::string_view reason);
    void shutdown() noexcept;
    void schedule_deferred_detach() noexcept;
    void detach_watches() noexcept;

    jb::core::EventLoop&                        _loop;
    CURLM*                                      _multi;
    CompletionHandler                           _completion;
    FailureHandler                              _failure_handler;
    InitialDriveHandler                         _initial_drive;
    std::optional<jb::core::Error>              _failure;
    std::shared_ptr<CallbackState>              _callback_state;
    std::unordered_map<int, WatchState>         _watches;
    std::unordered_map<int, PendingWatchUpdate> _pending_watch_updates;
    std::unordered_set<CURL*>                   _easy_handles;
    jb::core::TimerHandle                       _timer;
    long                                        _requested_timeout_ms{-1};
    bool                                        _timer_update_pending{false};
    bool                                        _reconcile_queued{false};
    bool                                        _curl_callback_failed{false};
    bool                                        _accept_curl_callbacks{true};
    bool                                        _inside_socket_callback{false};
    bool                                        _detach_queued{false};
};

} // namespace jb::net::http::detail
