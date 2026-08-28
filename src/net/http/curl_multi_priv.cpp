#include "curl_multi_priv.hpp"

#include "curl_multi_test_priv.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace jb::net::http::detail {

namespace {

using AdapterResult = jb::core::Result<std::unique_ptr<CurlMultiAdapter>, jb::core::Error>;
using VoidResult    = jb::core::Result<void, jb::core::Error>;

thread_local CurlMultiFailurePoint g_next_failure{CurlMultiFailurePoint::None};

auto consume_failure(CurlMultiFailurePoint point) noexcept -> bool
{
    if (g_next_failure != point) {
        return false;
    }
    g_next_failure = CurlMultiFailurePoint::None;
    return true;
}

auto backend_failed(std::string_view reason) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::Internal,
        .code     = "net.http.backend_failed",
        .message  = "The system HTTP transfer backend failed",
        .detail   = std::string{reason},
    };
}

auto curl_events(int action) noexcept -> jb::core::FdEvents
{
    auto events = jb::core::FdEvents{};
    if (action == CURL_POLL_IN || action == CURL_POLL_INOUT) {
        events.set(jb::core::FdEvent::Read);
    }
    if (action == CURL_POLL_OUT || action == CURL_POLL_INOUT) {
        events.set(jb::core::FdEvent::Write);
    }
    return events;
}

auto curl_ready_events(jb::core::FdEvents events) noexcept -> int
{
    auto ready = 0;
    if (events.test(jb::core::FdEvent::Read)) {
        ready |= CURL_CSELECT_IN;
    }
    if (events.test(jb::core::FdEvent::Write)) {
        ready |= CURL_CSELECT_OUT;
    }
    return ready;
}

} // anonymous namespace

struct CurlMultiAdapter::CallbackState {
    CurlMultiAdapter* owner;
};

struct CurlMultiAdapter::WatchState {
    jb::core::FdWatch              handle;
    jb::core::FdEvents             events;
    std::shared_ptr<CallbackState> callback;
};

struct CurlMultiAdapter::PendingWatchUpdate {
    jb::core::FdEvents events;
    bool               remove{false};
};

void fail_next_curl_multi_operation_for_testing(CurlMultiFailurePoint point) noexcept
{
    g_next_failure = point;
}

auto CurlMultiAdapterTestAccess::record_socket_update(CurlMultiAdapter&       adapter,
                                                      int                     fd,
                                                      CurlMultiSocketInterest interest) -> bool
{
    auto action = CURL_POLL_REMOVE;
    switch (interest) {
        case CurlMultiSocketInterest::Remove:
            action = CURL_POLL_REMOVE;
            break;
        case CurlMultiSocketInterest::Read:
            action = CURL_POLL_IN;
            break;
        case CurlMultiSocketInterest::Write:
            action = CURL_POLL_OUT;
            break;
        case CurlMultiSocketInterest::ReadWrite:
            action = CURL_POLL_INOUT;
            break;
    }
    return adapter.record_socket_update(static_cast<curl_socket_t>(fd), action) == 0;
}

void CurlMultiAdapterTestAccess::record_timer_update(CurlMultiAdapter& adapter, long timeout_ms) noexcept
{
    static_cast<void>(adapter.record_timer_update(timeout_ms));
}

auto CurlMultiAdapterTestAccess::refresh_socket_watch_after_readiness(CurlMultiAdapter& adapter, int fd) -> bool
{
    return adapter.refresh_socket_watch_after_readiness(fd);
}

auto CurlMultiAdapterTestAccess::schedule_reconcile(CurlMultiAdapter& adapter) -> bool
{
    return adapter.schedule_reconcile();
}

auto CurlMultiAdapterTestAccess::state(CurlMultiAdapter const& adapter) noexcept -> CurlMultiAdapterTestState
{
    return {
        .watch_count                = adapter._watches.size(),
        .pending_watch_update_count = adapter._pending_watch_updates.size(),
        .timer_armed                = static_cast<bool>(adapter._timer),
        .timer_update_pending       = adapter._timer_update_pending,
        .reconcile_queued           = adapter._reconcile_queued,
    };
}

void CurlMultiAdapterTestAccess::shutdown(CurlMultiAdapter& adapter) noexcept
{
    adapter.shutdown();
}

