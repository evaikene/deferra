#include "event_loop.hpp"
#include "thread_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace jb::core;
using namespace std::chrono_literals;

TEST_CASE("EventLoop initial state", "[core][event_loop]")
{
    EventLoop loop;
    REQUIRE_FALSE(loop.is_running());
    REQUIRE(loop.thread_ctx() == ThreadCtx::current());
}

TEST_CASE("EventLoop process_events executes a posted task", "[core][event_loop]")
{
    EventLoop loop;
    int counter = 0;
    loop.post([&counter]() -> void { ++counter; });
    loop.process_events();
    REQUIRE(counter == 1);
}

TEST_CASE("EventLoop process_events executes multiple tasks in order", "[core][event_loop]")
{
    EventLoop loop;
    std::vector<int> order;
    loop.post([&order]() -> void { order.push_back(1); });
    loop.post([&order]() -> void { order.push_back(2); });
    loop.post([&order]() -> void { order.push_back(3); });
    loop.process_events();
    REQUIRE(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("EventLoop process_events returns false when loop is not running", "[core][event_loop]")
{
    EventLoop loop;
    REQUIRE_FALSE(loop.process_events());
}

TEST_CASE("EventLoop run and quit", "[core][event_loop][thread]")
{
    EventLoop loop;
    REQUIRE_FALSE(loop.is_running());

    std::thread t([&loop]() -> void { loop.run(); });
    while (!loop.is_running()) {
        std::this_thread::yield();
    }

    REQUIRE(loop.is_running());
    loop.quit();
    t.join();

    REQUIRE_FALSE(loop.is_running());
}

TEST_CASE("EventLoop run executes tasks posted from another thread", "[core][event_loop][thread]")
{
    EventLoop loop;
    std::atomic_int counter{0};

    std::thread t([&loop]() -> void { loop.run(); });
    while (!loop.is_running()) {
        std::this_thread::yield();
    }

    loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); });
    loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); });
    loop.quit();
    t.join();

    REQUIRE(counter.load() == 2);
}

TEST_CASE("EventLoop quit processes remaining tasks before exiting", "[core][event_loop]")
{
    EventLoop loop;
    std::atomic_int counter{0};

    // Queue tasks then quit — run() must drain all of them before exiting
    loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); });
    loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); });
    loop.quit();
    loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); });

    loop.run();

    REQUIRE(counter.load() == 3);
}

TEST_CASE("EventLoop post_at in the past fires on next process_events", "[core][event_loop]")
{
    EventLoop loop;
    bool fired = false;
    loop.post_at(Clock::now() - 100ms, [&fired]() -> void { fired = true; });
    loop.process_events();
    REQUIRE(fired);
}

TEST_CASE("EventLoop post_delayed does not fire before deadline", "[core][event_loop]")
{
    EventLoop loop;
    bool fired = false;
    loop.post_delayed(50ms, [&fired]() -> void { fired = true; });
    loop.process_events();
    REQUIRE_FALSE(fired);
}

TEST_CASE("EventLoop post_delayed fires after deadline", "[core][event_loop]")
{
    EventLoop loop;
    bool fired = false;
    loop.post_delayed(10ms, [&fired]() -> void { fired = true; });

    std::this_thread::sleep_for(30ms);
    loop.process_events();
    REQUIRE(fired);
}

TEST_CASE("EventLoop post_at fires after deadline", "[core][event_loop]")
{
    EventLoop loop;
    bool fired = false;
    loop.post_at(Clock::now() + 10ms, [&fired]() -> void { fired = true; });

    std::this_thread::sleep_for(30ms);
    loop.process_events();
    REQUIRE(fired);
}

TEST_CASE("EventLoop cancel_timer prevents a one-shot timer from firing", "[core][event_loop]")
{
    EventLoop loop;
    bool fired = false;
    auto h = loop.post_delayed(10ms, [&fired]() -> void { fired = true; });

    loop.cancel_timer(h);

    std::this_thread::sleep_for(30ms);
    loop.process_events();
    REQUIRE_FALSE(fired);
}

TEST_CASE("EventLoop cancel_timer on already-expired timer is a no-op", "[core][event_loop]")
{
    EventLoop loop;
    int count = 0;
    auto h = loop.post_at(Clock::now() - 1ms, [&count]() -> void { ++count; });

    loop.process_events(); // fires it
    REQUIRE(count == 1);

    REQUIRE_NOTHROW(loop.cancel_timer(h)); // should not throw or crash
}

TEST_CASE("EventLoop post_repeating fires on each interval", "[core][event_loop]")
{
    EventLoop loop;
    int count = 0;
    loop.post_repeating(10ms, [&count]() -> void { ++count; });

    std::this_thread::sleep_for(15ms);
    loop.process_events();
    REQUIRE(count >= 1);

    std::this_thread::sleep_for(15ms);
    loop.process_events();
    REQUIRE(count >= 2);
}

TEST_CASE("EventLoop cancel_timer stops a repeating timer", "[core][event_loop]")
{
    EventLoop loop;
    int count = 0;
    auto h = loop.post_repeating(10ms, [&count]() -> void { ++count; });

    std::this_thread::sleep_for(25ms);
    loop.process_events();
    int count_after_first = count;
    REQUIRE(count_after_first >= 1);

    loop.cancel_timer(h);

    std::this_thread::sleep_for(25ms);
    loop.process_events();
    REQUIRE(count == count_after_first);
}

TEST_CASE("EventLoop post is thread-safe under concurrent posters", "[core][event_loop][thread]")
{
    EventLoop loop;
    constexpr int kTasksPerThread = 25;
    constexpr int kThreads        = 4;
    std::atomic_int counter{0};

    std::thread runner([&loop]() -> void { loop.run(); });
    while (!loop.is_running()) {
        std::this_thread::yield();
    }

    std::vector<std::thread> posters;
    posters.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        posters.emplace_back([&loop, &counter]() -> void {
            for (int j = 0; j < kTasksPerThread; ++j) {
                loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }
    for (auto& p : posters) {
        p.join();
    }

    loop.quit();
    runner.join();

    REQUIRE(counter.load() == kThreads * kTasksPerThread);
}
