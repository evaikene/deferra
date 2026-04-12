#include "object.hpp"
#include "object_priv.hpp"

#include <cassert>

namespace df::core {

Object::Object(Object* parent)
    : _d(std::make_unique<priv::ObjectData>())
{
    if (parent) {
        _d->parent = parent;
        _d->parent->_d->add_child(this);
    }
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

} // namespace df::core
