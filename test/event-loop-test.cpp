#include "event_loop.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "thread_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace std::chrono_literals;

namespace {

struct FakeLoop {
    std::unique_ptr<EventLoop>            loop;
    jb::core::priv::FakeEventLoopBackend* backend;
};

auto make_fake_loop() -> FakeLoop
{
    auto  backend = std::make_unique<jb::core::priv::FakeEventLoopBackend>();
    auto* ptr     = backend.get();
    return {.loop = jb::core::priv::EventLoopTestAccess::make_event_loop(std::move(backend)), .backend = ptr};
}

class NonblockingPipe final {
public:

    NonblockingPipe()
    {
        int fds[2];
        if (::pipe(fds) != 0) {
            return;
        }
        _read_fd  = fds[0];
        _write_fd = fds[1];

        auto const flags = ::fcntl(_read_fd, F_GETFL);
        if (flags < 0 || ::fcntl(_read_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            close_descriptors();
        }
    }

    ~NonblockingPipe() { close_descriptors(); }

    NonblockingPipe(NonblockingPipe const&)                    = delete;
    auto operator=(NonblockingPipe const&) -> NonblockingPipe& = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return _read_fd >= 0 && _write_fd >= 0; }

    [[nodiscard]] auto read_fd() const noexcept -> int { return _read_fd; }

