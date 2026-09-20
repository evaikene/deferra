#include "shutdown_signal_priv.hpp"

#include "event_loop.hpp"
#include "event_loop_types.hpp"
#include "logging.hpp"
#include "shutdown_signal_linux_priv.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal> // IWYU pragma: keep POSIX signal sets and dispositions.
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace jb::jobud::detail {

namespace {

using VoidResult  = jb::core::Result<void, jb::core::Error>;
using RelayResult = jb::core::Result<std::unique_ptr<ShutdownSignalRelay>, jb::core::Error>;
using WatchResult = jb::core::Result<std::unique_ptr<ShutdownSignalWatch>, jb::core::Error>;

// Published with both signals blocked before any worker exists. Neither descriptor nor ownership
// changes until workers have joined and the owner has blocked both signals again.
volatile sig_atomic_t shutdown_requested{0};
volatile sig_atomic_t handler_descriptor{-1};
bool                  relay_owned{false};

// sig_atomic_t protects interrupted access on one thread, not concurrent worker handlers. Only the
// winning handler writes the flag; release/acquire publication makes every subsequent read safe.
// These atomics are required to be lock-free so their use remains async-signal-safe.
static_assert(std::atomic<unsigned>::is_always_lock_free);
std::atomic<unsigned> request_publication{0}; // 0: unclaimed, 1: publishing, 2: immutable flag is visible

void handle_shutdown_signal(int /*signal*/) noexcept
{
    auto const saved_errno = errno;
    unsigned   unclaimed{0};
    if (request_publication.compare_exchange_strong(unclaimed, 1, std::memory_order_relaxed)) {
        shutdown_requested = 1;
        request_publication.store(2, std::memory_order_release);
    }

    // A full pipe already wakes the loop. Retry EINTR so an unrelated handler cannot lose the wakeup.
    char const byte{'s'};
    while (::write(handler_descriptor, &byte, 1) < 0 && errno == EINTR) {
    }
    errno = saved_errno;
}

auto has_shutdown_request() noexcept -> bool
{
    return request_publication.load(std::memory_order_acquire) == 2 && shutdown_requested != 0;
}

auto signal_error(std::string_view code, std::string_view reason) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::Unavailable,
            .code     = std::string{code},
            .message  = "The daemon shutdown signal relay is unavailable",
            .detail   = std::string{reason}};
}

auto shutdown_signals() -> sigset_t
{
    sigset_t signals;
    ::sigemptyset(&signals);
    ::sigaddset(&signals, SIGTERM);
    ::sigaddset(&signals, SIGINT);
    return signals;
}

} // namespace

struct ShutdownSignalWatch::State {
    jb::core::EventLoop*  loop{};
    jb::core::FdWatch     watch;
    int                   descriptor{-1};
    std::function<void()> notify;
    bool                  active{true};
    bool                  delivered{false};
};

struct ShutdownSignalRelay::State {
    ShutdownSignalOperations                  operations;
    std::array<int, 2>                        descriptors{-1, -1};
    sigset_t                                  original_mask{};
    struct sigaction                          original_term{};
    struct sigaction                          original_int{};
    bool                                      mask_saved{false};
    bool                                      term_installed{false};
    bool                                      int_installed{false};
    bool                                      owns_relay{false};
    std::weak_ptr<ShutdownSignalWatch::State> watch;
};

ShutdownSignalRelay::ShutdownSignalRelay(std::unique_ptr<State> state)
    : _state{std::move(state)}
{}

ShutdownSignalRelay::~ShutdownSignalRelay()
{
    auto result = close();
    if (!result) {
        jb::core::log_error("Unable to restore daemon signal ownership: {} ({})",
                            result.error().detail,
                            result.error().code);
    }
}

auto ShutdownSignalRelay::install() -> RelayResult
{
    return install(ShutdownSignalOperations{});
}

auto ShutdownSignalRelay::install(ShutdownSignalOperations const& operations) -> RelayResult
{
    if (relay_owned) {
        return RelayResult::failure(signal_error("jobud.signal.active", "already_installed"));
    }

    auto state        = std::make_unique<State>();
    state->operations = operations;
    auto  relay       = std::unique_ptr<ShutdownSignalRelay>{new ShutdownSignalRelay{std::move(state)}};
    auto& data        = *relay->_state;
    auto  fail        = [&relay](std::string_view reason) -> RelayResult {
        auto cleanup = relay->close();
        if (!cleanup) {
            return RelayResult::failure(cleanup.error());
        }
        return RelayResult::failure(signal_error("jobud.signal.setup", reason));
    };

    auto const signals = shutdown_signals();
    if (operations.mask(SIG_BLOCK, &signals, &data.original_mask) != 0) {
        return fail("block_signals");
    }
    data.mask_saved = true;
    data.owns_relay = true;
    relay_owned     = true;

    if (operations.pipe(data.descriptors.data(), O_NONBLOCK | O_CLOEXEC) != 0) {
        return fail("create_pipe");
    }

    // Both handlers mask both target signals, preventing nested TERM/INT delivery on a handler thread.
    struct sigaction action{};
    action.sa_handler  = &handle_shutdown_signal;
    action.sa_mask     = signals;
    action.sa_flags    = SA_RESTART;
    handler_descriptor = data.descriptors[1];
    if (operations.action(SIGTERM, &action, &data.original_term) != 0) {
        return fail("install_sigterm");
    }
    data.term_installed = true;
    if (operations.action(SIGINT, &action, &data.original_int) != 0) {
        return fail("install_sigint");
    }
    data.int_installed = true;

    // Deliberately enable delivery even when the launching shell blocked a target signal. Workers
    // created afterward inherit this mask; the process-main lifetime contract includes their join.
    auto serving_mask = data.original_mask;
    ::sigdelset(&serving_mask, SIGTERM);
    ::sigdelset(&serving_mask, SIGINT);
    if (operations.mask(SIG_SETMASK, &serving_mask, nullptr) != 0) {
        return fail("enable_signals");
    }
    return RelayResult::success(std::move(relay));
}

