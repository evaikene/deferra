#include "shutdown_signal_priv.hpp"

#include "event_loop.hpp"
#include "event_loop_types.hpp"
#include "logging.hpp"
#include "shutdown_signal_linux_priv.hpp"

#ifdef JOBUD_SIGNAL_TESTING
#  include "shutdown_signal_test_priv.hpp"
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal> // IWYU pragma: keep POSIX signal sets and dispositions.
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

namespace jb::jobud::detail {

namespace {

using VoidResult  = jb::core::Result<void, jb::core::Error>;
using RelayResult = jb::core::Result<std::unique_ptr<ShutdownSignalRelay>, jb::core::Error>;
using WatchResult = jb::core::Result<std::unique_ptr<ShutdownSignalWatch>, jb::core::Error>;

// Handler-visible storage has process lifetime: even a handler selected before disposition
// restoration may enter after the relay object is gone. The descriptor is protected separately.
volatile sig_atomic_t shutdown_requested{0};
int                   handler_descriptor{-1};
bool                  relay_owned{false};

// sig_atomic_t protects interrupted access on one thread, not concurrent worker handlers. Only the
// winning handler writes the flag; release/acquire publication makes every subsequent read safe.
// These atomics are required to be lock-free so their use remains async-signal-safe.
static_assert(std::atomic<unsigned>::is_always_lock_free);
std::atomic<unsigned> request_publication{0}; // 0: unclaimed, 1: publishing, 2: immutable flag is visible

// One atomic word combines admission and the number of handlers borrowing the descriptor.
// A separate flag and counter would let retirement miss a handler between its check and increment.
constexpr unsigned    kHandlerAdmissionClosed = 1U << (std::numeric_limits<unsigned>::digits - 1);
std::atomic<unsigned> handler_access{kHandlerAdmissionClosed};

auto acquire_handler_descriptor() noexcept -> bool
{
    auto access = handler_access.load(std::memory_order_relaxed);
    while (access < kHandlerAdmissionClosed - 1U) {
        // Acquire the published descriptor only if admission is still open. Reserve the high bit
        // for retirement and refuse counter overflow rather than allowing it to reopen the gate.
        if (handler_access.compare_exchange_weak(access, access + 1U, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void retire_handler_descriptor() noexcept
{
    handler_access.fetch_or(kHandlerAdmissionClosed, std::memory_order_acq_rel);
    // Wait only for already admitted signal handlers, never for the workers that ran them. Late
    // handlers cannot acquire the descriptor. The final acquire observes every borrower's release
    // before close/reuse; yielding here is owner-thread teardown, not signal-handler work.
    while (handler_access.load(std::memory_order_acquire) != kHandlerAdmissionClosed) {
#ifdef JOBUD_SIGNAL_TESTING
        signal_test_checkpoint(SignalTestCheckpoint::RetirementWaiting);
#endif
        ::sched_yield();
    }
    handler_descriptor = -1;
}

void handle_shutdown_signal(int /*signal*/) noexcept
{
    auto const saved_errno = errno;
#ifdef JOBUD_SIGNAL_TESTING
    signal_test_checkpoint(SignalTestCheckpoint::HandlerEntered);
#endif
    unsigned unclaimed{0};
    if (request_publication.compare_exchange_strong(unclaimed, 1, std::memory_order_relaxed)) {
        shutdown_requested = 1;
        request_publication.store(2, std::memory_order_release);
    }

    if (acquire_handler_descriptor()) {
#ifdef JOBUD_SIGNAL_TESTING
        signal_test_checkpoint(SignalTestCheckpoint::DescriptorAcquired);
#endif
        // A full pipe already wakes the loop. Retry EINTR so an unrelated handler cannot lose the wakeup.
        char const byte{'s'};
        while (::write(handler_descriptor, &byte, 1) < 0 && errno == EINTR) {
        }
#ifdef JOBUD_SIGNAL_TESTING
        signal_test_checkpoint(SignalTestCheckpoint::DescriptorWritten);
#endif
        handler_access.fetch_sub(1U, std::memory_order_release);
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
    handler_access.store(0U, std::memory_order_release);
    if (operations.action(SIGTERM, &action, &data.original_term) != 0) {
        return fail("install_sigterm");
    }
    data.term_installed = true;
    if (operations.action(SIGINT, &action, &data.original_int) != 0) {
        return fail("install_sigint");
    }
    data.int_installed = true;

    // Deliberately enable delivery even when the launching shell blocked a target signal. Workers
    // created afterward inherit this mask. Retirement fences descriptor access even if a library
    // worker outlives its owner (for example, a detached libcurl DNS resolver).
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

    // Block owner-thread delivery while restoring dispositions. Other threads can still be inside
    // our handler, so restoring the dispositions alone does not permit descriptor retirement.
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

    retire_handler_descriptor();
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