auto CurlMultiAdapter::create(jb::core::EventLoop& loop, CompletionHandler completion, FailureHandler failure)
    -> AdapterResult
{
    if (consume_failure(CurlMultiFailurePoint::Initialization)) {
        return AdapterResult::failure(backend_failed("multi.initialization_failed"));
    }

    auto* multi = curl_multi_init();
    if (!multi) {
        return AdapterResult::failure(backend_failed("multi.initialization_failed"));
    }

    auto adapter = std::unique_ptr<CurlMultiAdapter>{
        new CurlMultiAdapter{loop, multi, std::move(completion), std::move(failure)}
    };
    if (curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, &CurlMultiAdapter::socket_callback) != CURLM_OK ||
        curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, adapter.get()) != CURLM_OK ||
        curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, &CurlMultiAdapter::timer_callback) != CURLM_OK ||
        curl_multi_setopt(multi, CURLMOPT_TIMERDATA, adapter.get()) != CURLM_OK) {
        adapter->shutdown();
        return AdapterResult::failure(backend_failed("multi.callback_configuration_failed"));
    }

    return AdapterResult::success(std::move(adapter));
}

CurlMultiAdapter::CurlMultiAdapter(jb::core::EventLoop& loop,
                                   CURLM*               multi,
                                   CompletionHandler    completion,
                                   FailureHandler       failure)
    : _loop{loop}
    , _multi{multi}
    , _completion{std::move(completion)}
    , _failure_handler{std::move(failure)}
    , _callback_state{std::make_shared<CallbackState>(CallbackState{.owner = this})}
{}

CurlMultiAdapter::~CurlMultiAdapter()
{
    shutdown();
}

auto CurlMultiAdapter::add(CURL* easy) -> VoidResult
{
    if (!is_available()) {
        return VoidResult::failure(_failure.value_or(backend_failed("multi.unavailable")));
    }

    auto const result = curl_multi_add_handle(_multi, easy);
    if (result != CURLM_OK) {
        enter_failed("multi.add_handle_failed");
        return VoidResult::failure(*_failure);
    }
    _easy_handles.insert(easy);
    if (!check_callback_failure()) {
        enter_failed("multi.add_handle_failed");
        return VoidResult::failure(*_failure);
    }
    return VoidResult::success();
}

auto CurlMultiAdapter::remove(CURL* easy) -> VoidResult
{
    if (!is_available() || !_easy_handles.contains(easy)) {
        return VoidResult::failure(_failure.value_or(backend_failed("multi.unavailable")));
    }

    auto const result = curl_multi_remove_handle(_multi, easy);
    if (result != CURLM_OK || !check_callback_failure()) {
        enter_failed("multi.remove_handle_failed");
        return VoidResult::failure(*_failure);
    }
    _easy_handles.erase(easy);
    if (!schedule_reconcile()) {
        return VoidResult::failure(*_failure);
    }
    return VoidResult::success();
}

auto CurlMultiAdapter::queue_initial_drive() -> VoidResult
{
    if (!is_available()) {
        return VoidResult::failure(_failure.value_or(backend_failed("multi.unavailable")));
    }

    auto callback = _callback_state;
    // The initial multi drive is queued so an accepted request can never complete from inside start().
    if (!_loop.post([callback = std::move(callback)]() -> void {
            if (callback->owner) {
                callback->owner->run_initial_drive();
            }
        })) {
        enter_failed("event_loop.initial_drive_failed");
        return VoidResult::failure(*_failure);
    }
    return VoidResult::success();
}

void CurlMultiAdapter::fail_backend(std::string_view reason)
{
    enter_failed(reason);
}

auto CurlMultiAdapter::is_available() const noexcept -> bool
{
    return _multi != nullptr && !_failure.has_value();
}

auto CurlMultiAdapter::failure() const -> std::optional<jb::core::Error>
{
    return _failure;
}

auto CurlMultiAdapter::socket_callback(CURL* /*easy*/,
                                       curl_socket_t socket,
                                       int           action,
                                       void*         context,
                                       void* /*socket_context*/) noexcept -> int
{
    auto* adapter = static_cast<CurlMultiAdapter*>(context);
    if (!adapter || !adapter->_accept_curl_callbacks) {
        return 0;
    }

    try {
        return adapter->record_socket_update(socket, action);
    }
    catch (...) {
        // No exception may unwind through libcurl's C callback boundary.
        adapter->_curl_callback_failed = true;
        return -1;
    }
}

auto CurlMultiAdapter::timer_callback(CURLM* /*multi*/, long timeout_ms, void* context) noexcept -> int
{
    auto* adapter = static_cast<CurlMultiAdapter*>(context);
    if (!adapter || !adapter->_accept_curl_callbacks) {
        return 0;
    }
    return adapter->record_timer_update(timeout_ms);
}

