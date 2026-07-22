#include "signal.hpp"

#include "application.hpp"
#include "event_thread.hpp"
#include "object.hpp"
#include "support/fake_event_loop_backend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

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

class ConstSender : public Object {
public:

    Signal<int> changed;

    void notify(int value) { emit(changed, value); }
};

class ConstReceiver : public Object {
public:

    void set_value(int value) { received = value; }

    int received{0};
};

class CopyCountingValue {
public:

    explicit CopyCountingValue(int value)
        : value{value}
        , instance{++instances}
        , lifetime{std::make_shared<int>(0)}
    {}

    CopyCountingValue(CopyCountingValue const& other)
        : value{other.value}
        , instance{++instances}
        , lifetime{other.lifetime}
    {
        ++copies;
    }

    CopyCountingValue(CopyCountingValue&&) noexcept                    = default;
    auto operator=(CopyCountingValue const&) -> CopyCountingValue&     = default;
    auto operator=(CopyCountingValue&&) noexcept -> CopyCountingValue& = default;

    static void reset()
    {
        copies    = 0;
        instances = 0;
    }

    int                  value;
    std::size_t          instance;
    std::shared_ptr<int> lifetime;

    static inline std::size_t copies{0};
    static inline std::size_t instances{0};
};

class CopyCountingSender : public Object {
public:

    Signal<CopyCountingValue> value_changed;

    void notify(CopyCountingValue const& value) { emit(value_changed, value); }
};

class CopyCountingReceiver : public Object {
public:

    void receive(CopyCountingValue const& value)
    {
        received_value    = value.value;
        received_instance = value.instance;
    }

