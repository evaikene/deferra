#include "timer.hpp"

#include "event_loop.hpp"
#include "timer_priv.hpp"

namespace jb::core {

Timer::Timer(Object* parent)
    : Timer(*new priv::TimerPrivate, parent)
{}

Timer::Timer(priv::TimerPrivate& dd, Object* parent)
    : Object(dd, parent)
{}

Timer::~Timer()
{
    // Cancel EventLoop callbacks while Timer's timeout signal still exists.
    stop();
}

auto Timer::handle() const -> TimerHandle
{
    return d_ptr<priv::TimerPrivate const>()->handle;
}

auto Timer::is_active() const -> bool
{
    return static_cast<bool>(d_ptr<priv::TimerPrivate const>()->handle);
}

auto Timer::is_repeating() const -> bool
{
    return d_ptr<priv::TimerPrivate const>()->repeating;
}

void Timer::set_interval(Duration interval)
{
    auto*      data   = d_ptr<priv::TimerPrivate>();
    auto const active = is_active();

    // stop the timer if it is currently active
    if (active) {
        stop();
    }

    data->interval = interval;

    // restart the timer if it was active before
    if (active) {
        start();
    }
}

void Timer::set_repeating(bool repeating)
{
    d_ptr<priv::TimerPrivate>()->repeating = repeating;
}

void Timer::start()
{
    auto* data = d_ptr<priv::TimerPrivate>();

    // must have an event loop
    if (!event_loop()) {
        return;
    }

    // stop the timer if it is currently active
    if (is_active()) {
        stop();
    }

    // start the timer with the current interval and repeating settings
    if (data->repeating && data->interval.count() > 0) {
        data->handle = event_loop()->post_repeating(data->interval, [this]() -> void { emit(timeout); });
    }
    else {
        data->handle = event_loop()->post_delayed(data->interval, [this]() -> void {
            d_ptr<priv::TimerPrivate>()->handle = TimerHandle{};
            emit(timeout);
        });
    }
}

void Timer::start(Duration interval)
{
    d_ptr<priv::TimerPrivate>()->interval = interval;
    start();
}

void Timer::stop()
{
    auto* data = d_ptr<priv::TimerPrivate>();

    // must have an event loop and an active timer
    if (!event_loop() || !data->handle) {
        return;
    }

    event_loop()->cancel_timer(data->handle);
    data->handle = TimerHandle{};
}

} // namespace jb::core
