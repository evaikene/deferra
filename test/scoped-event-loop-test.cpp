#include "scoped_event_loop.hpp"

#include "application.hpp"
#include "event_loop.hpp"
#include "object.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "thread_context.hpp"
#include "timer.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono> // IWYU pragma: keep for duration literals
#include <memory>
#include <thread>
#include <utility>

namespace jb::core::priv {

struct ScopedEventLoopTestAccess {
    static auto attach(std::unique_ptr<EventLoop> loop) -> Result<std::unique_ptr<ScopedEventLoop>, Error>
    {
        return ScopedEventLoop::attach(std::move(loop));
    }
};

} // namespace jb::core::priv

namespace {

using namespace jb::core;
using namespace jb::core::priv;
using namespace std::chrono_literals;

auto create_scope() -> std::unique_ptr<ScopedEventLoop>
{
    auto result = ScopedEventLoop::create();
    REQUIRE(result);
    return std::move(result).value();
}

TEST_CASE("Scoped loops repeatedly restore the host binding and object affinity", "[core][embedding]")
{
    REQUIRE(EventLoop::current() == nullptr);
    for (int iteration = 0; iteration < 3; ++iteration) {
        auto scope = create_scope();
        CHECK(Application::instance() == nullptr);
        CHECK(EventLoop::current() == &scope->loop());
        Object owner;
        Object child{&owner};
        CHECK(owner.event_loop() == &scope->loop());
        CHECK(child.event_loop() == &scope->loop());
        CHECK(child.thread_ctx() == ThreadCtx::current());
    }
    CHECK(EventLoop::current() == nullptr);

    Application app{0, nullptr};
    auto* const previous = EventLoop::current();
    {
        auto scope = create_scope();
        {
            auto nested = create_scope();
            CHECK(EventLoop::current() == &nested->loop());
        }
        CHECK(EventLoop::current() == &scope->loop());
        CHECK(Application::instance() == &app);
    }
    CHECK(EventLoop::current() == previous);
    CHECK(Application::instance() == &app);
    CHECK(app.process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);
}

TEST_CASE("Invalid scoped backend leaves the host binding intact", "[core][embedding]")
{
    auto host    = create_scope();
    auto invalid = EventLoopTestAccess::make_event_loop(nullptr);
    auto result  = ScopedEventLoopTestAccess::attach(std::move(invalid));
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::Unavailable);
    CHECK(result.error().code == "core.event_loop.unavailable");
    CHECK(EventLoop::current() == &host->loop());
}

TEST_CASE("Bounded waits avoid polling completed or expired operations", "[core][embedding]")
{
    auto fake  = make_fake_event_loop();
    auto owned = ScopedEventLoopTestAccess::attach(std::move(fake.loop));
    REQUIRE(owned);
    auto& loop = (*owned)->loop();

    CHECK(loop.run_until(Clock::now() - 1s, [] { return true; }));
    auto expired = loop.run_until(Clock::now() - 1s, [] { return false; });
    REQUIRE_FALSE(expired);
    CHECK(expired.error().category == ErrorCategory::Timeout);
    CHECK(expired.error().code == "core.event_loop.timeout");
    CHECK(fake.backend->poll_calls == 0);
    CHECK_FALSE(loop.is_running());

    bool complete{false};
    REQUIRE(loop.post([&] { complete = true; }));
    CHECK(loop.run_until(Clock::now() + 1s, [&] { return complete; }));
    CHECK(fake.backend->poll_calls == 1);
    CHECK(fake.backend->last_timeout_ms > 0);
    CHECK(fake.backend->last_timeout_ms <= 1000);
    CHECK_FALSE(loop.is_running());
}

TEST_CASE("Bounded waits report failures and remain reusable", "[core][embedding]")
{
    auto fake  = make_fake_event_loop();
    auto owned = ScopedEventLoopTestAccess::attach(std::move(fake.loop));
    REQUIRE(owned);
    auto& loop = (*owned)->loop();

    CHECK_FALSE(loop.run_until(Clock::now() + 1s, {}));
    fake.backend->poll_result = -1;
    auto failed               = loop.run_until(Clock::now() + 1s, [] { return false; });
    REQUIRE_FALSE(failed);
    CHECK(failed.error().category == ErrorCategory::Io);
    CHECK(failed.error().code == "core.event_loop.processing_failed");
    CHECK_FALSE(loop.is_running());

    fake.backend->poll_result.reset();
    REQUIRE(loop.quit());
    auto stopped = loop.run_until(Clock::now() + 1s, [] { return false; });
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == "core.event_loop.stopped");
    CHECK_FALSE(loop.is_running());
    CHECK(loop.run_until(Clock::now() + 1s, [] { return true; }));
}

TEST_CASE("Completing a bounded wait preserves queued work for the next operation", "[core][embedding]")
{
    auto scope = create_scope();
    bool first{false};
    bool second{false};
    REQUIRE(scope->loop().post([&] {
        first = true;
        REQUIRE(scope->loop().post([&] { second = true; }));
    }));

    auto const deadline = Clock::now() + 1s;
    REQUIRE(scope->loop().run_until(deadline, [&] { return first; }));
    CHECK_FALSE(second);
    REQUIRE(scope->loop().run_until(deadline, [&] { return second; }));
    CHECK(second);
}

