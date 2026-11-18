#include "object.hpp"

#include "application.hpp"
#include "event_thread.hpp"
#include "logging.hpp"
#include "support/fake_event_loop_backend.hpp"
#include "support/operational_log_capture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

using namespace jb::core;

namespace {

std::atomic<int> object_counter{0};

/// Testable class that counts the number of existing instances.
class Testable : public Object {
public:

    explicit Testable(Testable* parent = nullptr)
        : Object(parent)
    {
        ++object_counter;
    }

    ~Testable() override { --object_counter; }
};

class TeardownObject : public Object {
public:

    using Object::Object;

    ~TeardownObject() override { emit(tearing_down); }

    Signal<> tearing_down;
};

} // anonymous namespace

TEST_CASE("Object basics", "[core]")
{
    // do not abort on fatal errors
    logger()->set_abort_on_fatal_error(false);
    logger()->set_level(LogLevel::Info);

    // default constructor without Application instance
    {
        object_counter = 0;

        auto* obj = new Testable();

        CHECK(obj->parent() == nullptr);
        CHECK(obj->children().empty());

        // there shouldn't be any event loop or thread context because no Application instance exists yet
        CHECK(obj->event_loop() == nullptr);
        CHECK(obj->thread_ctx() == nullptr);

        delete obj;

        CHECK(object_counter == 0);
    }

    // default constructor with Application instance
    {
        jb::core::Application app{0, nullptr};

        object_counter = 0;

        auto* obj = new Testable();

        CHECK(obj->parent() == nullptr);
        CHECK(obj->children().empty());

        // the object's event loop and thread context should be the same as the application's
        CHECK(obj->event_loop() == app.event_loop());
        CHECK(obj->thread_ctx() == app.thread_ctx());

        delete obj;

        CHECK(object_counter == 0);
    }

    // constructor with a parent
    {
        object_counter = 0;

        auto* parent = new Testable();
        auto* child  = new Testable(parent);

        CHECK(child->parent() == parent);
        CHECK(parent->children().size() == 1);
        CHECK(parent->children().back() == child);

        delete parent;

        CHECK(object_counter == 0);
    }

    // add child to a parent
    {
        object_counter = 0;

        auto* parent = new Testable();
        auto* child  = new Testable();

        child->set_parent(parent);
        CHECK(child->parent() == parent);
        CHECK(parent->children().size() == 1);
        CHECK(parent->children().back() == child);

        delete parent;

        CHECK(object_counter == 0);
    }

    // add child to a parent that already has children
    {
        object_counter = 0;

        auto* parent = new Testable();
        {
            auto* _ = new Testable(parent);
        }
        auto* child = new Testable();

        child->set_parent(parent);
        CHECK(child->parent() == parent);
        CHECK(parent->children().size() == 2);
        CHECK(parent->children().back() == child);

        delete parent;

        CHECK(object_counter == 0);
    }

    // remove child from the parent
    {
        object_counter = 0;

        auto* parent = new Testable();
        auto* child  = new Testable(parent);

        child->set_parent(nullptr);
        CHECK(child->parent() == nullptr);
        CHECK(parent->children().empty());

        delete parent;
        delete child;

        CHECK(object_counter == 0);
    }

    // remove child from the parent that has multiple children
    {
        object_counter = 0;

        auto* parent = new Testable();

        // 3 children
        {
            auto* _ = new Testable(parent);
        }
        auto* child = new Testable(parent);
        {
            auto* _ = new Testable(parent);
        }

        child->set_parent(nullptr);
        CHECK(child->parent() == nullptr);
        CHECK(parent->children().size() == 2);

        delete parent;
        delete child;

        CHECK(object_counter == 0);
    }

    // explicitly delete child before the parent
    {
        object_counter = 0;

        auto* parent = new Testable();

        // 3 children
        {
            auto* _ = new Testable(parent);
        }
        auto* child = new Testable(parent);
        {
            auto* _ = new Testable(parent);
        }

        delete child;
        CHECK(parent->children().size() == 2);

        delete parent;

        CHECK(object_counter == 0);
    }

    // `destroyed` signal
    {
        object_counter = 0;

        // create receiver and object to be destroyed
        auto* receiver  = new jb::core::Object;
        auto* obj       = new Testable;
        bool  destroyed = false;
        obj->destroyed.connect(receiver, [&destroyed]() -> void { destroyed = true; });

        // destroy the object and check the signal is emitted
        delete obj;
        CHECK(destroyed);

        delete receiver;
    }

    // `delete_later` method
    {
        object_counter = 0;

        jb::core::Application app{0, nullptr};

        auto* obj = new Testable;
        REQUIRE_NOTHROW(obj->delete_later());

        // object should not be deleted until the event loop processes events
        CHECK(object_counter == 1);

        // task processing should delete the object
        CHECK(app.process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);
        CHECK(object_counter == 0);
    }

    // `delete_later` runs after event processing
    {
        object_counter = 0;

        jb::core::Application app{0, nullptr};

        auto* obj = new Testable;
        obj->delete_later();

        CHECK(app.process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
        CHECK(object_counter == 0);
    }

    // timer-only and watcher-only processing do not run deferred deletes
    {
        object_counter = 0;

        jb::core::Application app{0, nullptr};

        auto* obj = new Testable;
        obj->delete_later();

        CHECK(app.process_events(EventFlag::Timers) == ProcessEventsResult::Stopped);
        CHECK(object_counter == 1);

        CHECK(app.process_events(EventFlag::Watchers, 0) == ProcessEventsResult::Stopped);
        CHECK(object_counter == 1);

        CHECK(app.process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);
        CHECK(object_counter == 0);
    }

    // repeated `delete_later` calls only delete once
    {
        object_counter = 0;

        jb::core::Application app{0, nullptr};

        auto* obj = new Testable;
        obj->delete_later();
        obj->delete_later();

        CHECK(object_counter == 1);

        CHECK(app.process_events(EventFlag::Events) == ProcessEventsResult::Stopped);
        CHECK(object_counter == 0);
    }

    // pending `delete_later` tasks are ignored after parent-owned destruction
    {
        object_counter = 0;

        jb::core::Application app{0, nullptr};

        auto* parent = new Testable;
        auto* child  = new Testable{parent};

        child->delete_later();
        delete parent;

        CHECK(object_counter == 0);

        CHECK(app.process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);
        CHECK(object_counter == 0);
    }

    // `move_to_thread` method
    {
        object_counter = 0;

        // default event loop
        Application app{0, nullptr};

        auto* thread     = new EventThread;
        auto* obj        = new Testable;
        auto* child      = new Testable{obj};
        auto* grandchild = new Testable{child};

        // moving the child to a different thread should fail because it has a parent
        CHECK_FALSE(child->move_to_thread(thread));

        // move object and all the children to the thread
        CHECK(obj->move_to_thread(thread));

        // moving the object again should still succeed, because the threaded event loop is not running yet
        CHECK(obj->move_to_thread(app.thread()));
        CHECK(obj->move_to_thread(thread));

        // run the threaded event loop
        CHECK(thread->exec(true));

        // moving the object should now fail
        CHECK_FALSE(obj->move_to_thread(app.thread()));

        // verify the object's event loop is the one from the thread, not the main thread
        CHECK(obj->event_loop() != app.thread()->as_event_loop());
        CHECK(obj->event_loop() == thread->as_event_loop());
        CHECK(child->event_loop() == thread->as_event_loop());
        CHECK(grandchild->event_loop() == thread->as_event_loop());

        // let the threaded event loop delete the object
        obj->delete_later();

        // stop the threaded event loop and wait for it to finish
        CHECK(thread->quit());
        thread->wait();

        CHECK(object_counter == 0);

        // cleanup
        delete thread;
    }
}

TEST_CASE("Object delete_later can be retried after wakeup failure", "[core][object]")
{
    object_counter = 0;

    auto                                   fake = jb::core::priv::make_fake_event_loop();
    jb::core::priv::ScopedCurrentEventLoop current_loop{fake.loop.get()};
    auto*                                  object = new Testable;

    fake.backend->wakeup_result = false;
    CHECK_NOTHROW(object->delete_later());
    CHECK(object_counter == 1);

    fake.backend->wakeup_result = true;
    CHECK_NOTHROW(object->delete_later());
    CHECK(fake.loop->process_events(EventFlag::Tasks) == ProcessEventsResult::Stopped);
    CHECK(object_counter == 0);
}

TEST_CASE("Object rejects ownership cycles through an owner in teardown", "[core][object]")
{
    jb::test::OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);

    Object root;
    auto*  branch = new Object{&root};
    auto*  child  = new Object{branch};
    CHECK_FALSE(root.set_parent(&root));
    CHECK_FALSE(root.set_parent(child));

    branch->destroyed.connect(child, [&root, branch, child]() -> void {
        CHECK(child->parent() == branch);
        CHECK_FALSE(root.set_parent(child));
        CHECK(root.parent() == nullptr);
        CHECK(root.children() == std::vector<Object*>{branch});
        CHECK(branch->children() == std::vector<Object*>{child});
    });
    delete branch;

    CHECK(root.children().empty());
    CHECK(logs.capture->records().size() == 3);
}

