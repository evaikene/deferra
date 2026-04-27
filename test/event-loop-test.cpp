#include "event_loop.hpp"
#include "thread_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <unistd.h>
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
    loop.process_events(EventFlag::Tasks);
    REQUIRE(counter == 1);
}

TEST_CASE("EventLoop process_events executes multiple tasks in order", "[core][event_loop]")
{
    EventLoop loop;
    std::vector<int> order;
    loop.post([&order]() -> void { order.push_back(1); });
    loop.post([&order]() -> void { order.push_back(2); });
    loop.post([&order]() -> void { order.push_back(3); });
    loop.process_events(EventFlag::Tasks);
    REQUIRE(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("EventLoop process_events returns false when loop is not running", "[core][event_loop]")
{
    EventLoop loop;
    REQUIRE_FALSE(loop.process_events(EventFlag::Tasks));
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
    loop.process_events(EventFlag::Timers);
    REQUIRE(fired);
}

TEST_CASE("EventLoop post_delayed does not fire before deadline", "[core][event_loop]")
{
    EventLoop loop;
    bool fired = false;
    loop.post_delayed(50ms, [&fired]() -> void { fired = true; });
    loop.process_events(EventFlag::Timers);
    REQUIRE_FALSE(fired);
}

TEST_CASE("EventLoop post_delayed fires after deadline", "[core][event_loop]")
{
    EventLoop loop;
    bool fired = false;
    loop.post_delayed(10ms, [&fired]() -> void { fired = true; });

    std::this_thread::sleep_for(30ms);
    loop.process_events(EventFlag::Timers);
    REQUIRE(fired);
}

TEST_CASE("EventLoop post_at fires after deadline", "[core][event_loop]")
{
    EventLoop loop;
    bool fired = false;
    loop.post_at(Clock::now() + 10ms, [&fired]() -> void { fired = true; });

    std::this_thread::sleep_for(30ms);
    loop.process_events(EventFlag::Timers);
    REQUIRE(fired);
}

TEST_CASE("EventLoop cancel_timer prevents a one-shot timer from firing", "[core][event_loop]")
{
    EventLoop loop;
    bool fired = false;
    auto h = loop.post_delayed(10ms, [&fired]() -> void { fired = true; });

    loop.cancel_timer(h);

    std::this_thread::sleep_for(30ms);
    loop.process_events(EventFlag::Timers);
    REQUIRE_FALSE(fired);
}

TEST_CASE("EventLoop cancel_timer on already-expired timer is a no-op", "[core][event_loop]")
{
    EventLoop loop;
    int count = 0;
    auto h = loop.post_at(Clock::now() - 1ms, [&count]() -> void { ++count; });

    loop.process_events(EventFlag::Timers); // fires it
    REQUIRE(count == 1);

    REQUIRE_NOTHROW(loop.cancel_timer(h)); // should not throw or crash
}

TEST_CASE("EventLoop post_repeating fires on each interval", "[core][event_loop]")
{
    EventLoop loop;
    int count = 0;
    loop.post_repeating(10ms, [&count]() -> void { ++count; });

    std::this_thread::sleep_for(15ms);
    loop.process_events(EventFlag::Timers);
    REQUIRE(count >= 1);

    std::this_thread::sleep_for(15ms);
    loop.process_events(EventFlag::Timers);
    REQUIRE(count >= 2);
}

TEST_CASE("EventLoop cancel_timer stops a repeating timer", "[core][event_loop]")
{
    EventLoop loop;
    int count = 0;
    auto h = loop.post_repeating(10ms, [&count]() -> void { ++count; });

    std::this_thread::sleep_for(25ms);
    loop.process_events(EventFlag::Timers);
    int count_after_first = count;
    REQUIRE(count_after_first >= 1);

    loop.cancel_timer(h);

    std::this_thread::sleep_for(25ms);
    loop.process_events(EventFlag::Timers);
    REQUIRE(count == count_after_first);
}

TEST_CASE("EventLoop watch_fd returns a valid handle", "[core][event_loop]")
{
    EventLoop loop;
    int fds[2];
    REQUIRE(pipe(fds) == 0);

    auto h = loop.watch_fd(fds[0], FdEvents{FdEvent::Read}, [](int, FdEvents) -> void {});
    REQUIRE(h);
    REQUIRE(h.fd == fds[0]);

    loop.unwatch_fd(h);
    close(fds[0]);
    close(fds[1]);
}

TEST_CASE("EventLoop watch_fd callback fires when fd is readable", "[core][event_loop]")
{
    EventLoop loop;
    int fds[2];
    REQUIRE(pipe(fds) == 0);

    bool fired = false;
    loop.watch_fd(fds[0], FdEvents{FdEvent::Read}, [&fired](int, FdEvents) -> void {
        fired = true;
    });

    char byte = 'x';
    (void)write(fds[1], &byte, 1);
    loop.process_events(EventFlag::Watchers); // fd already readable — returns immediately

    close(fds[0]);
    close(fds[1]);
    REQUIRE(fired);
}

TEST_CASE("EventLoop watch_fd callback receives correct fd and events", "[core][event_loop]")
{
    EventLoop loop;
    int fds[2];
    REQUIRE(pipe(fds) == 0);

    int received_fd = -1;
    FdEvents received_events{};

    loop.watch_fd(fds[0], FdEvents{FdEvent::Read}, [&](int fd, FdEvents events) -> void {
        received_fd     = fd;
        received_events = events;
    });

    char byte = 'x';
    (void)write(fds[1], &byte, 1);
    loop.process_events(EventFlag::Watchers);

    close(fds[0]);
    close(fds[1]);
    REQUIRE(received_fd == fds[0]);
    REQUIRE(received_events.test(FdEvent::Read));
}

TEST_CASE("EventLoop unwatch_fd prevents callback from firing", "[core][event_loop]")
{
    EventLoop loop;
    int fds[2];
    REQUIRE(pipe(fds) == 0);

    int count = 0;
    auto h = loop.watch_fd(fds[0], FdEvents{FdEvent::Read}, [&count](int, FdEvents) -> void {
        ++count;
    });
    loop.unwatch_fd(h);

    char byte = 'x';
    (void)write(fds[1], &byte, 1);
    loop.process_events(EventFlag::Watchers, 0); // non-blocking: fd not watched

    close(fds[0]);
    close(fds[1]);
    REQUIRE(count == 0);
}

TEST_CASE("EventLoop unwatch_fd stops a repeating watch", "[core][event_loop]")
{
    EventLoop loop;
    int fds[2];
    REQUIRE(pipe(fds) == 0);

    int count = 0;
    auto h = loop.watch_fd(fds[0], FdEvents{FdEvent::Read}, [&count](int, FdEvents) -> void {
        ++count;
    });

    char byte = 'x';
    (void)write(fds[1], &byte, 1);
    loop.process_events(EventFlag::Watchers); // fires once
    REQUIRE(count == 1);

    char buf[1];
    (void)read(fds[0], buf, 1); // drain pipe so the fd is no longer readable

    loop.unwatch_fd(h);
    (void)write(fds[1], &byte, 1);
    loop.process_events(EventFlag::Watchers, 0); // non-blocking — must not fire

    close(fds[0]);
    close(fds[1]);
    REQUIRE(count == 1);
}

TEST_CASE("EventLoop watch_fd multiple fds fire independently", "[core][event_loop]")
{
    EventLoop loop;
    int fds0[2];
    int fds1[2];
    REQUIRE(pipe(fds0) == 0);
    REQUIRE(pipe(fds1) == 0);

    int count0 = 0;
    int count1 = 0;
    loop.watch_fd(fds0[0], FdEvents{FdEvent::Read}, [&count0](int, FdEvents) -> void { ++count0; });
    loop.watch_fd(fds1[0], FdEvents{FdEvent::Read}, [&count1](int, FdEvents) -> void { ++count1; });

    char byte = 'x';
    (void)write(fds0[1], &byte, 1);
    loop.process_events(EventFlag::Watchers);

    close(fds0[0]); close(fds0[1]);
    close(fds1[0]); close(fds1[1]);
    REQUIRE(count0 >= 1);
    REQUIRE(count1 == 0);
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
