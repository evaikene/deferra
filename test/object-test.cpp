#include "object.hpp"

#include "application.hpp"
#include "event_thread.hpp"

#include <catch2/catch_test_macros.hpp>

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
        app.process_events(EventFlag::Tasks);
        CHECK(object_counter == 0);
    }

    // `delete_later` runs after event processing
    {
        object_counter = 0;

        jb::core::Application app{0, nullptr};

        auto* obj = new Testable;
        obj->delete_later();

        app.process_events(EventFlag::Events);
        CHECK(object_counter == 0);
    }

    // timer-only and watcher-only processing do not run deferred deletes
    {
        object_counter = 0;

        jb::core::Application app{0, nullptr};

        auto* obj = new Testable;
        obj->delete_later();

        app.process_events(EventFlag::Timers);
        CHECK(object_counter == 1);

        app.process_events(EventFlag::Watchers, 0);
        CHECK(object_counter == 1);

        app.process_events(EventFlag::Tasks);
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

        app.process_events(EventFlag::Events);
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

        app.process_events(EventFlag::Tasks);
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
        thread->exec(true);

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
        thread->quit();
        thread->wait();

        CHECK(object_counter == 0);

        // cleanup
        delete thread;
    }
}