    int         received_value{0};
    std::size_t received_instance{0};
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

TEST_CASE("Direct signal slots borrow owning arguments", "[core][signal]")
{
    CopyCountingSender       sender;
    CopyCountingReceiver     receiver;
    std::vector<int>         values;
    std::vector<std::size_t> instances;

    sender.value_changed.connect([&](CopyCountingValue const& value) {
        using Argument = std::remove_reference_t<decltype(value)>;
        static_assert(std::is_const_v<Argument>);

        values.push_back(value.value);
        instances.push_back(value.instance);
    });
    sender.value_changed.connect([&](CopyCountingValue const& value) {
        values.push_back(value.value);
        instances.push_back(value.instance);
    });
    sender.value_changed.connect(&receiver, &CopyCountingReceiver::receive);

    CopyCountingValue::reset();
    CopyCountingValue value{42};
    sender.notify(value);

    CHECK(values == std::vector<int>{42, 42});
    REQUIRE(instances.size() == 2);
    CHECK(instances[0] == instances[1]);
    CHECK(receiver.received_value == 42);
    CHECK(receiver.received_instance == instances[0]);
    CHECK(CopyCountingValue::copies == 0);
}

TEST_CASE("Direct signal slots may copy owning arguments intentionally", "[core][signal]")
{
    CopyCountingSender sender;
    std::size_t        borrowed_instance{0};
    std::size_t        copied_instance{0};

    sender.value_changed.connect([&](CopyCountingValue const& value) { borrowed_instance = value.instance; });
    sender.value_changed.connect([&](CopyCountingValue value) { copied_instance = value.instance; });

    CopyCountingValue::reset();
    CopyCountingValue value{42};
    sender.notify(value);

    CHECK(borrowed_instance != 0);
    CHECK(copied_instance != 0);
    CHECK(copied_instance != borrowed_instance);
    CHECK(CopyCountingValue::copies == 1);
}

TEST_CASE("Queued signal delivery owns local arguments", "[core][signal]")
{
    Application          app{0, nullptr};
    CopyCountingSender   sender;
    CopyCountingReceiver receiver;
    std::weak_ptr<int>   argument_lifetime;

    sender.value_changed.connect(&receiver, &CopyCountingReceiver::receive, ConnectionType::Queued);

    CopyCountingValue::reset();
    {
        CopyCountingValue value{42};
        argument_lifetime = value.lifetime;
        sender.notify(value);

        CHECK(receiver.received_value == 0);
        CHECK(CopyCountingValue::copies == 1);
    }

    CHECK_FALSE(argument_lifetime.expired());
    CHECK(app.process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
    CHECK(receiver.received_value == 42);
    CHECK(argument_lifetime.expired());
}

TEST_CASE("Queued signal slots share one owning snapshot", "[core][signal]")
{
    Application              app{0, nullptr};
    CopyCountingSender       sender;
    Object                   first_receiver;
    Object                   second_receiver;
    std::vector<int>         values;
    std::vector<std::size_t> instances;

    sender.value_changed.connect(
        &first_receiver,
        [&](CopyCountingValue const& value) {
            values.push_back(value.value);
            instances.push_back(value.instance);
        },
        ConnectionType::Queued);
    sender.value_changed.connect(
        &second_receiver,
        [&](CopyCountingValue const& value) {
            values.push_back(value.value);
            instances.push_back(value.instance);
        },
        ConnectionType::Queued);

    CopyCountingValue::reset();
    CopyCountingValue value{42};
    sender.notify(value);

    CHECK(values.empty());
    CHECK(CopyCountingValue::copies == 1);
    CHECK(app.process_events(EventFlag::Events) == ProcessEventsResult::Stopped);

    CHECK(values == std::vector<int>{42, 42});
    REQUIRE(instances.size() == 2);
    CHECK(instances[0] == instances[1]);
    CHECK(CopyCountingValue::copies == 1);
}

TEST_CASE("Queued signal slots observe the pre-dispatch snapshot", "[core][signal]")
{
    Application        app{0, nullptr};
    CopyCountingSender sender;
    Object             receiver;
    int                direct_value{0};
    int                queued_value{0};
    std::size_t        direct_instance{0};
    std::size_t        queued_instance{0};

    CopyCountingValue::reset();
    CopyCountingValue value{42};

    sender.value_changed.connect([&](CopyCountingValue const& observed) {
        direct_value    = observed.value;
        direct_instance = observed.instance;
        value.value     = 99;
    });
    sender.value_changed.connect(
        &receiver,
        [&](CopyCountingValue const& observed) {
            queued_value    = observed.value;
            queued_instance = observed.instance;
        },
        ConnectionType::Queued);

    sender.notify(value);

    CHECK(direct_value == 42);
    CHECK(direct_instance == value.instance);
    CHECK(value.value == 99);
    CHECK(CopyCountingValue::copies == 1);

    CHECK(app.process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
    CHECK(queued_value == 42);
    CHECK(queued_instance != direct_instance);
}

TEST_CASE("Cancelled queued signal slots release their shared snapshot", "[core][signal]")
{
    Application        app{0, nullptr};
    CopyCountingSender sender;
    Object             first_receiver;
    auto*              second_receiver = new Object;
    int                deliveries{0};
    std::weak_ptr<int> argument_lifetime;

    auto first_connection = sender.value_changed.connect(
        &first_receiver,
        [&](CopyCountingValue const&) { ++deliveries; },
        ConnectionType::Queued);
    sender.value_changed.connect(
        second_receiver,
        [&](CopyCountingValue const&) { ++deliveries; },
        ConnectionType::Queued);

    {
        CopyCountingValue value{42};
        argument_lifetime = value.lifetime;
        sender.notify(value);
    }

    first_connection.disconnect();
    delete second_receiver;

    CHECK_FALSE(argument_lifetime.expired());
    CHECK(app.process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
    CHECK(deliveries == 0);
    CHECK(argument_lifetime.expired());
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
        CHECK(app.process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);
        CHECK(captured_value == 0);
        CHECK(lambda_value == 0);
        CHECK(receiver->captured_value == 0);

        // object-event processing calls queued slots and updates values
        CHECK(app.process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
        CHECK(captured_value == 2);
        CHECK(lambda_value == 2);
        CHECK(receiver->captured_value == 2);

        delete receiver;

        // after the receiver is deleted, slots should no longer be called
        CHECK(obj->inc() == 3);
        CHECK(app.process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
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
    CHECK(app.process_events(EventFlag::Events) == ProcessEventsResult::Stopped);

    CHECK(receiver->captured_value == 1);
    CHECK(receiver->event_count == 0);

    CHECK(sender->inc() == 2);
    CHECK(app.process_events(EventFlag::All, 0) == ProcessEventsResult::Stopped);

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

    CHECK(app.process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
    CHECK(delivered == 0);

    CHECK(sender->inc() == 2);
    CHECK(sender->incremented.count() == 0);

    delete sender;
}

TEST_CASE("Queued signal delivery drops one emission when wakeup fails", "[core][signal]")
{
    auto                                   fake = jb::core::priv::make_fake_event_loop();
    jb::core::priv::ScopedCurrentEventLoop current_loop{fake.loop.get()};
    Testable                               sender;
    Testable                               receiver;
    sender.incremented.connect(&receiver, &Testable::incremented_slot, ConnectionType::Queued);

    fake.backend->wakeup_result = false;
    CHECK_NOTHROW(sender.inc());

    fake.backend->wakeup_result = true;
    CHECK(fake.loop->process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
    CHECK(receiver.captured_value == 0);

    CHECK(sender.inc() == 2);
    CHECK(fake.loop->process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
    CHECK(receiver.captured_value == 2);
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

TEST_CASE("Connecting context-free slots through const signals", "[core]")
{
    ConstSender        sender;
    ConstSender const& const_sender = sender;
    int                received     = 0;

    auto connection = const_sender.changed.connect([&received](int value) { received = value; });

    sender.notify(42);

    CHECK(connection.is_valid());
    CHECK(received == 42);
}

TEST_CASE("Connecting receiver slots through const signals", "[core]")
{
    ConstSender        sender;
    ConstSender const& const_sender = sender;
    ConstReceiver      receiver;

    auto connection = const_sender.changed.connect(&receiver, &ConstReceiver::set_value);

    sender.notify(42);

    CHECK(connection.is_valid());
    CHECK(receiver.received == 42);
}

TEST_CASE("Disconnecting through const signals", "[core]")
{
    ConstSender        sender;
    ConstSender const& const_sender = sender;
    int                calls        = 0;

    auto connection = const_sender.changed.connect([&calls](int) { ++calls; });

    sender.notify(1);
    const_sender.changed.disconnect(connection);
    sender.notify(2);

    CHECK(calls == 1);
    CHECK_FALSE(connection.is_valid());
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
    CHECK(thread.exec(true));

    // Create the `receiver` object in the context of the thread
    auto* receiver = new Object;

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

    // Connections created before a move must use the receiver's current loop.
    receiver->move_to_thread(&thread);

    CHECK(obj->inc() == 1);
    CHECK(obj->inc() == 2);

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (queued_value.load(std::memory_order_relaxed) != 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    CHECK(queued_value.load(std::memory_order_relaxed) == 2);

    CHECK(thread.quit());
    thread.wait();

    // now the slot should have been called only using the threaded event loop
    CHECK(direct_value == 0);
    CHECK(queued_value.load(std::memory_order_relaxed) == 2);

    // cleanup
    delete obj;
    delete receiver; // safe to delete after the thread has finished
}