auto CurlMultiAdapter::record_socket_update(curl_socket_t socket, int action) -> int
{
    auto const fd = static_cast<int>(socket);
    if (static_cast<curl_socket_t>(fd) != socket || fd < 0) {
        _curl_callback_failed = true;
        return -1;
    }

    if (action == CURL_POLL_REMOVE || action == CURL_POLL_NONE) {
        _pending_watch_updates[fd] = {.events = {}, .remove = true};
        return 0;
    }

    auto const events = curl_events(action);
    if (events.none()) {
        _curl_callback_failed = true;
        return -1;
    }
    _pending_watch_updates[fd] = {.events = events, .remove = false};
    return 0;
}

auto CurlMultiAdapter::record_timer_update(long timeout_ms) noexcept -> int
{
    // Cancel the superseded timer immediately so EventLoop's timer-before-task ordering cannot fire stale curl work
    // while the replacement remains queued for reconciliation.
    if (_timer) {
        _loop.cancel_timer(_timer);
        _timer = {};
    }
    _requested_timeout_ms = timeout_ms;
    _timer_update_pending = true;
    return 0;
}

void CurlMultiAdapter::handle_socket_ready(int fd, jb::core::FdEvents events)
{
    if (!is_available()) {
        return;
    }
    _inside_socket_callback = true;
    auto const driven       = drive(static_cast<curl_socket_t>(fd), curl_ready_events(events));
    _inside_socket_callback = false;
    if (driven) {
        static_cast<void>(refresh_socket_watch_after_readiness(fd));
    }
}

void CurlMultiAdapter::handle_timeout()
{
    _timer = {};
    if (!is_available()) {
        return;
    }
    static_cast<void>(drive(CURL_SOCKET_TIMEOUT, 0));
}

void CurlMultiAdapter::run_initial_drive()
{
    if (!is_available() || !reconcile()) {
        return;
    }
    static_cast<void>(drive(CURL_SOCKET_TIMEOUT, 0));
}

void CurlMultiAdapter::run_reconcile()
{
    _reconcile_queued = false;
    if (is_available()) {
        static_cast<void>(reconcile());
    }
}

void CurlMultiAdapter::run_deferred_detach()
{
    _detach_queued = false;
    detach_watches();
}

auto CurlMultiAdapter::drive(curl_socket_t socket, int events) -> bool
{
    if (!is_available()) {
        return false;
    }
    if (consume_failure(CurlMultiFailurePoint::SocketAction)) {
        enter_failed("multi.socket_action_failed");
        return false;
    }

    int        running_handles{0};
    auto const result = curl_multi_socket_action(_multi, socket, events, &running_handles);
    if (result != CURLM_OK || !check_callback_failure()) {
        enter_failed("multi.socket_action_failed");
        return false;
    }
    if (!drain_completions()) {
        return false;
    }
    return schedule_reconcile();
}

auto CurlMultiAdapter::drain_completions() -> bool
{
    int messages_remaining{0};
    while (auto* message = curl_multi_info_read(_multi, &messages_remaining)) {
        if (message->msg != CURLMSG_DONE) {
            continue;
        }

        auto*      easy   = message->easy_handle;
        auto const result = curl_multi_remove_handle(_multi, easy);
        if (result != CURLM_OK || !check_callback_failure()) {
            enter_failed("multi.completion_remove_failed");
            return false;
        }
        _easy_handles.erase(easy);
        _completion(easy, message->data.result);
        if (!is_available()) {
            return false;
        }
    }
    return true;
}

auto CurlMultiAdapter::refresh_socket_watch_after_readiness(int fd) -> bool
{
    if (!is_available()) {
        return false;
    }
    if (_pending_watch_updates.contains(fd)) {
        return schedule_reconcile();
    }

    auto const current = _watches.find(fd);
    if (current == _watches.end()) {
        return true;
    }

    // libcurl's socket API expects level-triggered readiness. Refresh an unchanged EventLoop watch after each drive so
    // an edge-triggered backend can report a socket that remains writable during a sustained transfer.
    _pending_watch_updates[fd] = {.events = current->second.events, .remove = false};
    return schedule_reconcile();
}

auto CurlMultiAdapter::schedule_reconcile() -> bool
{
    if (!is_available() || (_pending_watch_updates.empty() && !_timer_update_pending) || _reconcile_queued) {
        return is_available();
    }

    auto callback     = _callback_state;
    _reconcile_queued = true;
    // Reconciliation runs after the originating watcher callback returns, so replacing that EventLoop callback cannot
    // destroy the callable currently on the dispatch stack.
    if (!_loop.post([callback = std::move(callback)]() -> void {
            if (callback->owner) {
                callback->owner->run_reconcile();
            }
        })) {
        _reconcile_queued = false;
        enter_failed("event_loop.reconcile_failed");
        return false;
    }
    return true;
}

auto CurlMultiAdapter::reconcile() -> bool
{
    return reconcile_watches() && reconcile_timer();
}

