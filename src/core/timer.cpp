#include "timer.hpp"

#include "event_loop.hpp"

namespace jb::core {

Timer::Timer(Object* parent)
    : Object(parent)
{}

Timer::~Timer()
{
    stop();
}

void Timer::set_interval(Duration interval)
{
    auto active = is_active();

    // stop the timer if it is currently active
    if (active) {
        stop();
    }

    _interval = interval;

    // restart the timer if it was active before
    if (active) {
        start();
    }
}

void Timer::start()
{
    // must have an event loop
    if (!event_loop()) {
        return;
    }

    // stop the timer if it is currently active
    if (is_active()) {
        stop();
    }

    // start the timer with the current interval and repeating settings
    if (_repeating && _interval.count() > 0) {
        _handle = event_loop()->post_repeating(_interval, [this]() -> void {
            emit(timeout);
        });
    }
    else {
        _handle = event_loop()->post_delayed(_interval, [this]() -> void {
            _handle = TimerHandle{};
            emit(timeout);
        });
    }
}

void Timer::start(Duration interval)
{
    _interval = interval;
    start();
}

void Timer::stop()
{
    // must have an event loop and an active timer
    if (!event_loop() || !is_active()) {
        return;
    }

    event_loop()->cancel_timer(_handle);
    _handle = TimerHandle{};
}

} // namespace jb::core
