#pragma once

#include "object.hpp"
#include "signal.hpp"

namespace jb::core {

class EventThread;

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

    /// Returns the event thread this application is running on
    auto thread() const -> EventThread* { return _event_loop.get(); }

    /// Runs the application event loop until quit is signaled to quit
    /// @return Exit code (0 for success, non-zero for failure)
    auto exec() -> int;

    /// Processes all pending events in the event loop without blocking
    /// @return True if the event loop is still running; false if it has been quit
    auto process_events() -> bool;

    /// Signals the application to quit with the given exit code
    /// @param[in] exit_code Exit code to quit with (default: 0)
    void quit(int exit_code = 0);

    /// Returns the exit code of the application after it has finished executing
    /// @return Exit code of the application
    auto exit_code() const -> int { return _exit_code; }

    //--- SIGNALS ---

    /// Signal emitted when the application is about to start.
    Signal<> about_to_start;

    /// Signal emitted when the application is about to quit.
    Signal<> about_to_quit;

private:

    static Application* s_instance;

    int                          _argc      = 0;
    char const**                 _argv      = nullptr;
    int                          _exit_code = 0;
    std::unique_ptr<EventThread> _event_loop;
};

} // namespace jb::core
