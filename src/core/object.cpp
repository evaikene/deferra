#include "object.hpp"
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

auto Object::event_loop() const -> EventLoop*
{
    return _d->thread_ctx->event_loop();
}

void Object::set_parent(Object* parent)
{
    assert(parent != this);

    if (_d->parent == parent) {
        return;
    }
    if (_d->parent) {
        _d->parent->_d->remove_child(this);
    }
    _d->parent = parent;
    if (parent) {
        parent->_d->add_child(this);
    }
}

} // namespace jb::core
