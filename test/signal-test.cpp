#include "signal.hpp"

#include "application.hpp"
#include "event_thread.hpp"
#include "object.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

using namespace jb::core;

namespace {

class Testable : public Object {
public:

    explicit Testable()  = default;
    ~Testable() override = default;

    int value{0};
    int captured_value{0};

    Signal<int> incremented;

    auto inc() -> int
    {
        ++value;
        emit(incremented, value);
        return value;
    }

    void incremented_slot(int v) { captured_value = v; }
};

class EventCountingTestable : public Testable {
public:

    auto event(Event& event) -> bool override
    {
        ++event_count;
        return Testable::event(event);
    }

    int event_count{0};
};

int captured_value = 0;

void value_incremented(int v)
{
    captured_value = v;
}

} // anonymous namespace

TEST_CASE("Direct signal-slot connection", "[core]")
{
    // Test direct slots without receiver
    {
        captured_value   = 0;
        int lambda_value = 0;

        auto* obj = new Testable();
        obj->incremented.connect(value_incremented);
        obj->incremented.connect([&lambda_value](int v) -> void { lambda_value = v; });

        // incrementing the testable should call slots and update values
        CHECK(obj->inc() == 1);
        CHECK(obj->inc() == 2);
        CHECK(captured_value == 2);
        CHECK(lambda_value == 2);

        delete obj;
    }

    // Test direct slots with receiver
    {
        auto* receiver = new Object;

        captured_value   = 0;
        int lambda_value = 0;

        auto* obj = new Testable();
        obj->incremented.connect(receiver, value_incremented);
        obj->incremented.connect(receiver, [&lambda_value](int v) -> void { lambda_value = v; });

        // incrementing the testable should call slots and update values
        CHECK(obj->inc() == 1);
        CHECK(obj->inc() == 2);
        CHECK(captured_value == 2);
        CHECK(lambda_value == 2);

        delete receiver;

        // after the receiver is deleted, slots should no longer be called
        CHECK(obj->inc() == 3);
        CHECK(captured_value == 2);
        CHECK(lambda_value == 2);

        delete obj;
    }

    // Test direct member function slot
    {
        captured_value = 0;

        auto* obj      = new Testable();
        auto* receiver = new Testable();
        obj->incremented.connect(receiver, &Testable::incremented_slot);

        // incrementing the testable should call the member function slot and update the value
        CHECK(obj->inc() == 1);
        CHECK(obj->inc() == 2);
        CHECK(receiver->captured_value == 2);

        delete receiver;
        delete obj;
    }
}

TEST_CASE("Queued signal-slot connection", "[core]")
{
    // Setup event loop
    Application app{0, nullptr};

    // Test queued slots with receiver
    {
        auto* receiver = new Testable();

        captured_value   = 0;
        int lambda_value = 0;

        auto* obj = new Testable();
        obj->incremented.connect(receiver, value_incremented, ConnectionType::Queued);
        obj->incremented.connect(
            receiver,
            [&lambda_value](int v) -> void { lambda_value = v; },
            jb::core::ConnectionType::Queued);
        obj->incremented.connect(receiver, &Testable::incremented_slot, ConnectionType::Queued);

        // Signal should have 3 connections
        CHECK(obj->incremented.count() == 3);

        // incrementing the testable should NOT call slots yet
        CHECK(obj->inc() == 1);
        CHECK(obj->inc() == 2);
        CHECK(captured_value == 0);
        CHECK(lambda_value == 0);
        CHECK(receiver->captured_value == 0);

        // generic tasks do not deliver queued signals
        app.process_events(EventFlag::Tasks);
        CHECK(captured_value == 0);
        CHECK(lambda_value == 0);
        CHECK(receiver->captured_value == 0);

        // object-event processing calls queued slots and updates values
        app.process_events(EventFlag::Events);
        CHECK(captured_value == 2);
        CHECK(lambda_value == 2);
        CHECK(receiver->captured_value == 2);

        delete receiver;

        // after the receiver is deleted, slots should no longer be called
        CHECK(obj->inc() == 3);
        app.process_events(EventFlag::Events);
        CHECK(captured_value == 2);
        CHECK(lambda_value == 2);

        // Signal should have no connections left
        CHECK(obj->incremented.count() == 0);

        delete obj;
    }
}

