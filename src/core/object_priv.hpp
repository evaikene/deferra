#pragma once

#include "connectable.hpp"
#include "object.hpp"
#include "thread_context.hpp"

#include <cassert>
#include <vector>

namespace jb::core::priv {

struct ObjectData : public ObjectToken {

    /// Thread affinity (never nullptr)
    jb::core::ThreadCtx const* thread_ctx = jb::core::ThreadCtx::current();

    /// Event loop this object lives on (can be nullptr if not set)
    EventLoop* event_loop = jb::core::ThreadCtx::current()->event_loop();

    /// Optional parent
    Object* parent = nullptr;

    /// Optional children
    std::vector<Object*> children;

    /// Finds a child
    /// @param[in] child The child object
    /// @return Iterator to the child when found
    auto find_child(Object* child) const -> std::vector<Object*>::const_iterator
    {
        for (auto it = children.cbegin(); it != children.cend(); ++it) {
            if (*it == child) {
                return it;
            }
        }
        return children.cend();
    }

    /// Adds a child
    /// @param[in] child The child object
    void add_child(Object* child)
    {
        // only children within the same thread can be added
        assert(thread_ctx == child->_d->thread_ctx);

        if (find_child(child) != children.cend()) {
            return; // already in the list
        }
        children.push_back(child);
    }

    /// Removes a child
    /// @param[in] child The child object
    void remove_child(Object* child)
    {
        auto it = find_child(child);
        if (it == children.cend()) {
            return;
        }
        children.erase(it);
    }
};

} // namespace jb::core::priv
