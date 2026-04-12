#define private public
#define protected public
#include "object.hpp"
#undef protected
#undef private

#include "object_priv.hpp"
#include "thread_context.hpp"

#include <catch2/catch_test_macros.hpp>

namespace {
    
int object_counter = 0;

class Testable : public df::core::Object {
public:

    explicit Testable(Testable* parent = nullptr)
        : df::core::Object(parent)
    {
        ++object_counter;
    }
    
    ~Testable() override
    {
        --object_counter;
    }
};

} // anonymous namespace

TEST_CASE("constructors", "[Object]")
{
    // default constructor
    {
        object_counter = 0;

        auto* obj = new Testable();

        REQUIRE(obj->_d->parent == nullptr);
        REQUIRE(obj->_d->children.empty());
        REQUIRE(obj->_d->thread_ctx == df::core::ThreadCtx::current());

        delete obj;

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
}