    [[nodiscard]] auto write_byte() const noexcept -> bool
    {
        for (;;) {
            char const byte{'x'};
            auto const count = ::write(_write_fd, &byte, 1);
            if (count == 1) {
                return true;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
    }

    [[nodiscard]] auto drain_read_end() const noexcept -> bool
    {
        for (;;) {
            char       buffer[64];
            auto const count = ::read(_read_fd, buffer, sizeof(buffer));
            if (count > 0) {
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        }
    }

private:

    int _read_fd{-1};
    int _write_fd{-1};

    void close_descriptors() noexcept
    {
        if (_read_fd >= 0) {
            static_cast<void>(::close(_read_fd));
            _read_fd = -1;
        }
        if (_write_fd >= 0) {
            static_cast<void>(::close(_write_fd));
            _write_fd = -1;
        }
    }
};

} // anonymous namespace

TEST_CASE("EventLoop initial state", "[core][event_loop]")
{
    EventLoop loop;
    CHECK(loop.is_valid());
    CHECK_FALSE(loop.is_running());
    CHECK(loop.thread_ctx() == ThreadCtx::current());
}

TEST_CASE("EventLoop reports invalid backend initialization", "[core][event_loop]")
{
    auto loop = jb::core::priv::EventLoopTestAccess::make_event_loop(nullptr);

    CHECK_FALSE(loop->is_valid());
    CHECK(loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Failed);
    CHECK_FALSE(loop->post([]() -> void {}));
    CHECK_FALSE(loop->quit());
    CHECK_FALSE(loop->run());
    CHECK_FALSE(loop->is_running());
}

TEST_CASE("EventLoop manual and running processing report distinct states", "[core][event_loop]")
{
    auto fake = make_fake_loop();

    CHECK(fake.loop->process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);

    auto nested_result = ProcessEventsResult::Failed;
    bool quit_queued{false};
    REQUIRE(fake.loop->post([&]() -> void {
        nested_result = fake.loop->process_events(EventFlag::Timers);
        quit_queued   = fake.loop->quit();
    }));

    CHECK(fake.loop->run());
    CHECK(nested_result == ProcessEventsResult::Running);
    CHECK(quit_queued);
}

TEST_CASE("EventLoop does not queue a task when wakeup fails", "[core][event_loop]")
{
    auto fake    = make_fake_loop();
    int  counter = 0;

    fake.backend->wakeup_result = false;
    auto increment_counter      = [&counter]() -> void { ++counter; };
    CHECK_FALSE(fake.loop->post(std::move(increment_counter)));

    fake.backend->wakeup_result = true;
    CHECK(fake.loop->process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);
    CHECK(counter == 0);
}

TEST_CASE("EventLoop validates watches before backend registration", "[core][event_loop]")
{
    auto fake = make_fake_loop();

    CHECK_FALSE(fake.loop->watch_fd(-1, FdEvent::Read, FdTriggerMode::Edge, [](int, FdEvents) -> void {}));
    CHECK_FALSE(fake.loop->watch_fd(10, {}, FdTriggerMode::Edge, [](int, FdEvents) -> void {}));
    CHECK_FALSE(fake.loop->watch_fd(10, FdEvent::Read, FdTriggerMode::Edge, {}));
    CHECK(fake.backend->add_fd_calls == 0);

    fake.backend->add_fd_result = false;
    CHECK_FALSE(fake.loop->watch_fd(10, FdEvent::Read, FdTriggerMode::Level, [](int, FdEvents) -> void {}));
    CHECK(fake.backend->add_fd_calls == 1);
    CHECK(fake.backend->last_added_fd == 10);
    CHECK(fake.backend->last_added_events.bits() == FdEvents{FdEvent::Read}.bits());
    CHECK(fake.backend->last_added_trigger_mode == FdTriggerMode::Level);
}

TEST_CASE("EventLoop propagates fd trigger modes to the backend", "[core][event_loop]")
{
    auto fake = make_fake_loop();

    auto const edge  = fake.loop->watch_fd(40, FdEvent::Read, FdTriggerMode::Edge, [](int, FdEvents) -> void {});
    auto const level = fake.loop->watch_fd(41, FdEvent::Write, FdTriggerMode::Level, [](int, FdEvents) -> void {});

    REQUIRE(edge);
    REQUIRE(level);
    REQUIRE(fake.backend->add_fd_history.size() == 2);
    CHECK(fake.backend->add_fd_history[0].trigger_mode == FdTriggerMode::Edge);
    CHECK(fake.backend->add_fd_history[1].trigger_mode == FdTriggerMode::Level);
}

TEST_CASE("EventLoop callback-only replacement does not update the backend", "[core][event_loop]")
{
    auto fake = make_fake_loop();

    for (auto const trigger_mode : {FdTriggerMode::Edge, FdTriggerMode::Level}) {
        auto const fd = trigger_mode == FdTriggerMode::Edge ? 42 : 43;
        int        original_calls{0};
        int        replacement_calls{0};

        auto const original =
            fake.loop->watch_fd(fd, FdEvent::Read, trigger_mode, [&original_calls](int, FdEvents) -> void {
                ++original_calls;
            });
        REQUIRE(original);
        auto const add_calls = fake.backend->add_fd_calls;

        auto const replacement =
            fake.loop->watch_fd(fd, FdEvent::Read, trigger_mode, [&replacement_calls](int, FdEvents) -> void {
                ++replacement_calls;
            });
        REQUIRE(replacement);
        CHECK(replacement.fd == original.fd);
        CHECK(fake.backend->add_fd_calls == add_calls);

        fake.backend->ready_events.push_back({.fd = fd, .events = FdEvent::Read});
        CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
        CHECK(original_calls == 0);
        CHECK(replacement_calls == 1);
    }
}

TEST_CASE("EventLoop trigger-mode replacement updates the backend", "[core][event_loop]")
{
    auto fake = make_fake_loop();

    REQUIRE(fake.loop->watch_fd(42, FdEvent::Read, FdTriggerMode::Edge, [](int, FdEvents) -> void {}));
    REQUIRE(fake.loop->watch_fd(42, FdEvent::Read, FdTriggerMode::Level, [](int, FdEvents) -> void {}));
    REQUIRE(fake.loop->watch_fd(42, FdEvent::Read, FdTriggerMode::Edge, [](int, FdEvents) -> void {}));

    REQUIRE(fake.backend->add_fd_history.size() == 3);
    CHECK(fake.backend->add_fd_history[0].trigger_mode == FdTriggerMode::Edge);
    CHECK(fake.backend->add_fd_history[1].trigger_mode == FdTriggerMode::Level);
    CHECK(fake.backend->add_fd_history[2].trigger_mode == FdTriggerMode::Edge);
}

TEST_CASE("EventLoop event-mask replacement updates the backend", "[core][event_loop]")
{
    auto fake = make_fake_loop();

    REQUIRE(fake.loop->watch_fd(42, FdEvent::Read, FdTriggerMode::Edge, [](int, FdEvents) -> void {}));
    REQUIRE(fake.loop->watch_fd(42, FdEvent::Write, FdTriggerMode::Edge, [](int, FdEvents) -> void {}));

    REQUIRE(fake.backend->add_fd_history.size() == 2);
    CHECK(fake.backend->add_fd_history[0].events.bits() == FdEvents{FdEvent::Read}.bits());
    CHECK(fake.backend->add_fd_history[1].events.bits() == FdEvents{FdEvent::Write}.bits());
}

TEST_CASE("EventLoop preserves an existing watch when event and trigger replacement fails", "[core][event_loop]")
{
    auto fake = make_fake_loop();
    int  original_calls{0};
    int  replacement_calls{0};

    auto const original =
        fake.loop->watch_fd(42, FdEvent::Read, FdTriggerMode::Edge, [&original_calls](int, FdEvents) -> void {
            ++original_calls;
        });
    REQUIRE(original);

    fake.backend->add_fd_result = false;
    auto const replacement =
        fake.loop->watch_fd(42, FdEvent::Write, FdTriggerMode::Level, [&replacement_calls](int, FdEvents) -> void {
            ++replacement_calls;
        });
    CHECK_FALSE(replacement);

    auto const registration = jb::core::priv::EventLoopTestAccess::fd_registration(*fake.loop, 42);
    REQUIRE(registration);
    CHECK(registration->events.bits() == FdEvents{FdEvent::Read}.bits());
    CHECK(registration->trigger_mode == FdTriggerMode::Edge);

    fake.backend->ready_events.push_back({.fd = 42, .events = FdEvent::Read});
    CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(original_calls == 1);
    CHECK(replacement_calls == 0);
}

TEST_CASE("EventLoop preserves an existing watch when trigger-only replacement fails", "[core][event_loop]")
{
    auto fake = make_fake_loop();
    int  original_calls{0};
    int  replacement_calls{0};

    auto const original =
        fake.loop->watch_fd(42, FdEvent::Read, FdTriggerMode::Edge, [&original_calls](int, FdEvents) -> void {
            ++original_calls;
        });
    REQUIRE(original);

    fake.backend->add_fd_result = false;
    auto const replacement =
        fake.loop->watch_fd(42, FdEvent::Read, FdTriggerMode::Level, [&replacement_calls](int, FdEvents) -> void {
            ++replacement_calls;
        });
    CHECK_FALSE(replacement);

    auto const registration = jb::core::priv::EventLoopTestAccess::fd_registration(*fake.loop, 42);
    REQUIRE(registration);
    CHECK(registration->events.bits() == FdEvents{FdEvent::Read}.bits());
    CHECK(registration->trigger_mode == FdTriggerMode::Edge);

    fake.backend->ready_events.push_back({.fd = 42, .events = FdEvent::Read});
    CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(original_calls == 1);
    CHECK(replacement_calls == 0);
}

TEST_CASE("EventLoop replaces fd events and trigger mode together", "[core][event_loop]")
{
    auto fake = make_fake_loop();

    REQUIRE(fake.loop->watch_fd(42, FdEvent::Read, FdTriggerMode::Edge, [](int, FdEvents) -> void {}));
    REQUIRE(fake.loop->watch_fd(42, FdEvent::Write, FdTriggerMode::Level, [](int, FdEvents) -> void {}));

    auto const registration = jb::core::priv::EventLoopTestAccess::fd_registration(*fake.loop, 42);
    REQUIRE(registration);
    CHECK(registration->events.bits() == FdEvents{FdEvent::Write}.bits());
    CHECK(registration->trigger_mode == FdTriggerMode::Level);
    CHECK(fake.backend->add_fd_calls == 2);
}

TEST_CASE("EventLoop retains a watch after failed removal", "[core][event_loop]")
{
    auto fake = make_fake_loop();
    int  callback_calls{0};

    auto const watch =
        fake.loop->watch_fd(42, FdEvent::Read, FdTriggerMode::Edge, [&callback_calls](int, FdEvents) -> void {
            ++callback_calls;
        });
    REQUIRE(watch);

    fake.backend->remove_fd_result = false;
    CHECK_FALSE(fake.loop->unwatch_fd(watch));
    fake.backend->ready_events.push_back({.fd = 42, .events = FdEvent::Read});
    CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_calls == 1);

    fake.backend->remove_fd_result = true;
    CHECK(fake.loop->unwatch_fd(watch));
    CHECK(fake.loop->unwatch_fd(watch));
    fake.backend->ready_events.push_back({.fd = 42, .events = FdEvent::Read});
    CHECK(fake.loop->process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_calls == 1);
}

TEST_CASE("EventLoop reports backend poll failures", "[core][event_loop]")
{
    auto fake = make_fake_loop();
    int  task_calls{0};
    auto increment_task_calls = [&task_calls]() -> void { ++task_calls; };
    REQUIRE(fake.loop->post(std::move(increment_task_calls)));
    fake.backend->poll_result = -1;

    CHECK(fake.loop->process_events(EventFlag::All, 0) == ProcessEventsResult::Failed);
    CHECK(task_calls == 0);

    auto failed_run                 = make_fake_loop();
    failed_run.backend->poll_result = -1;
    CHECK_FALSE(failed_run.loop->run());
    CHECK_FALSE(failed_run.loop->is_running());
}

TEST_CASE("EventLoop process_events executes a posted task", "[core][event_loop]")
{
    EventLoop loop;
    int       counter           = 0;
    auto      increment_counter = [&counter]() -> void { ++counter; };
    REQUIRE(loop.post(std::move(increment_counter)));
    CHECK(loop.process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);
    CHECK(counter == 1);
}

TEST_CASE("EventLoop process_events executes multiple tasks in order", "[core][event_loop]")
{
    EventLoop        loop;
    std::vector<int> order;
    REQUIRE(loop.post([&order]() -> void { order.push_back(1); }));
    REQUIRE(loop.post([&order]() -> void { order.push_back(2); }));
    REQUIRE(loop.post([&order]() -> void { order.push_back(3); }));
    CHECK(loop.process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);
    CHECK(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("EventLoop process_events reports stopped when loop is not running", "[core][event_loop]")
{
    EventLoop loop;
    CHECK(loop.process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);
}

TEST_CASE("EventLoop run and quit", "[core][event_loop][thread]")
{
    EventLoop loop;
    CHECK_FALSE(loop.is_running());

    bool        run_succeeded{false};
    std::thread t([&loop, &run_succeeded]() -> void { run_succeeded = loop.run(); });
    while (!loop.is_running()) {
        std::this_thread::yield();
    }

    CHECK(loop.is_running());
    CHECK(loop.quit());
    t.join();

    CHECK(run_succeeded);
    CHECK_FALSE(loop.is_running());
}

TEST_CASE("EventLoop run executes tasks posted from another thread", "[core][event_loop][thread]")
{
    EventLoop       loop;
    std::atomic_int counter{0};
    bool            run_succeeded{false};

    std::thread t([&loop, &run_succeeded]() -> void { run_succeeded = loop.run(); });
    while (!loop.is_running()) {
        std::this_thread::yield();
    }

    REQUIRE(loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); }));
    REQUIRE(loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); }));
    REQUIRE(loop.quit());
    t.join();

    CHECK(run_succeeded);
    CHECK(counter.load() == 2);
}

