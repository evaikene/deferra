#include "event_thread.hpp"

#include "application.hpp"
#include "event_loop.hpp"
#include "thread_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace jb::core;

TEST_CASE("EventThread initial state and never-started destruction", "[core][event_thread]")
{
    Application app{0, nullptr};

    {
        EventThread event_thread;

        CHECK(event_thread.thread_ctx() == ThreadCtx::current());
        CHECK(event_thread.event_loop() == event_thread.as_event_loop());
        CHECK(event_thread.as_event_loop() != app.event_loop());
        CHECK_FALSE(event_thread.is_running());
        CHECK(event_thread.exit_code() == 0);
    }
}

TEST_CASE("EventThread exec waits for its event loop", "[core][event_thread]")
{
    Application app{0, nullptr};
    EventThread event_thread;

    std::vector<int> signal_order;
    EventLoop*       start_loop{nullptr};
    EventLoop*       quit_loop{nullptr};

    event_thread.about_to_start.connect([&]() -> void {
        signal_order.push_back(1);
        start_loop = EventLoop::current();
    });
    event_thread.about_to_quit.connect([&]() -> void {
        signal_order.push_back(2);
        quit_loop = EventLoop::current();
    });

    CHECK(event_thread.exec(true));
    CHECK(event_thread.is_running());
    CHECK(event_thread.thread_ctx() != ThreadCtx::current());

    CHECK(event_thread.quit(1));
    CHECK_NOTHROW(event_thread.wait());

    auto const expected_order = std::vector<int>{1, 2};
    CHECK(signal_order == expected_order);
    CHECK(start_loop == event_thread.as_event_loop());
    CHECK(quit_loop == event_thread.as_event_loop());
    CHECK(event_thread.as_event_loop()->thread_ctx() == ThreadCtx::current());
    CHECK(event_thread.exit_code() == 1);
}

TEST_CASE("EventThread exec can return before its event loop runs", "[core][event_thread]")
{
    EventThread      event_thread;
    std::atomic_bool start_slot_entered{false};
    std::atomic_bool allow_event_loop{false};

    event_thread.about_to_start.connect([&]() -> void {
        start_slot_entered.store(true, std::memory_order_release);
        while (!allow_event_loop.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    CHECK(event_thread.exec(false));

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!start_slot_entered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    if (!start_slot_entered.load(std::memory_order_acquire)) {
        allow_event_loop.store(true, std::memory_order_release);
        FAIL("about_to_start slot did not run before the deadline");
    }

    CHECK_FALSE(event_thread.is_running());
    auto const quit_queued = event_thread.quit();
    allow_event_loop.store(true, std::memory_order_release);

    CHECK(quit_queued);
    event_thread.wait();
}

TEST_CASE("EventThread destructor stops and joins a started thread", "[core][event_thread]")
{
    std::atomic_bool about_to_start_emitted{false};
    std::atomic_bool about_to_quit_emitted{false};

    {
        EventThread event_thread;
        event_thread.about_to_start.connect(
            [&about_to_start_emitted]() -> void { about_to_start_emitted.store(true, std::memory_order_release); });
        event_thread.about_to_quit.connect(
            [&about_to_quit_emitted]() -> void { about_to_quit_emitted.store(true, std::memory_order_release); });

        REQUIRE(event_thread.start());
    }

    CHECK(about_to_start_emitted.load(std::memory_order_acquire));
    CHECK(about_to_quit_emitted.load(std::memory_order_acquire));
}
