#define private public
#define protected public
#include "object.hpp"
#undef protected
#undef private

#include "event_loop.hpp"
#include "object_priv.hpp" // IWYU pragma: keep for accessing Object private data
#include "thread_context.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {

int object_counter = 0;

/// Testable class that counts the number of existing instances.
class Testable : public jb::core::Object {
public:

    explicit Testable(Testable* parent = nullptr)
        : jb::core::Object(parent)
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
        auto token = obj->token();

        REQUIRE(obj->_d->parent == nullptr);
        REQUIRE(obj->_d->children.empty());
        REQUIRE(obj->_d->thread_ctx == jb::core::ThreadCtx::current());
        REQUIRE(token.lock());

        delete obj;
        REQUIRE(!token.lock());

        REQUIRE(object_counter == 0);
    }

    // constructor with a parent
    {
        object_counter = 0;

        auto* parent = new Testable();
        auto* child = new Testable(parent);

        REQUIRE(child->_d->parent == parent);
        REQUIRE(parent->_d->children.size() == 1);
        REQUIRE(parent->_d->children.back() == child);

        delete parent;

        REQUIRE(object_counter == 0);
    }

    // add child to a parent
    {
        object_counter = 0;

        auto* parent = new Testable();
        auto* child = new Testable();

        child->set_parent(parent);
        REQUIRE(child->_d->parent == parent);
        REQUIRE(parent->_d->children.size() == 1);
        REQUIRE(parent->_d->children.back() == child);

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
        REQUIRE(child->_d->parent == parent);
        REQUIRE(parent->_d->children.size() == 2);
        REQUIRE(parent->_d->children.back() == child);

        delete parent;

        REQUIRE(object_counter == 0);
    }

    // remove child from the parent
    {
        object_counter = 0;

        auto* parent = new Testable();
        auto* child = new Testable(parent);

        child->set_parent(nullptr);
        REQUIRE(child->_d->parent == nullptr);
        REQUIRE(parent->_d->children.empty());

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
        REQUIRE(child->_d->parent == nullptr);
        REQUIRE(parent->_d->children.size() == 2);

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
        REQUIRE(parent->_d->children.size() == 2);

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

        jb::core::EventLoop event_loop;
        jb::core::ThreadCtx::current()->set_event_loop(&event_loop);

        auto* obj = new Testable;
        REQUIRE_NOTHROW(obj->delete_later());

        // object should not be deleted until the event loop processes events
        REQUIRE(object_counter == 1);

        // processing events should delete the object
        event_loop.process_events();
        REQUIRE(object_counter == 0);

        // cleanup
        jb::core::ThreadCtx::current()->set_event_loop(nullptr);
    }
}