TEST_CASE("Queued signal delivery bypasses Object event overrides", "[core]")
{
    Application app{0, nullptr};

    auto* sender   = new Testable;
    auto* receiver = new EventCountingTestable;
    sender->incremented.connect(receiver, &Testable::incremented_slot, ConnectionType::Queued);

    CHECK(sender->inc() == 1);
    app.process_events(EventFlag::Events);

    CHECK(receiver->captured_value == 1);
    CHECK(receiver->event_count == 0);

    CHECK(sender->inc() == 2);
    app.process_events(EventFlag::All, 0);

    CHECK(receiver->captured_value == 2);
    CHECK(receiver->event_count == 0);

    delete receiver;
    delete sender;
}

TEST_CASE("Queued signal delivery skips destroyed receivers", "[core]")
{
    Application app{0, nullptr};

    int   delivered = 0;
    auto* sender    = new Testable;
    auto* receiver  = new Testable;
    sender->incremented.connect(receiver, [&delivered](int) -> void { ++delivered; }, ConnectionType::Queued);

    CHECK(sender->inc() == 1);
    delete receiver;

    CHECK_NOTHROW(app.process_events(EventFlag::Events));
    CHECK(delivered == 0);

    CHECK(sender->inc() == 2);
    CHECK(sender->incremented.count() == 0);

    delete sender;
}

TEST_CASE("Disconnecting signal-slot connections", "[core]")
{
    auto* receiver = new Object;

    captured_value   = 0;
    int lambda_value = 0;

    auto slot = [&lambda_value](int v) -> void { lambda_value = v; };

    auto* obj = new Testable();
    auto  c1  = obj->incremented.connect(receiver, value_incremented);
    auto  c2  = obj->incremented.connect(receiver, slot);

    // Signal should have 2 connections
    CHECK(obj->incremented.count() == 2);

    // incrementing the testable should call slots and update values
    CHECK(obj->inc() == 1);
    CHECK(obj->inc() == 2);
    CHECK(captured_value == 2);
    CHECK(lambda_value == 2);

    // disconnect the slots
    obj->incremented.disconnect(c1);
    obj->incremented.disconnect(c2);

    // incrementing the testable should no longer call the disconnected slots
    CHECK(obj->inc() == 3);
    CHECK(captured_value == 2);
    CHECK(lambda_value == 2);

    delete receiver;
    delete obj;
}

TEST_CASE("Modifying signal-slot connections inside a slot", "[core]")
{
    auto* receiver = new Object;

    int  count = 0; // how many times the slot was called
    auto slot  = [&count](int v) -> void { ++count; };

    // chained connect
    auto* obj = new Testable();
    obj->incremented.connect(receiver, slot);
    obj->incremented.connect(receiver,
                             [obj, receiver, &slot](int) -> void { obj->incremented.connect(receiver, slot); });

    // incrementing the testable should call the slot once
    CHECK(obj->inc() == 1);
    CHECK(count == 1);

    // incrementing the testable again should call the slot twice
    CHECK(obj->inc() == 2);
    CHECK(count == 3);

    // incrementing the testable once again should call the slot three times
    CHECK(obj->inc() == 3);
    CHECK(count == 6);

    delete receiver;
    delete obj;
}

TEST_CASE("Auto signal-slot connection with threaded event loop", "[core][test]")
{
    // Setup main event loop
    Application app{0, nullptr};
    logger()->set_level(LogLevel::Info);

    // Create a separate thread with its own event loop
    EventThread thread;
    thread.exec(true);

    // Create the `receiver` object in the context of the thread
    auto* receiver = new Object;
    receiver->move_to_thread(&thread);

    int              direct_value{0}; // incremented if the slot is called directly
    std::atomic<int> queued_value{0}; // incremented if the slot is called via the threaded event loop

    auto* obj = new Testable();
    obj->incremented.connect(receiver, [&app, &direct_value, &queued_value](int v) -> void {
        if (app.thread_ctx() == ThreadCtx::current()) {
            direct_value = v;
        }
        else {
            queued_value.store(v, std::memory_order_relaxed);
        }
    });

    CHECK(obj->inc() == 1);
    CHECK(obj->inc() == 2);

    while (queued_value.load(std::memory_order_relaxed) != 2) {
        std::this_thread::yield();
    }

    thread.quit();
    thread.wait();

    // now the slot should have been called only using the threaded event loop
    CHECK(direct_value == 0);
    CHECK(queued_value.load(std::memory_order_relaxed) == 2);

    // cleanup
    delete obj;
    delete receiver; // safe to delete after the thread has finished
}
