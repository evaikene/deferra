#pragma once

#include <memory>

namespace df::core {

namespace priv {
struct ObjectData;
} // namespace priv

/// Base for all the objects
class Object {
public:

    /// Constructor
    /// @param[in] parent Optional parent
    Object(Object* parent = nullptr);

    /// Destructor
    virtual ~Object();

    /// Objects are not copyable nor movable
    Object(Object const&)                    = delete;
    Object(Object&&)                         = delete;
    auto operator=(Object const&) -> Object& = delete;
    auto operator=(Object&&) -> Object&      = delete;

    /// Sets or changes the parent
    /// @param[in] parent New parent
    void set_parent(Object* parent);

private:
    
    friend struct priv::ObjectData;

    /// Optional object data d-ptr
    std::unique_ptr<priv::ObjectData> _d;
};

} // namespace df::core
