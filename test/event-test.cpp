#include "event.hpp"

#include "application.hpp"
#include "event_thread.hpp"
#include "logging.hpp"
#include "object.hpp"
#include "thread_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace jb::core;

namespace {

class CustomEvent : public Event {
public:

    static constexpr Type TypeId{Event::User};

    CustomEvent()
        : Event(TypeId)
    {}
};

class TrackedEvent : public CustomEvent {
public:

    explicit TrackedEvent(int& destruction_count)
        : _destruction_count(destruction_count)
    {}

    ~TrackedEvent() override { ++_destruction_count; }

private:

    int& _destruction_count;
};

class EventReceiver : public Object {
public:

    auto event(Event& event) -> bool override
    {
        if (event.type() != CustomEvent::TypeId) {
            return Object::event(event);
        }

        received = true;
        event.accept();
        return true;
    }

    bool received{false};
};

class PostingEventReceiver : public EventReceiver {
public:

    auto event(Event& event) -> bool override
    {
        auto const handled = EventReceiver::event(event);
        if (handled && !posted_follow_up) {
            posted_follow_up = true;
            Application::post_event(this, std::make_unique<CustomEvent>());
        }
        return handled;
    }

    bool posted_follow_up{false};
};

class CaptureLogger : public Logger {
public:

    void log(LogMessage const& message) override
    {
        levels.push_back(message.level);
        messages.emplace_back(message.message);
    }

    std::vector<LogLevel>    levels;
    std::vector<std::string> messages;
};

class CountingEventReceiver : public EventReceiver {
public:

    CountingEventReceiver(int& delivery_count, int& destruction_count)
        : _delivery_count(delivery_count)
        , _destruction_count(destruction_count)
    {}

    ~CountingEventReceiver() override { ++_destruction_count; }

    auto event(Event& event) -> bool override
    {
        auto const handled = EventReceiver::event(event);
        if (handled) {
            ++_delivery_count;
        }
        return handled;
    }

private:

    int& _delivery_count;
    int& _destruction_count;
};

class ThreadRecordingReceiver : public EventReceiver {
public:

    auto event(Event& event) -> bool override
    {
        event_thread.store(ThreadCtx::current(), std::memory_order_relaxed);
        auto const handled = EventReceiver::event(event);
        delivered.store(true, std::memory_order_release);
        return handled;
    }

    std::atomic_bool              delivered{false};
    std::atomic<ThreadCtx const*> event_thread{nullptr};
};

} // anonymous namespace

TEST_CASE("Event defaults to ignored and retains its type", "[core][event]")
{
    Event event{Event::CoreBase};

    CHECK(event.type() == Event::CoreBase);
    CHECK_FALSE(event.accepted());
}

TEST_CASE("Event can be accepted and ignored", "[core][event]")
{
    Event event{Event::User};

    event.accept();
    CHECK(event.accepted());

    event.ignore();
    CHECK_FALSE(event.accepted());
}

TEST_CASE("Object does not recognize events by default", "[core][event]")
{
    Object object;
    Event  event{Event::None};

    CHECK_FALSE(object.event(event));
    CHECK_FALSE(event.accepted());
}

TEST_CASE("Application sends an event directly to its receiver", "[core][event]")
{
    EventReceiver receiver;
    CustomEvent   event;

    CHECK(Application::send_event(&receiver, event));
    CHECK(receiver.received);
    CHECK(event.accepted());
}

TEST_CASE("Application send_event returns false for a null receiver", "[core][event]")
{
    Event event{Event::None};

    CHECK_FALSE(Application::send_event(nullptr, event));
    CHECK_FALSE(event.accepted());
}

TEST_CASE("Application post_event delivers only during event processing", "[core][event]")
{
    Application   app{0, nullptr};
    EventReceiver receiver;

    Application::post_event(&receiver, std::make_unique<CustomEvent>());

    app.process_events(EventFlag::Tasks);
    CHECK_FALSE(receiver.received);

    app.process_events(EventFlag::Events);
    CHECK(receiver.received);
}

TEST_CASE("Application post_event is thread-safe", "[core][event][thread]")
{
    Application   app{0, nullptr};
    EventReceiver receiver;

    std::thread posting_thread{
        [&receiver]() -> void { Application::post_event(&receiver, std::make_unique<CustomEvent>()); }};
    posting_thread.join();

    app.process_events(EventFlag::Events);
    CHECK(receiver.received);
}