auto ShutdownSignalRelay::requested() const noexcept -> bool
{
    return has_shutdown_request();
}

auto ShutdownSignalRelay::attach(jb::core::EventLoop& loop, std::function<void()> notify) -> WatchResult
{
    auto const previous = _state->watch.lock();
    if (!_state->owns_relay || !notify || (previous && previous->active)) {
        return WatchResult::failure(signal_error("jobud.signal.setup", "invalid_watch"));
    }

    auto state        = std::make_shared<ShutdownSignalWatch::State>();
    state->loop       = &loop;
    state->descriptor = _state->descriptors[0];
    state->notify     = std::move(notify);
    auto dispatch     = [state](int /*fd*/, jb::core::FdEvents /*events*/) {
        if (!state->active) {
            return;
        }

        // One bounded read keeps a signal storm from monopolizing dispatch. Level readiness keeps
        // remaining bytes observable; the monotonic flag carries meaning, not the byte count.
        std::array<char, 256> bytes{};
        static_cast<void>(::read(state->descriptor, bytes.data(), bytes.size()));
        if (has_shutdown_request() && !state->delivered) {
            state->delivered = true;
            state->notify();
        }
    };
    state->watch =
        loop.watch_fd(state->descriptor, jb::core::FdEvent::Read, jb::core::FdTriggerMode::Level, std::move(dispatch));
    if (!state->watch) {
        return WatchResult::failure(signal_error("jobud.signal.setup", "watch_pipe"));
    }
    _state->watch = state;
    return WatchResult::success(std::unique_ptr<ShutdownSignalWatch>{new ShutdownSignalWatch{std::move(state)}});
}

auto ShutdownSignalRelay::close() -> VoidResult
{
    auto&      data  = *_state;
    auto const watch = data.watch.lock();
    if (watch && watch->active) {
        return VoidResult::failure(signal_error("jobud.signal.cleanup", "watch_still_attached"));
    }
    if (!data.mask_saved) {
        return VoidResult::success();
    }

    // The caller has joined every possible handler thread. Blocking this last thread now makes
    // disposition restoration and descriptor retirement safe against close/reuse races.
    auto const signals = shutdown_signals();
    if (data.operations.mask(SIG_BLOCK, &signals, nullptr) != 0) {
        return VoidResult::failure(signal_error("jobud.signal.cleanup", "block_signals"));
    }
    if (data.term_installed) {
        if (data.operations.action(SIGTERM, &data.original_term, nullptr) != 0) {
            return VoidResult::failure(signal_error("jobud.signal.cleanup", "restore_sigterm"));
        }
        data.term_installed = false;
    }
    if (data.int_installed) {
        if (data.operations.action(SIGINT, &data.original_int, nullptr) != 0) {
            return VoidResult::failure(signal_error("jobud.signal.cleanup", "restore_sigint"));
        }
        data.int_installed = false;
    }

    handler_descriptor = -1;
    for (auto& descriptor : data.descriptors) {
        if (descriptor >= 0) {
            // Linux closes the descriptor even when close reports EINTR; never retry a reused number.
            ::close(descriptor);
            descriptor = -1;
        }
    }
    if (data.operations.mask(SIG_SETMASK, &data.original_mask, nullptr) != 0) {
        return VoidResult::failure(signal_error("jobud.signal.cleanup", "restore_mask"));
    }
    data.mask_saved = false;
    data.owns_relay = false;
    relay_owned     = false;
    return VoidResult::success();
}

ShutdownSignalWatch::ShutdownSignalWatch(std::shared_ptr<State> state)
    : _state{std::move(state)}
{}

ShutdownSignalWatch::~ShutdownSignalWatch()
{
    // EventLoop can retain a copied callback, including after failed native removal. Invalidate it
    // before releasing any borrowed target; its shared state then contains no usable owner pointer.
    _state->active = false;
    _state->notify = {};
    if (!_state->loop->unwatch_fd(_state->watch)) {
        jb::core::log_error("Unable to remove daemon shutdown signal watch (jobud.signal.cleanup)");
    }
}

} // namespace jb::jobud::detail
