#include "signal.hpp"

#include "application.hpp"
#include "event_thread.hpp"
#include "object.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>

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
        incremented(value);
        return value;
    }

    void incremented_slot(int v)
    {
        captured_value = v;
    }

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
        obj->incremented.connect(nullptr, value_incremented);
        obj->incremented.connect(nullptr, [&lambda_value](int v) -> void { lambda_value = v; });

        // incrementing the testable should call slots and update values
        REQUIRE(obj->inc() == 1);
        REQUIRE(obj->inc() == 2);
        REQUIRE(captured_value == 2);
        REQUIRE(lambda_value == 2);

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
        REQUIRE(obj->inc() == 1);
        REQUIRE(obj->inc() == 2);
        REQUIRE(captured_value == 2);
        REQUIRE(lambda_value == 2);

        delete receiver;

        // after the receiver is deleted, slots should no longer be called
        REQUIRE(obj->inc() == 3);
        REQUIRE(captured_value == 2);
        REQUIRE(lambda_value == 2);

        delete obj;
    }

    // Test direct member function slot
    {
        captured_value   = 0;

        auto* obj = new Testable();
        auto* receiver = new Testable();
        obj->incremented.connect(receiver, &Testable::incremented_slot);

        // incrementing the testable should call the member function slot and update the value
        REQUIRE(obj->inc() == 1);
        REQUIRE(obj->inc() == 2);
        REQUIRE(receiver->captured_value == 2);

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
        REQUIRE(obj->incremented.connection_count() == 3);

        // incrementing the testable should NOT call slots yet
        REQUIRE(obj->inc() == 1);
        REQUIRE(obj->inc() == 2);
        REQUIRE(captured_value == 0);
        REQUIRE(lambda_value == 0);
        REQUIRE(receiver->captured_value == 0);

        // processing events should call the queued slots and update values
        app.process_events();
        REQUIRE(captured_value == 2);
        REQUIRE(lambda_value == 2);
        REQUIRE(receiver->captured_value == 2);

        delete receiver;

        // after the receiver is deleted, slots should no longer be called
        REQUIRE(obj->inc() == 3);
        app.process_events();
        REQUIRE(captured_value == 2);
        REQUIRE(lambda_value == 2);

        // Signal should have no connections left
        REQUIRE(obj->incremented.connection_count() == 0);

        delete obj;
    }
}

TEST_CASE("Disconnecting signal-slot connections", "[core]")
{
    auto* receiver = new Object;

    captured_value   = 0;
    int lambda_value = 0;

    auto slot = [&lambda_value](int v) -> void { lambda_value = v; };

    auto* obj = new Testable();
    auto c1 = obj->incremented.connect(receiver, value_incremented);
    auto c2 = obj->incremented.connect(receiver, slot);

    // Signal should have 2 connections
    REQUIRE(obj->incremented.connection_count() == 2);

    // incrementing the testable should call slots and update values
    REQUIRE(obj->inc() == 1);
    REQUIRE(obj->inc() == 2);
    REQUIRE(captured_value == 2);
    REQUIRE(lambda_value == 2);

    // disconnect the slots
    obj->incremented.disconnect(c1);
    obj->incremented.disconnect(c2);

    // Signal should have no connections left
    REQUIRE(obj->incremented.connection_count() == 0);

    // incrementing the testable should no longer call the disconnected slots
    REQUIRE(obj->inc() == 3);
    REQUIRE(captured_value == 2);
    REQUIRE(lambda_value == 2);

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
    REQUIRE(obj->inc() == 1);
    REQUIRE(count == 1);

    // incrementing the testable again should call the slot twice
    REQUIRE(obj->inc() == 2);
    REQUIRE(count == 3);

    // incrementing the testable once again should call the slot three times
    REQUIRE(obj->inc() == 3);
    REQUIRE(count == 6);

    delete receiver;
    delete obj;
}

TEST_CASE("Auto signal-slot connection with threaded event loop", "[core]")
{
    // Setup main event loop
    Application app{0, nullptr};

    // Create a separate thread with its own event loop
    EventThread thread;

    // Create the `receiver` object in the context of the thread
    auto* receiver = new Object;
    receiver->move_to_thread(&thread);

    std::atomic<int> value{0};
    auto* obj = new Testable();
    obj->incremented.connect(receiver, [&value](int v) -> void { value.store(v, std::memory_order_relaxed); });

    // incrementing the testable should queue the slot
    REQUIRE(obj->inc() == 1);
    REQUIRE(obj->inc() == 2);

    // the slot should be not called yet
    REQUIRE(value.load(std::memory_order_relaxed) == 0);

    // run the threaded event loop and quit it
    thread.exec();
    thread.quit();
    thread.wait();

    // now the slot should have been called and the value updated
    REQUIRE(value.load(std::memory_order_relaxed) == 2);

    // cleanup
    delete obj;
    delete receiver; // safe to delete after the thread has finished
}