TEST_CASE("Object can release a child during its destroyed signal", "[core][object]")
{
    jb::test::OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);

    auto                    owner          = std::make_unique<Object>();
    auto*                   original_owner = owner.get();
    auto*                   child          = new Object{original_owner};
    std::unique_ptr<Object> detached;

    owner->destroyed.connect(child, [original_owner, child, &detached]() -> void {
        CHECK(child->parent() == original_owner);
        auto* candidate = new Object;
        CHECK_FALSE(candidate->set_parent(original_owner));
        CHECK(candidate->parent() == nullptr);
        if (!candidate->parent()) {
            delete candidate;
        }

        CHECK(child->set_parent(nullptr));
        if (!child->parent()) {
            detached.reset(child);
        }
    });
    owner.reset();

    REQUIRE(detached);
    CHECK(detached->parent() == nullptr);
    CHECK(logs.capture->records().size() == 1);
}

TEST_CASE("Object rejects transfers after recursive deletion is committed", "[core][object]")
{
    jb::test::OperationalLogGuard logs;
    logs.capture->set_level(LogLevel::Fatal);

    Object           destination;
    std::vector<int> destroyed;
    auto             owner          = std::make_unique<Object>();
    auto*            original_owner = owner.get();
    auto*            first          = new TeardownObject{original_owner};
    auto*            later          = new Object{original_owner};
    first->destroyed.connect([&destroyed]() -> void { destroyed.push_back(1); });
    later->destroyed.connect([&destroyed]() -> void { destroyed.push_back(2); });

    first->tearing_down.connect(later, [first, later, original_owner, &destination]() -> void {
        // Core cleared the first link before its subclass destructor. The
        // later sibling still points to the owner but is already claimed too.
        CHECK(first->parent() == nullptr);
        CHECK(later->parent() == original_owner);
        CHECK(original_owner->children().empty());

        CHECK_FALSE(first->set_parent(nullptr));
        CHECK_FALSE(first->set_parent(&destination));
        CHECK_FALSE(later->set_parent(nullptr));
        CHECK_FALSE(later->set_parent(&destination));

        CHECK(first->parent() == nullptr);
        CHECK(later->parent() == original_owner);
        CHECK(destination.children().empty());
    });
    owner.reset();

    CHECK(destroyed == std::vector{1, 2});
    CHECK(logs.capture->records().size() == 4);
}
