#pragma once

#include "event_loop_types.hpp"
#include "object.hpp"
#include "signal.hpp"

namespace jb::core {

/// Timer class that can be used to schedule a callback to be called after a certain
/// amount of time has passed.
///
/// Timer object use the event loop of the thread they are created in to schedule
/// the timer events. Therefore, a Timer object must be created in a thread that has
/// an event loop running, like the main Application thread or an EventThread thread.
///
/// Threading contract: A Timer object must be created and used in the same thread.
/// It is not thread-safe to use a Timer object from multiple threads.
class Timer : public Object {
public:

    /// Constructor
    /// @param[in] parent Optional parent object.
    ///
    /// Constructs a new Timer object with zero interval and non-repeating by default.
    /// The timer will not be active until `start()` is called.
    explicit Timer(Object* parent = nullptr);

    /// Destructor
    ~Timer() override;

    /// Returns the timer handle
    /// @return Timer handle if the timer is active, or an invalid handle if the timer is not active.
    auto handle() const -> TimerHandle { return _handle; }

    /// Returns true if the timer is active
    auto is_active() const -> bool { return static_cast<bool>(_handle); }

    /// Returns true if the timer is repeating
    auto is_repeating() const -> bool { return _repeating; }

    /// Sets the timer interval
    /// @param[in] interval Timer interval.
    ///
    /// If the timer is active, it will be restarted with the new interval.
    void set_interval(Duration interval);

    /// Sets the timer to be repeating or non-repeating
    /// @param[in] repeating True to make the timer repeating, false to make it non-repeating.
    ///
    /// Changing the repeating flag does not affect the timer's active state.
    /// If the timer is active, it will continue to run with the new repeating setting.
    void set_repeating(bool repeating) { _repeating = repeating; }

    /// Starts the timer uwing the current interval and repeating settings.
    /// If the timer is already active, it will be restarted with the current settings.
    void start();

    /// Starts the timer using the given interval.
    /// @param[in] interval Timer interval.
    ///
    /// If the timer is already active, it will be restarted with the new interval and
    /// current repeating settings.
    void start(Duration interval);

    /// Stops the timer.
    /// If the timer is not active, this method does nothing.
    void stop();

    //--- SIGNALS ---

    /// Timer timeout signal
    Signal<> timeout;

private:

    /// Timer handle
    TimerHandle _handle;

    /// Current interval
    Duration _interval{0};

    /// Flag indicating whether the timer is repeating
    bool _repeating{false};
};

} // namespace jb::core