TEST_CASE("Application post_event uses a moved receiver's current event loop", "[core][event][thread]")
{
    Application app{0, nullptr};
    EventThread thread;
    thread.exec(true);

    auto* receiver = new ThreadRecordingReceiver;
    CHECK(receiver->move_to_thread(&thread));

    Application::post_event(receiver, std::make_unique<CustomEvent>());

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (!receiver->delivered.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }

    CHECK(receiver->delivered.load(std::memory_order_acquire));
    CHECK(receiver->event_thread.load(std::memory_order_relaxed) == thread.as_event_loop()->thread_ctx());

    receiver->delete_later();
    thread.quit();
    thread.wait();
}

TEST_CASE("Application post_event ignores null inputs", "[core][event]")
{
    Application   app{0, nullptr};
    EventReceiver receiver;

    Application::post_event(nullptr, std::make_unique<CustomEvent>());
    Application::post_event(&receiver, nullptr);

    app.process_events(EventFlag::Events);
    CHECK_FALSE(receiver.received);
}

TEST_CASE("Application post_event logs and drops events for receivers without an event loop", "[core][event]")
{
    auto capture = std::make_shared<CaptureLogger>();
    capture->set_level(LogLevel::Error);
    set_logger(capture);

    {
        EventReceiver receiver;
        Application::post_event(&receiver, std::make_unique<CustomEvent>());
        CHECK_FALSE(receiver.received);
    }

    set_logger(nullptr);

    REQUIRE(capture->levels.size() == 1);
    CHECK(capture->levels.front() == LogLevel::Error);
    CHECK(capture->messages.front() == "Application::post_event: receiver must have an event loop");
}

TEST_CASE("Application post_event drops events for destroyed receivers", "[core][event]")
{
    Application app{0, nullptr};
    auto*       receiver = new EventReceiver;

    Application::post_event(receiver, std::make_unique<CustomEvent>());
    delete receiver;

    CHECK_NOTHROW(app.process_events(EventFlag::Events));
}

TEST_CASE("Events posted during dispatch wait for the next event phase", "[core][event]")
{
    Application          app{0, nullptr};
    PostingEventReceiver receiver;

    Application::post_event(&receiver, std::make_unique<CustomEvent>());

    receiver.received = false;
    app.process_events(EventFlag::Events);
    CHECK(receiver.received);

    receiver.received = false;
    app.process_events(EventFlag::Events);
    CHECK(receiver.received);

    receiver.received = false;
    app.process_events(EventFlag::Events);
    CHECK_FALSE(receiver.received);
}

TEST_CASE("EventFlag All includes object events", "[core][event]")
{
    Application   app{0, nullptr};
    EventReceiver receiver;

    STATIC_CHECK(EventFlags{EventFlag::All}.test(EventFlag::Events));

    Application::post_event(&receiver, std::make_unique<CustomEvent>());
    app.process_events(EventFlag::All, 0);

    CHECK(receiver.received);
}

TEST_CASE("Deferred deletion runs after tasks and object events", "[core][event]")
{
    Application app{0, nullptr};
    int         delivery_count    = 0;
    int         destruction_count = 0;
    auto*       receiver          = new CountingEventReceiver{delivery_count, destruction_count};

    app.event_loop()->post([receiver]() -> void { receiver->delete_later(); });
    Application::post_event(receiver, std::make_unique<CustomEvent>());

    app.process_events(EventFlag::All, 0);

    CHECK(delivery_count == 1);
    CHECK(destruction_count == 1);
}

TEST_CASE("EventLoop exit drains deferred deletes without delivering remaining object events", "[core][event]")
{
    Application app{0, nullptr};
    int         delivery_count    = 0;
    int         destruction_count = 0;
    auto*       receiver          = new CountingEventReceiver{delivery_count, destruction_count};

    Application::post_event(receiver, std::make_unique<CustomEvent>());
    app.quit();
    app.event_loop()->post([receiver]() -> void { receiver->delete_later(); });

    app.exec();

    CHECK(delivery_count == 0);
    CHECK(destruction_count == 1);
}

TEST_CASE("EventLoop exit discards remaining object events", "[core][event]")
{
    Application   app{0, nullptr};
    EventReceiver receiver;
    int           destruction_count = 0;

    Application::post_event(&receiver, std::make_unique<TrackedEvent>(destruction_count));
    app.quit();
    app.exec();

    CHECK_FALSE(receiver.received);
    CHECK(destruction_count == 1);

    app.quit();
    app.exec();

    CHECK_FALSE(receiver.received);
    CHECK(destruction_count == 1);
}