TEST_CASE("Bounded waits require the current owner thread and binding", "[core][embedding]")
{
    auto outer = create_scope();
    {
        auto inner  = create_scope();
        auto result = outer->loop().run_until(Clock::now() + 1s, [] { return true; });
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "core.event_loop.invalid_context");
    }

    Result<void, Error> result;
    std::thread         worker{[&] { result = outer->loop().run_until(Clock::now() + 1s, [] { return true; }); }};
    worker.join();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "core.event_loop.invalid_context");
    CHECK(outer->loop().run_until(Clock::now() + 1s, [] { return true; }));
}

TEST_CASE("Every dispatch phase rejects synchronous waits and new loop scopes", "[core][embedding]")
{
    auto const phase = GENERATE(EventFlag::Tasks, EventFlag::Timers, EventFlag::Events, EventFlag::Watchers);
    auto       fake  = make_fake_event_loop();
    auto       owned = ScopedEventLoopTestAccess::attach(std::move(fake.loop));
    REQUIRE(owned);
    auto&     loop = (*owned)->loop();
    EventLoop other;
    Object    receiver;
    Timer     timer;
    bool      checked{false};

    auto check_reentrancy = [&] {
        for (auto* candidate : {&loop, &other}) {
            auto result = candidate->run_until(Clock::now() + 1s, [] { return true; });
            REQUIRE_FALSE(result);
            CHECK(result.error().code == "core.event_loop.reentrant_wait");
        }
        auto attached = ScopedEventLoop::create();
        REQUIRE_FALSE(attached);
        CHECK(attached.error().code == "core.event_loop.reentrant_wait");
        CHECK(EventLoop::current() == &loop);
        checked = true;
    };

    if (phase == EventFlag::Tasks) {
        REQUIRE(loop.post(check_reentrancy));
    }
    else if (phase == EventFlag::Timers) {
        REQUIRE(loop.post_at(Clock::now(), check_reentrancy));
    }
    else if (phase == EventFlag::Events) {
        timer.timeout.connect(&receiver, check_reentrancy, ConnectionType::Queued);
        timer.start(0ms);
    }
    else {
        REQUIRE(loop.watch_fd(17, FdEvent::Read, FdTriggerMode::Edge, [&](int, FdEvents) { check_reentrancy(); }));
        fake.backend->ready_events.push_back({.ident = 17, .events = FdEvent::Read});
    }

    // Exercise ordinary manual dispatch as well as the bounded entry point: both must mark callback context.
    REQUIRE(loop.process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);
    REQUIRE(checked);
    CHECK(loop.run_until(Clock::now() + 1s, [] { return true; }));
}

TEST_CASE("Completion predicates and final run drains cannot reenter a synchronous wait", "[core][embedding]")
{
    auto  scope = create_scope();
    auto& loop  = scope->loop();
    CHECK(loop.run_until(Clock::now() + 1s, [&] {
        auto nested = loop.run_until(Clock::now() + 1s, [] { return true; });
        REQUIRE_FALSE(nested);
        CHECK(nested.error().code == "core.event_loop.reentrant_wait");
        CHECK_FALSE(ScopedEventLoop::create());
        return true;
    }));

    bool checked{false};
    REQUIRE(loop.post([&] {
        REQUIRE(loop.post([&] {
            CHECK_FALSE(loop.run_until(Clock::now() + 1s, [] { return true; }));
            checked = true;
        }));
        loop.request_quit();
    }));
    REQUIRE(loop.run());
    CHECK(checked);
}

TEST_CASE("Native bounded waits expire without a polling thread", "[core][embedding]")
{
    auto scope  = create_scope();
    auto result = scope->loop().run_until(Clock::now() + 2ms, [] { return false; });
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "core.event_loop.timeout");
    CHECK_FALSE(scope->loop().is_running());
}

TEST_CASE("Scoped teardown cancels timers and drops stale receivers and unstarted work", "[core][embedding]")
{
    auto scope = create_scope();
    int  delivered{0};
    {
        auto  receiver = std::make_unique<Object>();
        Timer timer;
        timer.timeout.connect(receiver.get(), [&] { ++delivered; }, ConnectionType::Queued);
        timer.start(0ms);
        // Queue a receiver delivery, then destroy both its receiver and timer before processing that queue.
        REQUIRE(scope->loop().process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
        timer.start(1h);
    }
    CHECK(EventLoopTestAccess::active_timer_count(scope->loop()) == 0);

    int   deleted{0};
    auto* deferred = new Object;
    deferred->destroyed.connect([&] { ++deleted; });
    deferred->delete_later();
    REQUIRE(scope->loop().run_until(Clock::now() + 1s, [&] { return deleted == 1; }));
    CHECK(delivered == 0);

    bool ran{false};
    REQUIRE(scope->loop().post([&] { ran = true; }));
    scope.reset();
    CHECK_FALSE(ran);
    CHECK(EventLoop::current() == nullptr);
    auto fresh = create_scope();
    CHECK(fresh->loop().run_until(Clock::now() + 1s, [] { return true; }));
    CHECK_FALSE(ran);
}

} // namespace