TEST_CASE("EventLoop quit processes remaining tasks before exiting", "[core][event_loop]")
{
    EventLoop       loop;
    std::atomic_int counter{0};

    // Queue tasks then quit — run() must drain all of them before exiting
    REQUIRE(loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); }));
    REQUIRE(loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); }));
    REQUIRE(loop.quit());
    REQUIRE(loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); }));

    CHECK(loop.run());

    CHECK(counter.load() == 3);
}

TEST_CASE("EventLoop post_at in the past fires on next process_events", "[core][event_loop]")
{
    EventLoop loop;
    bool      fired = false;
    REQUIRE(loop.post_at(Clock::now() - 100ms, [&fired]() -> void { fired = true; }));
    CHECK(loop.process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK(fired);
}

TEST_CASE("EventLoop post_delayed does not fire before deadline", "[core][event_loop]")
{
    EventLoop loop;
    bool      fired = false;
    REQUIRE(loop.post_delayed(50ms, [&fired]() -> void { fired = true; }));
    CHECK(loop.process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK_FALSE(fired);
}

TEST_CASE("EventLoop post_delayed fires after deadline", "[core][event_loop]")
{
    EventLoop loop;
    bool      fired = false;
    REQUIRE(loop.post_delayed(10ms, [&fired]() -> void { fired = true; }));

    std::this_thread::sleep_for(30ms);
    CHECK(loop.process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK(fired);
}

TEST_CASE("EventLoop post_at fires after deadline", "[core][event_loop]")
{
    EventLoop loop;
    bool      fired = false;
    REQUIRE(loop.post_at(Clock::now() + 10ms, [&fired]() -> void { fired = true; }));

    std::this_thread::sleep_for(30ms);
    CHECK(loop.process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK(fired);
}

TEST_CASE("EventLoop cancel_timer prevents a one-shot timer from firing", "[core][event_loop]")
{
    EventLoop loop;
    bool      fired = false;
    auto      h     = loop.post_delayed(10ms, [&fired]() -> void { fired = true; });
    REQUIRE(h);

    loop.cancel_timer(h);

    std::this_thread::sleep_for(30ms);
    CHECK(loop.process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK_FALSE(fired);
}

TEST_CASE("EventLoop cancel_timer on already-expired timer is a no-op", "[core][event_loop]")
{
    EventLoop loop;
    int       count = 0;
    auto      h     = loop.post_at(Clock::now() - 1ms, [&count]() -> void { ++count; });
    REQUIRE(h);

    CHECK(loop.process_events(EventFlag::Timers) == ProcessEventsResult::Stopped); // fires it
    CHECK(count == 1);

    CHECK_NOTHROW(loop.cancel_timer(h)); // should not throw or crash
}

TEST_CASE("EventLoop post_repeating fires on each interval", "[core][event_loop]")
{
    EventLoop loop;
    int       count           = 0;
    auto      increment_count = [&count]() -> void { ++count; };
    REQUIRE(loop.post_repeating(10ms, std::move(increment_count)));

    std::this_thread::sleep_for(15ms);
    CHECK(loop.process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK(count >= 1);

    std::this_thread::sleep_for(15ms);
    CHECK(loop.process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK(count >= 2);
}

TEST_CASE("EventLoop cancel_timer stops a repeating timer", "[core][event_loop]")
{
    EventLoop loop;
    int       count = 0;
    auto      h     = loop.post_repeating(10ms, [&count]() -> void { ++count; });
    REQUIRE(h);

    std::this_thread::sleep_for(25ms);
    CHECK(loop.process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    int count_after_first = count;
    CHECK(count_after_first >= 1);

    loop.cancel_timer(h);

    std::this_thread::sleep_for(25ms);
    CHECK(loop.process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
    CHECK(count == count_after_first);
}

TEST_CASE("EventLoop watch_fd returns a valid handle", "[core][event_loop]")
{
    EventLoop loop;
    int       fds[2];
    CHECK(pipe(fds) == 0);

    auto h = loop.watch_fd(fds[0], FdEvents{FdEvent::Read}, FdTriggerMode::Edge, [](int, FdEvents) -> void {});
    CHECK(h);
    CHECK(h.fd == fds[0]);

    CHECK(loop.unwatch_fd(h));
    close(fds[0]);
    close(fds[1]);
}

TEST_CASE("EventLoop watch_fd callback fires when fd is readable", "[core][event_loop]")
{
    EventLoop loop;
    int       fds[2];
    CHECK(pipe(fds) == 0);

    bool       fired = false;
    auto const watch =
        loop.watch_fd(fds[0], FdEvents{FdEvent::Read}, FdTriggerMode::Edge, [&fired](int, FdEvents) -> void {
            fired = true;
        });
    REQUIRE(watch);

    char byte = 'x';
    (void)write(fds[1], &byte, 1);
    CHECK(loop.process_events(EventFlag::Watchers) == ProcessEventsResult::Stopped);
    CHECK(loop.unwatch_fd(watch));

    close(fds[0]);
    close(fds[1]);
    CHECK(fired);
}

TEST_CASE("EventLoop watch_fd callback receives correct fd and events", "[core][event_loop]")
{
    EventLoop loop;
    int       fds[2];
    CHECK(pipe(fds) == 0);

    int      received_fd = -1;
    FdEvents received_events{};

    auto const watch =
        loop.watch_fd(fds[0], FdEvents{FdEvent::Read}, FdTriggerMode::Edge, [&](int fd, FdEvents events) -> void {
            received_fd     = fd;
            received_events = events;
        });
    REQUIRE(watch);

    char byte = 'x';
    (void)write(fds[1], &byte, 1);
    CHECK(loop.process_events(EventFlag::Watchers) == ProcessEventsResult::Stopped);
    CHECK(loop.unwatch_fd(watch));

    close(fds[0]);
    close(fds[1]);
    CHECK(received_fd == fds[0]);
    CHECK(received_events.test(FdEvent::Read));
}

TEST_CASE("EventLoop unwatch_fd prevents callback from firing", "[core][event_loop]")
{
    EventLoop loop;
    int       fds[2];
    CHECK(pipe(fds) == 0);

    int  count = 0;
    auto h     = loop.watch_fd(fds[0], FdEvents{FdEvent::Read}, FdTriggerMode::Edge, [&count](int, FdEvents) -> void {
        ++count;
    });
    REQUIRE(h);
    CHECK(loop.unwatch_fd(h));

    char byte = 'x';
    (void)write(fds[1], &byte, 1);
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);

    close(fds[0]);
    close(fds[1]);
    CHECK(count == 0);
}

TEST_CASE("EventLoop unwatch_fd stops a repeating watch", "[core][event_loop]")
{
    EventLoop loop;
    int       fds[2];
    CHECK(pipe(fds) == 0);

    int  count = 0;
    auto h     = loop.watch_fd(fds[0], FdEvents{FdEvent::Read}, FdTriggerMode::Edge, [&count](int, FdEvents) -> void {
        ++count;
    });
    REQUIRE(h);

    char byte = 'x';
    (void)write(fds[1], &byte, 1);
    CHECK(loop.process_events(EventFlag::Watchers) == ProcessEventsResult::Stopped);
    CHECK(count == 1);

    char buf[1];
    (void)read(fds[0], buf, 1); // drain pipe so the fd is no longer readable

    CHECK(loop.unwatch_fd(h));
    (void)write(fds[1], &byte, 1);
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);

    close(fds[0]);
    close(fds[1]);
    CHECK(count == 1);
}

TEST_CASE("EventLoop watch_fd multiple fds fire independently", "[core][event_loop]")
{
    EventLoop loop;
    int       fds0[2];
    int       fds1[2];
    CHECK(pipe(fds0) == 0);
    CHECK(pipe(fds1) == 0);

    int        count0 = 0;
    int        count1 = 0;
    auto const watch0 =
        loop.watch_fd(fds0[0], FdEvents{FdEvent::Read}, FdTriggerMode::Edge, [&count0](int, FdEvents) -> void {
            ++count0;
        });
    auto const watch1 =
        loop.watch_fd(fds1[0], FdEvents{FdEvent::Read}, FdTriggerMode::Edge, [&count1](int, FdEvents) -> void {
            ++count1;
        });
    REQUIRE(watch0);
    REQUIRE(watch1);

    char byte = 'x';
    (void)write(fds0[1], &byte, 1);
    CHECK(loop.process_events(EventFlag::Watchers) == ProcessEventsResult::Stopped);
    CHECK(loop.unwatch_fd(watch0));
    CHECK(loop.unwatch_fd(watch1));

    close(fds0[0]);
    close(fds0[1]);
    close(fds1[0]);
    close(fds1[1]);
    CHECK(count0 >= 1);
    CHECK(count1 == 0);
}

TEST_CASE("EventLoop edge watch repeats only after a new readiness transition", "[core][event_loop]")
{
    EventLoop       loop;
    NonblockingPipe descriptor;
    int             callback_count{0};
    REQUIRE(descriptor);

    auto const watch = loop.watch_fd(descriptor.read_fd(),
                                     FdEvent::Read,
                                     FdTriggerMode::Edge,
                                     [&callback_count](int, FdEvents) -> void { ++callback_count; });
    REQUIRE(watch);

    REQUIRE(descriptor.write_byte());
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_count == 1);
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_count == 1);

    REQUIRE(descriptor.drain_read_end());
    REQUIRE(descriptor.write_byte());
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_count == 2);
    CHECK(loop.unwatch_fd(watch));
}

TEST_CASE("EventLoop level watch repeats while the descriptor remains ready", "[core][event_loop]")
{
    EventLoop       loop;
    NonblockingPipe descriptor;
    int             callback_count{0};
    REQUIRE(descriptor);

    auto const watch = loop.watch_fd(descriptor.read_fd(),
                                     FdEvent::Read,
                                     FdTriggerMode::Level,
                                     [&callback_count](int, FdEvents) -> void { ++callback_count; });
    REQUIRE(watch);

    REQUIRE(descriptor.write_byte());
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_count == 1);
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_count == 2);

    REQUIRE(descriptor.drain_read_end());
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_count == 2);
    CHECK(loop.unwatch_fd(watch));
}

TEST_CASE("EventLoop changes native fd trigger mode at runtime", "[core][event_loop]")
{
    EventLoop       loop;
    NonblockingPipe descriptor;
    int             callback_count{0};
    REQUIRE(descriptor);

    auto callback = [&callback_count](int, FdEvents) -> void { ++callback_count; };
    auto watch    = loop.watch_fd(descriptor.read_fd(), FdEvent::Read, FdTriggerMode::Level, callback);
    REQUIRE(watch);

    REQUIRE(descriptor.write_byte());
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_count == 2);
    REQUIRE(descriptor.drain_read_end());
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_count == 2);

    watch = loop.watch_fd(descriptor.read_fd(), FdEvent::Read, FdTriggerMode::Edge, callback);
    REQUIRE(watch);
    REQUIRE(descriptor.write_byte());
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_count == 3);
    REQUIRE(descriptor.drain_read_end());
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_count == 3);

    watch = loop.watch_fd(descriptor.read_fd(), FdEvent::Read, FdTriggerMode::Level, callback);
    REQUIRE(watch);
    REQUIRE(descriptor.write_byte());
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(loop.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
    CHECK(callback_count == 5);
    REQUIRE(descriptor.drain_read_end());
    CHECK(loop.unwatch_fd(watch));
}

TEST_CASE("EventLoop post is thread-safe under concurrent posters", "[core][event_loop][thread]")
{
    EventLoop        loop;
    constexpr int    kTasksPerThread = 25;
    constexpr int    kThreads        = 4;
    std::atomic_int  counter{0};
    std::atomic_bool all_posts_succeeded{true};
    bool             run_succeeded{false};

    std::thread runner([&loop, &run_succeeded]() -> void { run_succeeded = loop.run(); });
    while (!loop.is_running()) {
        std::this_thread::yield();
    }

    std::vector<std::thread> posters;
    posters.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        posters.emplace_back([&loop, &counter, &all_posts_succeeded]() -> void {
            for (int j = 0; j < kTasksPerThread; ++j) {
                if (!loop.post([&counter]() -> void { counter.fetch_add(1, std::memory_order_relaxed); })) {
                    all_posts_succeeded.store(false, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& p : posters) {
        p.join();
    }

    CHECK(loop.quit());
    runner.join();

    CHECK(run_succeeded);
    CHECK(all_posts_succeeded.load(std::memory_order_relaxed));
    CHECK(counter.load() == kThreads * kTasksPerThread);
}
