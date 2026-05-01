#include "object.hpp"

#include "application.hpp"
#include "event_thread.hpp"
#include "object_priv.hpp" // IWYU pragma: keep for accessing Object private data
#include "thread_context.hpp"

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

    ~Testable() override
    {
        --object_counter;
    }
};

} // anonymous namespace

TEST_CASE("Object basics", "[core]")
{
    // default constructor
    {
        object_counter = 0;

        auto* obj = new Testable();

        REQUIRE(obj->parent() == nullptr);
        REQUIRE(obj->children().empty());
        REQUIRE(obj->thread_ctx() == ThreadCtx::current());

        delete obj;

        REQUIRE(object_counter == 0);
    }

    // constructor with a parent
    {
        object_counter = 0;

        auto* parent = new Testable();
        auto* child = new Testable(parent);

        REQUIRE(child->parent() == parent);
        REQUIRE(parent->children().size() == 1);
        REQUIRE(parent->children().back() == child);

        delete parent;

        REQUIRE(object_counter == 0);
    }

    // add child to a parent
    {
        object_counter = 0;

        auto* parent = new Testable();
        auto* child = new Testable();

        child->set_parent(parent);
        REQUIRE(child->parent() == parent);
        REQUIRE(parent->children().size() == 1);
        REQUIRE(parent->children().back() == child);

        delete parent;

        REQUIRE(object_counter == 0);
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
        REQUIRE(child->parent() == parent);
        REQUIRE(parent->children().size() == 2);
        REQUIRE(parent->children().back() == child);

        delete parent;

        REQUIRE(object_counter == 0);
    }

    // remove child from the parent
    {
        object_counter = 0;

        auto* parent = new Testable();
        auto* child = new Testable(parent);

        child->set_parent(nullptr);
        REQUIRE(child->parent() == nullptr);
        REQUIRE(parent->children().empty());

        delete parent;
        delete child;

        REQUIRE(object_counter == 0);
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
        REQUIRE(child->parent() == nullptr);
        REQUIRE(parent->children().size() == 2);

        delete parent;
        delete child;

        REQUIRE(object_counter == 0);
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
        REQUIRE(parent->children().size() == 2);

        delete parent;

        REQUIRE(object_counter == 0);
    }

    // `destroyed` signal
    {
        object_counter = 0;

        // create receiver and object to be destroyed
        auto* receiver = new jb::core::Object;
        auto* obj = new Testable;
        bool destroyed = false;
        obj->destroyed.connect(receiver, [&destroyed]() -> void {
            destroyed = true;
        });

        // destroy the object and check the signal is emitted
        delete obj;
        REQUIRE(destroyed);

        delete receiver;
    }

    // `delete_later` method
    {
        object_counter = 0;

        jb::core::Application app{0, nullptr};

        auto* obj = new Testable;
        REQUIRE_NOTHROW(obj->delete_later());

        // object should not be deleted until the event loop processes events
        REQUIRE(object_counter == 1);

        // processing events should delete the object
        app.process_events(EventFlag::Tasks);
        REQUIRE(object_counter == 0);
    }

    // `move_to_thread` method
    {
        object_counter = 0;

        // default event loop
        Application app{0, nullptr};

        auto* thread = new EventThread;
        auto* obj = new Testable;
        auto* child = new Testable{obj};

        // moving the child to a different thread should fail because it has a parent
        REQUIRE_FALSE(child->move_to_thread(thread));

        // move object and all the children to the thread
        REQUIRE(obj->move_to_thread(thread));

        // moving the object again should still succeed, because the threaded event loop is not running yet
        REQUIRE(obj->move_to_thread(app.thread()));
        REQUIRE(obj->move_to_thread(thread));

        // run the threaded event loop
        thread->exec(true);

        // moving the object should now fail
        REQUIRE_FALSE(obj->move_to_thread(app.thread()));

        // verify the object's event loop is the one from the thread, not the main thread
        REQUIRE(obj->event_loop() != app.thread()->as_event_loop());
        REQUIRE(obj->event_loop() == thread->as_event_loop());
        REQUIRE(child->event_loop() == thread->as_event_loop());

        // let the threaded event loop delete the object
        obj->delete_later();

        // stop the threaded event loop and wait for it to finish
        thread->quit();
        thread->wait();

        REQUIRE(object_counter == 0);

        // cleanup
        delete thread;
    }
}