auto CurlMultiAdapter::reconcile_watches() -> bool
{
    auto updates = std::move(_pending_watch_updates);
    _pending_watch_updates.clear();

    for (auto const& [fd, update] : updates) {
        auto current = _watches.find(fd);
        if (update.remove) {
            if (current != _watches.end()) {
                if (!_loop.unwatch_fd(current->second.handle)) {
                    enter_failed("event_loop.watch_removal_failed");
                    return false;
                }
                current->second.callback->owner = nullptr;
                _watches.erase(current);
            }
            continue;
        }

        auto callback = std::make_shared<CallbackState>(CallbackState{.owner = this});
        auto handle   = _loop.watch_fd(fd, update.events, [callback](int ready_fd, jb::core::FdEvents events) -> void {
            auto* owner = callback->owner;
            if (owner) {
                owner->handle_socket_ready(ready_fd, events);
            }
        });
        if (!handle) {
            enter_failed("event_loop.watch_registration_failed");
            return false;
        }

        if (current != _watches.end()) {
            current->second.callback->owner = nullptr;
        }
        _watches[fd] = {.handle = handle, .events = update.events, .callback = std::move(callback)};
    }
    return true;
}

auto CurlMultiAdapter::reconcile_timer() -> bool
{
    if (!_timer_update_pending) {
        return true;
    }

    _timer_update_pending = false;
    if (_timer) {
        _loop.cancel_timer(_timer);
        _timer = {};
    }
    if (_requested_timeout_ms < 0L) {
        return true;
    }
    if (consume_failure(CurlMultiFailurePoint::TimerRegistration)) {
        enter_failed("event_loop.timer_registration_failed");
        return false;
    }

    auto callback = _callback_state;
    auto delay    = std::chrono::milliseconds{_requested_timeout_ms};
    _timer        = _loop.post_delayed(delay, [callback = std::move(callback)]() -> void {
        if (callback->owner) {
            callback->owner->handle_timeout();
        }
    });
    if (!_timer) {
        enter_failed("event_loop.timer_registration_failed");
        return false;
    }
    return true;
}

auto CurlMultiAdapter::check_callback_failure() -> bool
{
    if (!_curl_callback_failed) {
        return true;
    }
    _curl_callback_failed = false;
    return false;
}

void CurlMultiAdapter::enter_failed(std::string_view reason)
{
    if (_failure) {
        return;
    }

    _failure               = backend_failed(reason);
    _accept_curl_callbacks = false;
    if (_timer) {
        _loop.cancel_timer(_timer);
        _timer = {};
    }

    if (_multi) {
        for (auto* easy : _easy_handles) {
            static_cast<void>(curl_multi_remove_handle(_multi, easy));
        }
        _easy_handles.clear();
        static_cast<void>(curl_multi_cleanup(_multi));
        _multi = nullptr;
    }

    for (auto& [fd, watch] : _watches) {
        (void)fd;
        watch.callback->owner = nullptr;
    }
    if (_inside_socket_callback) {
        schedule_deferred_detach();
    }
    else {
        detach_watches();
    }
    _pending_watch_updates.clear();
    _timer_update_pending = false;
    _failure_handler(*_failure);
}

void CurlMultiAdapter::schedule_deferred_detach() noexcept
{
    if (_detach_queued) {
        return;
    }
    _detach_queued = true;
    auto callback  = _callback_state;
    auto task      = [callback = std::move(callback)]() -> void {
        if (callback->owner) {
            callback->owner->run_deferred_detach();
        }
    };
    if (!_loop.post(task)) {
        static_cast<void>(_loop.post_delayed(jb::core::Duration::zero(), std::move(task)));
    }
}

void CurlMultiAdapter::shutdown() noexcept
{
    _accept_curl_callbacks = false;
    if (_callback_state) {
        _callback_state->owner = nullptr;
    }
    if (_timer) {
        _loop.cancel_timer(_timer);
        _timer = {};
    }

    if (_multi) {
        for (auto* easy : _easy_handles) {
            static_cast<void>(curl_multi_remove_handle(_multi, easy));
        }
        _easy_handles.clear();
        static_cast<void>(curl_multi_cleanup(_multi));
        _multi = nullptr;
    }
    detach_watches();
}

void CurlMultiAdapter::detach_watches() noexcept
{
    for (auto& [fd, watch] : _watches) {
        (void)fd;
        watch.callback->owner = nullptr;
        if (!_loop.unwatch_fd(watch.handle)) {
            // Closing curl's sockets normally detaches the native registration; retry once so EventLoop can discard
            // its retained callback. A persistent failure remains safe because the callback anchor is inert.
            static_cast<void>(_loop.unwatch_fd(watch.handle));
        }
    }
    _watches.clear();
}

} // namespace jb::net::http::detail
