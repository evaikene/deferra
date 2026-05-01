#include "object.hpp"
#include "object_priv.hpp"

#include "event_loop.hpp"
#include "thread_context.hpp"

#include <algorithm>
#include <cassert>

namespace jb::core {

Object::Object(Object* parent)
    : _d(new priv::ObjectData())
{
    init(parent);
}

Object::Object(priv::ObjectData& d, Object* parent)
    : _d(&d)
{
    init(parent);
}

void Object::init(Object* parent)
{
    _d->thread_ctx = ThreadCtx::current();
    _d->event_loop = EventLoop::current();

    if (parent) {
        _d->parent = parent;
        parent->_d->children.push_back(this);
    }
}

Object::~Object()
{
    //--- 1. deactivate all connections where this Object is the receiver
    {
        std::lock_guard<std::mutex> lock{_d->connextions_mx};
        for (auto& weak : _d->connections) {
            if (auto c = weak.lock()) {
                c->deactivate();
            }
        }
        _d->connections.clear();
    }

    //--- 2. emit the `destroyed` signal
    emit(destroyed);

    //--- 3. recursively delete all children
    {
        std::vector<Object*> children;
        children.swap(_d->children);

        for (auto* child : children) {
            child->_d->parent = nullptr;
            delete child;
        }
    }

    //--- 4. unlink from parent
    if (_d->parent) {
        auto& siblings = _d->parent->_d->children;
        auto  it       = std::ranges::find(siblings, this);
        if (it != siblings.end()) {
            siblings.erase(it);
        }
    }

    //--- 5. release private data
    delete _d;
}

void Object::delete_later()
{
    assert(event_loop());
}

auto Object::parent() const -> Object*
{
    return _d->parent;
}

auto Object::set_parent(Object* new_parent) -> bool
{
    if (new_parent == _d->parent) {
        return true; // no-op
    }

    // new parent must live in the same thread
    assert(!new_parent ||
           (_d->thread_ctx == new_parent->_d->thread_ctx) && "Object::set_parent: parent must live in the same thread");
    if (new_parent && (_d->thread_ctx != new_parent->_d->thread_ctx)) {
        log_error("Object::set_parent: parent must live in the same thread");
        return false;
    }

    // unlink from current parent
    if (_d->parent) {
        auto& siblings = _d->parent->_d->children;
        auto  it       = std::ranges::find(siblings, this);
        if (it != siblings.end()) {
            siblings.erase(it);
        }
    }

    _d->parent = new_parent;

    if (new_parent) {
        new_parent->_d->children.push_back(this);
    }
}

auto Object::children() const -> std::vector<Object*> const&
{
    return _d->children;
}

auto Object::thread_ctx() const noexcept -> ThreadCtx const*
{
    return _d->thread_ctx;
}

auto Object::event_loop() const noexcept -> EventLoop*
{
    return _d->event_loop;
}

auto Object::d_ptr() noexcept -> void*
{
    return _d;
}

auto Object::d_ptr() const noexcept -> void const*
{
    return _d;
}

void Object::register_connection(std::shared_ptr<priv::ConnectionBase> const& conn)
{
    std::lock_guard<std::mutex> lock{_d->connextions_mx};

    // opportunistically sweep expired entries to keep the list compact
    std::erase_if(_d->connections, [](std::weak_ptr<priv::ConnectionBase> const& w) -> bool {
        return w.expired();
    });

    _d->connections.emplace_back(conn);
}

} // namespace jb::core
