#include "object.hpp"
#include "object_priv.hpp"

#include "event.hpp"
#include "event_loop.hpp"
#include "event_thread.hpp"
#include "thread_context.hpp"

#include <algorithm>
#include <mutex>

namespace jb::core {

Object::Object(Object* parent)
    : _d(new priv::ObjectPrivate())
{
    init_common(parent);
}

Object::Object(priv::ObjectPrivate& dd, Object* parent)
    : _d(&dd)
{
    init_common(parent);
}

void Object::init_common(Object* parent)
{
    _d->event_loop           = parent ? parent->event_loop() : EventLoop::current();
    _d->lifetime->event_loop = _d->event_loop;

    if (parent) {
        _d->parent = parent;
        parent->_d->children.push_back(this);
    }
}

Object::~Object()
{
    _d->deletion_committed = true;

    // --- 1. mark this object as dead
    {
        std::scoped_lock lock{_d->lifetime->event_loop_mx};
        _d->lifetime->alive.store(false, std::memory_order_release);
        _d->lifetime->event_loop = nullptr;
    }

    // --- 2. deactivate all connections where this Object is the receiver.
    //        This prevents slots from firing on a half-destroyed object.
    {
        std::scoped_lock lock{_d->connections_mx};
        for (auto& weak : _d->connections) {
            if (auto c = weak.lock()) {
                c->deactivate();
            }
        }
        _d->connections.clear();
    }

    //--- 3. emit the `destroyed` signal
    emit(destroyed);

    //--- 4. recursively delete all children
    {
        std::vector<Object*> children;
        children.swap(_d->children);

        // A child destructor can try to move a later sibling. Claim the entire
        // list before any destructor runs, even for children still linked here.
        for (auto* child : children) {
            child->_d->deletion_committed = true;
        }

        for (auto* child : children) {
            child->_d->parent = nullptr;
            delete child;
        }
    }

    //--- 5. unlink from parent
    if (_d->parent) {
        auto& siblings = _d->parent->_d->children;
        auto  it       = std::ranges::find(siblings, this);
        if (it != siblings.end()) {
            siblings.erase(it);
        }
    }

    //--- 6. release private data
    delete _d;
}

void Object::delete_later()
{
    auto             lifetime = _d->lifetime;
    std::scoped_lock lock{lifetime->event_loop_mx};
    if (!lifetime->alive.load(std::memory_order_acquire)) {
        return;
    }

    auto* loop = lifetime->event_loop;
    if (!loop) {
        log_fatal("Object::delete_later: object must have an event loop");
        return;
    }

    if (lifetime->delete_later_pending.exchange(true, std::memory_order_acq_rel)) {
        return; // already scheduled for deletion
    }

    if (!loop->defer_delete(this, lifetime)) {
        lifetime->delete_later_pending.store(false, std::memory_order_release);
        log_error("Object::delete_later: event-loop wakeup failed; object remains alive");
    }
}

auto Object::parent() const -> Object*
{
    return _d->parent;
}

auto Object::set_parent(Object* parent) -> bool
{
    // Core may already have cleared the parent link for recursive deletion.
    if (_d->deletion_committed) {
        log_fatal("Object::set_parent: this object is committed to deletion");
        return false;
    }

    auto* old_parent = _d->parent;
    if (parent == old_parent) {
        return true;
    }

    if (parent) {
        if (thread_ctx() != parent->thread_ctx()) {
            log_fatal("Object::set_parent: parent must live in the same thread");
            return false;
        }
        if (parent->_d->deletion_committed) {
            log_fatal("Object::set_parent: new parent is committed to deletion");
            return false;
        }

        for (auto* ancestor = parent; ancestor; ancestor = ancestor->_d->parent) {
            if (ancestor == this) {
                log_fatal("Object::set_parent: parent must be outside this object's subtree");
                return false;
            }
        }
    }

    auto old_position = std::vector<Object*>::iterator{};
    if (old_parent) {
        auto& siblings = old_parent->_d->children;
        old_position   = std::ranges::find(siblings, this);
        if (old_position == siblings.end()) {
            log_fatal("Object::set_parent: current parent does not list this child");
            return false;
        }
    }

    // Prepare the new owner's slot before releasing the old ownership.
    if (parent) {
        parent->_d->children.push_back(this);
    }
    if (old_parent) {
        old_parent->_d->children.erase(old_position);
    }
    _d->parent = parent;

    if (parent) {
        move_to_event_loop(parent->event_loop());
    }

    return true;
}

auto Object::event(Event& /* event */) -> bool
{
    return false;
}

auto Object::children() const noexcept -> std::vector<Object*> const&
{
    return _d->children;
}

auto Object::thread_ctx() const noexcept -> ThreadCtx const*
{
    return _d->event_loop ? _d->event_loop->thread_ctx() : nullptr;
}

auto Object::event_loop() const noexcept -> EventLoop*
{
    return _d->event_loop;
}

auto Object::move_to_thread(EventThread* event_thread) -> bool
{
    // object must not have a parent (i.e. must be a root object)
    if (_d->parent) {
        log_fatal("Object::move_to_thread: object must not have a parent");
        return false;
    }

    // object must live in the current thread where the move is initiated
    if (thread_ctx() && thread_ctx() != ThreadCtx::current()) {
        log_fatal("Object::move_to_thread: object must live in the current thread");
        return false;
    }

    return move_to_thread_impl(event_thread);
}

auto Object::d_ptr() const noexcept -> void*
{
    return _d;
}

void Object::register_connection(std::shared_ptr<priv::ConnectionBase> const& conn)
{
    std::scoped_lock lock{_d->connections_mx};

    // opportunistically sweep expired entries to keep the list compact
    auto const range = std::ranges::remove_if(_d->connections, [](auto const& w) -> bool { return w.expired(); });
    _d->connections.erase(range.begin(), range.end());

    _d->connections.emplace_back(conn);
}

auto Object::lifetime() const -> std::weak_ptr<priv::ObjectLifetime>
{
    return _d->lifetime;
}

void Object::post_event_delivery(EventLoop*                          event_loop,
                                 Object*                             receiver,
                                 std::weak_ptr<priv::ObjectLifetime> lifetime,
                                 Task                                delivery)
{
    if (!event_loop->post_event_delivery(receiver, std::move(lifetime), std::move(delivery))) {
        log_error("Object::post_event_delivery: event-loop wakeup failed; dropping delivery");
    }
}

auto Object::move_to_thread_impl(EventThread* event_thread) -> bool
{

    auto* event_loop = event_thread->as_event_loop();
    _d->event_loop   = event_loop;
    {
        std::scoped_lock lock{_d->lifetime->event_loop_mx};
        _d->lifetime->event_loop = event_loop;
    }

    // recursively change all children to use the new event loop
    for (auto* child : _d->children) {
        child->move_to_thread_impl(event_thread);
    }

    return true;
}

void Object::move_to_event_loop(EventLoop* new_loop)
{
    _d->event_loop = new_loop;
    {
        std::scoped_lock lock{_d->lifetime->event_loop_mx};
        _d->lifetime->event_loop = new_loop;
    }

    for (auto* child : _d->children) {
        child->move_to_event_loop(new_loop);
    }
}

} // namespace jb::core
