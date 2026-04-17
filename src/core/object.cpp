#include "object.hpp"

#include "event_thread.hpp"
#include "object_priv.hpp"

#include <cassert>

namespace jb::core {

Object::Object(Object* parent)
    : _d(std::make_shared<priv::ObjectData>())
{
    set_parent(parent);
}

Object::~Object()
{
    // detach from the parent
    set_parent(nullptr);

    // delete all the children
    while (!_d->children.empty()) {
        // child object will remove itself from our list in the destructor
        delete _d->children.back();
    }

    // release private data and invalidate token
    _d.reset();

    // emit the `destroyed` signal
    // the token is already gone and no signal can be delivered back to this
    // object.
    destroyed.emit();
}

void Object::delete_later()
{
    assert(event_loop());

    auto tok = token();
    event_loop()->post([this, tok]() -> void {
        if (auto alive = tok.lock()) {
            delete this;
        }
    });
}

auto Object::token() const -> std::weak_ptr<priv::ObjectToken>
{
    return _d;
}

auto Object::thread_ctx() const -> ThreadCtx const*
{
    return _d->thread_ctx;
}

auto Object::parent() const -> Object*
{
    return _d->parent;
}

auto Object::children() const -> std::vector<Object*> const&
{
    return _d->children;
}

auto Object::event_loop() const -> EventLoop*
{
    return _d->event_loop;
}

void Object::set_event_loop(EventLoop* event_loop)
{
    _d->event_loop = event_loop;
}

auto Object::set_parent(Object* parent) -> bool
{
    assert(parent != this);

    if (_d->parent == parent) {
        return true; // already the parent
    }

    assert(!parent || parent->thread_ctx() == thread_ctx());
    if (parent && parent->thread_ctx() != thread_ctx()) {
        return false; // parent is in a different thread
    }

    if (_d->parent) {
        _d->parent->_d->remove_child(this);
    }
    _d->parent = parent;
    if (parent) {
        set_event_loop(parent->event_loop());
        parent->_d->add_child(this);
    }

    return true;
}

auto Object::move_to_thread(EventThread* event_thread) -> bool
{
    if (_d->parent) {
        return false; // object must not have a parent
    }
    if (thread_ctx() != ThreadCtx::current()) {
        return false; // can only be called from the thread the object was created in
    }
    if (event_loop() && ThreadCtx::current()->event_loop() &&
        (event_loop()->thread_ctx() != ThreadCtx::current()->event_loop()->thread_ctx())) {
        return false; // can only be called if the current event loop is not set or is
                      // running in the same thread context
    }

    // change the event loop for this object
    set_event_loop(event_thread ? event_thread->as_event_loop() : nullptr);

    // move all the children to the new event loop
    for (auto& o : _d->children) {
        o->move_to_thread(this, event_thread);
    }

    return true;
}

void Object::move_to_thread(Object* parent, EventThread* event_thread)
{
    // change the event loop for this object
    set_event_loop(event_thread ? event_thread->as_event_loop() : nullptr);

    // move all the children to the new event loop
    for (auto& o : _d->children) {
        o->move_to_thread(this, event_thread);
    }
}

} // namespace jb::core
