#pragma once

#include "event_loop_types.hpp"
#include "object.hpp"
#include "signal.hpp"

#include <memory>

namespace jb::core {

class Event;
class EventThread;

namespace priv {
struct ApplicationPrivate; // defined in application_priv.hpp
} // namespace priv

/// Main application class.
///
/// This class is responsible for initializing the application and running the
/// main event loop. It also provides a way to signal the application to quit.
///
/// There should be only one instance of this class in the application and it
/// should be created on the main thread before any other threads are started.
class Application : public Object {
public:

    /// Returns the global application instance
    /// @return Global application instance
    static auto instance() -> Application* { return s_instance; }

    /// Constructor
    /// @param[in] argc Argument count from the main function
    /// @param[in] argv Argument vector from the main function
    explicit Application(int argc, char const* argv[]);

    /// Destructor
    ~Application() override;

    /// Sends an event synchronously to an object.
    /// @param[in] receiver Object receiving the event; nullptr is allowed
    /// @param[in,out] event Event to dispatch
    /// @return The value returned by receiver's event handler, or false for a null receiver
    ///
    /// This method dispatches only to @p receiver and does not propagate the
    /// event through the parent ownership tree.
    static auto send_event(Object* receiver, Event& event) -> bool;

    /// Posts an event for asynchronous delivery to an object.
    /// @param[in] receiver Object receiving the event
    /// @param[in] event Event to deliver
    ///
    /// This method is thread-safe. A null receiver or event is ignored. Events
    /// posted to an object without an EventLoop are logged and dropped. Events
    /// that cannot wake the receiver's EventLoop are also logged and dropped.
    /// Events whose receiver is destroyed before delivery are silently discarded.
    static void post_event(Object* receiver, std::unique_ptr<Event> event);

    /// Returns the event thread this application is running on
    auto thread() const -> EventThread*;

    /// Runs the application event loop until quit is signaled to quit
    /// @return the requested exit code after an ordinary stop, or `EXIT_FAILURE`
    ///         when the event loop fails
    auto exec() -> int;

    /// Processes specified events until there are no more events to process
    /// @param[in] flags Events to process (tasks, object events, timers, watchers)
    /// @return `Running` or `Stopped` after successful processing according to
    ///         the event-loop state, or `Failed` when processing cannot complete
    ///
    /// This method is NOT thread-safe and must be called from the thread running the event loop.
    auto process_events(EventFlags flags) -> ProcessEventsResult;

    /// Processes specified events for `ms` milliseconds, or until there are no more events
    /// to process, whichever comes first.
    /// @param[in] flags Events to process (tasks, object events, timers, watchers)
    /// @param[in] ms Maximum time to process events in milliseconds (negative means no timeout)
    /// @return `Running` or `Stopped` after successful processing according to
    ///         the event-loop state, or `Failed` when processing cannot complete
    ///
    /// The `ms` timeout applies only to fd events. If `EventFlag::Watchers` is not set in `flags`,
    /// then `ms` is ignored and this method behaves the same as `process_events(flags)`.
    ///
    /// This method is NOT thread-safe and must be called from the thread running the event loop.
    auto process_events(EventFlags flags, int ms) -> ProcessEventsResult;

    /// Signals the application to quit with the given exit code
    /// @param[in] exit_code Exit code to quit with (default: 0)
    /// @return true when the stop task was queued, false when the caller must not
    ///         assume the event loop was woken
    ///
    /// The requested exit code is stored even if the stop task cannot be queued.
    auto quit(int exit_code = 0) -> bool;

    /// Returns the exit code of the application after it has finished executing
    /// @return Exit code of the application
    auto exit_code() const -> int;

    //--- SIGNALS ---

    /// Signal emitted when the application is about to start.
    Signal<> about_to_start;

    /// Signal emitted when the application is about to quit.
    Signal<> about_to_quit;

protected:

    /// Constructor for subclasses that supply their own private data.
    /// @param[in] dd Reference to a heap-allocated struct that inherits directly
    ///               or transitively from priv::ApplicationPrivate. Application
    ///               takes ownership; do NOT delete @p dd elsewhere.
    /// @param[in] argc Argument count from the main function
    /// @param[in] argv Argument vector from the main function
    Application(priv::ApplicationPrivate& dd, int argc, char const* argv[]);

private:
    static Application* s_instance;
};

} // namespace jb::core
