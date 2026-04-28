#include "timer.hpp"
#include "application.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace jb::core;
using namespace std::chrono_literals;

TEST_CASE("Timer initial state", "[core][timer]")
{
    Application app{0, nullptr};

    Timer t;
    REQUIRE_FALSE(t.is_active());
    REQUIRE_FALSE(t.is_repeating());
    REQUIRE_FALSE(t.handle());
}

TEST_CASE("Timer start activates the timer", "[core][timer]")
{
    Application app{0, nullptr};

    Timer t;
    t.start(100ms);
    REQUIRE(t.is_active());
    REQUIRE(t.handle());
    t.stop();
}

TEST_CASE("Timer stop deactivates the timer", "[core][timer]")
{
    Application app{0, nullptr};

    Timer t;
    t.start(100ms);
    REQUIRE(t.is_active());

    t.stop();
    REQUIRE_FALSE(t.is_active());
    REQUIRE_FALSE(t.handle());
}

TEST_CASE("Timer stop on inactive timer is a no-op", "[core][timer]")
{
    Application app{0, nullptr};

    Timer t;
    REQUIRE_NOTHROW(t.stop());
    REQUIRE_FALSE(t.is_active());
}

TEST_CASE("Timer one-shot fires timeout signal after deadline", "[core][timer]")
{
    Application app{0, nullptr};

    int count = 0;
    Timer t;
    t.timeout.connect(nullptr, [&count]() -> void { ++count; });
    t.start(10ms);

    std::this_thread::sleep_for(30ms);
    app.process_events(EventFlag::Timers);

    REQUIRE(count == 1);
}

TEST_CASE("Timer one-shot does not fire before deadline", "[core][timer]")
{
    Application app{0, nullptr};

    int count = 0;
    Timer t;
    t.timeout.connect(nullptr, [&count]() -> void { ++count; });
    t.start(100ms);

    app.process_events(EventFlag::Timers);

    REQUIRE(count == 0);
    t.stop();
}

TEST_CASE("Timer one-shot fires only once", "[core][timer]")
{
    Application app{0, nullptr};

    int count = 0;
    Timer t;
    t.timeout.connect(nullptr, [&count]() -> void { ++count; });
    t.start(10ms);

    std::this_thread::sleep_for(30ms);
    app.process_events(EventFlag::Timers);
    REQUIRE(count == 1);

    std::this_thread::sleep_for(30ms);
    app.process_events(EventFlag::Timers);
    REQUIRE(count == 1);
}

TEST_CASE("Timer repeating fires timeout signal multiple times", "[core][timer]")
{
    Application app{0, nullptr};

    int count = 0;
    Timer t;
    t.set_repeating(true);
    t.timeout.connect(nullptr, [&count]() -> void { ++count; });
    t.start(10ms);

    std::this_thread::sleep_for(15ms);
    app.process_events(EventFlag::Timers);
    REQUIRE(count >= 1);
    REQUIRE(t.is_active());

    std::this_thread::sleep_for(15ms);
    app.process_events(EventFlag::Timers);
    REQUIRE(count >= 2);

    t.stop();
}

TEST_CASE("Timer repeating remains active after firing", "[core][timer]")
{
    Application app{0, nullptr};

    Timer t;
    t.set_repeating(true);
    t.start(10ms);

    std::this_thread::sleep_for(30ms);
    app.process_events(EventFlag::Timers);

    REQUIRE(t.is_active());
    t.stop();
}

TEST_CASE("Timer start with interval overload", "[core][timer]")
{
    Application app{0, nullptr};

    int count = 0;
    Timer t;
    t.timeout.connect(nullptr, [&count]() -> void { ++count; });
    t.start(10ms);

    std::this_thread::sleep_for(30ms);
    app.process_events(EventFlag::Timers);

    REQUIRE(count == 1);
}

TEST_CASE("Timer set_interval changes the interval", "[core][timer]")
{
    Application app{0, nullptr};

    int count = 0;
    Timer t;
    t.timeout.connect(nullptr, [&count]() -> void { ++count; });
    t.set_interval(100ms);
    t.start();

    app.process_events(EventFlag::Timers);
    REQUIRE(count == 0);

    t.stop();
}

TEST_CASE("Timer set_interval on active timer restarts it", "[core][timer]")
{
    Application app{0, nullptr};

    int count = 0;
    Timer t;
    t.timeout.connect(nullptr, [&count]() -> void { ++count; });
    t.start(100ms);
    REQUIRE(t.is_active());

    // Change to a short interval — timer restarts from now
    std::this_thread::sleep_for(20ms);
    t.set_interval(10ms);
    REQUIRE(t.is_active());

    std::this_thread::sleep_for(30ms);
    app.process_events(EventFlag::Timers);
    REQUIRE(count == 1);
}

TEST_CASE("Timer start restarts an already active timer", "[core][timer]")
{
    Application app{0, nullptr};

    int count = 0;
    Timer t;
    t.timeout.connect(nullptr, [&count]() -> void { ++count; });
    t.start(50ms);

    // Restart the timer before it fires — deadline resets
    std::this_thread::sleep_for(20ms);
    t.start(50ms);

    // Only 20ms elapsed since restart, timer should not have fired
    std::this_thread::sleep_for(20ms);
    app.process_events(EventFlag::Timers);
    REQUIRE(count == 0);

    std::this_thread::sleep_for(40ms);
    app.process_events(EventFlag::Timers);
    REQUIRE(count == 1);
}

TEST_CASE("Timer destructor stops the timer", "[core][timer]")
{
    Application app{0, nullptr};

    int count = 0;
    {
        Timer t;
        t.timeout.connect(nullptr, [&count]() -> void { ++count; });
        t.start(50ms);
        REQUIRE(t.is_active());
    } // destructor stops the timer

    std::this_thread::sleep_for(70ms);
    app.process_events(EventFlag::Timers);
    REQUIRE(count == 0);
}

TEST_CASE("Timer set_repeating does not affect active state", "[core][timer]")
{
    Application app{0, nullptr};

    Timer t;
    t.start(100ms);
    REQUIRE(t.is_active());

    t.set_repeating(true);
    REQUIRE(t.is_active());

    t.set_repeating(false);
    REQUIRE(t.is_active());

    t.stop();
}
