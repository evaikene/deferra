#define private   public
#define protected public
#include "signal.hpp"
#undef protected
#undef private

#include "application.hpp"
#include "event_loop.hpp"
#include "object.hpp"
#include "thread_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

namespace {

class Testable : public jb::core::Object {
public:

    explicit Testable()  = default;
    ~Testable() override = default;

    int value{0};

    jb::core::Signal<int> incremented;

    auto inc() -> int
    {
        ++value;
        incremented.emit(value);
        return value;
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
        auto* receiver = new jb::core::Object;

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
}

TEST_CASE("Queued signal-slot connection", "[core]")
{
    // Setup event loop
    jb::core::Application app{0, nullptr};

    // Test queued slots with receiver
    {
        auto* receiver = new jb::core::Object;

        captured_value   = 0;
        int lambda_value = 0;

        auto* obj = new Testable();
        obj->incremented.connect(receiver, value_incremented, jb::core::ConnectionType::Queued);
        obj->incremented.connect(
            receiver,
            [&lambda_value](int v) -> void { lambda_value = v; },
            jb::core::ConnectionType::Queued);

        // Signal should have 2 connections
        REQUIRE(obj->incremented._connections.size() == 2);

        // incrementing the testable should NOT call slots yet
        REQUIRE(obj->inc() == 1);
        REQUIRE(obj->inc() == 2);
        REQUIRE(captured_value == 0);
        REQUIRE(lambda_value == 0);

        // processing events should call the queued slots and update values
        app.process_events();
        REQUIRE(captured_value == 2);
        REQUIRE(lambda_value == 2);

        delete receiver;

        // after the receiver is deleted, slots should no longer be called
        REQUIRE(obj->inc() == 3);
        app.process_events();
        REQUIRE(captured_value == 2);
        REQUIRE(lambda_value == 2);

        // Signal should have no connections left
        REQUIRE(obj->incremented._connections.empty());

        delete obj;
    }
}

TEST_CASE("Disconnecting signal-slot connections", "[core]")
{
    auto* receiver = new jb::core::Object;

    captured_value   = 0;
    int lambda_value = 0;

    auto slot = [&lambda_value](int v) -> void { lambda_value = v; };

    auto* obj = new Testable();
    obj->incremented.connect(receiver, value_incremented);
    obj->incremented.connect(receiver, slot);

    // Signal should have 2 connections
    REQUIRE(obj->incremented._connections.size() == 2);

    // incrementing the testable should call slots and update values
    REQUIRE(obj->inc() == 1);
    REQUIRE(obj->inc() == 2);
    REQUIRE(captured_value == 2);
    REQUIRE(lambda_value == 2);

    // disconnect the slots
    obj->incremented.disconnect(receiver, value_incremented);
    obj->incremented.disconnect(receiver, slot);

    // Signal should have no connections left
    REQUIRE(obj->incremented._connections.empty());

    // incrementing the testable should no longer call the disconnected slots
    REQUIRE(obj->inc() == 3);
    REQUIRE(captured_value == 2);
    REQUIRE(lambda_value == 2);

    delete receiver;
    delete obj;
}

TEST_CASE("Modifying signal-slot connections inside a slot", "[core]")
{
    auto* receiver = new jb::core::Object;

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
    jb::core::Application app{0, nullptr};

    // Start separate thread with its own event loop and receiver object
    std::atomic<jb::core::Object*> receiver{nullptr};
    jb::core::EventLoop            event_loop;
    std::thread                    thread([&receiver, &event_loop]() -> void {
        jb::core::ThreadCtx::current()->set_event_loop(&event_loop);

        // create receiver object on this thread
        receiver.store(new jb::core::Object, std::memory_order_release);

        // run the event loop until quit is signaled
        event_loop.run();

        // cleanup
        delete receiver;
        receiver = nullptr;
        jb::core::ThreadCtx::current()->set_event_loop(nullptr);
    });

    // wait for the event loop to start
    while (!event_loop.is_running()) {
        std::this_thread::yield();
    }

    // Test queued slots with receiver
    {
        std::atomic<int> value{0};

        auto* obj = new Testable();
        obj->incremented.connect(
            receiver.load(std::memory_order_acquire),
            [&value](int v) -> void { value.store(v, std::memory_order_relaxed); },
            jb::core::ConnectionType::Queued);

        // incrementing the testable should queue the slot
        REQUIRE(obj->inc() == 1);
        REQUIRE(obj->inc() == 2);

        // terminate the event loop and thread
        event_loop.quit();
        thread.join();

        // now the slot should have been called and the value updated
        REQUIRE(value.load(std::memory_order_relaxed) == 2);

        // cleanup
        delete obj;
    }
}
