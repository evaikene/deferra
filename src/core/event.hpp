#pragma once

namespace jb::core {

/// Base type for events dispatched to Object instances.
class Event {
public:

    using Type = int;

    static constexpr Type None{0};
    static constexpr Type CoreBase{1};
    static constexpr Type User{1000};

    /// Constructs an ignored event of the specified type.
    /// @param[in] type Event type
    explicit Event(Type type)
        : _type(type)
    {}

    /// Destructor
    virtual ~Event() = default;

    /// Returns the event type.
    [[nodiscard]] auto type() const noexcept -> Type { return _type; }

    /// Marks the event as accepted.
    void accept() noexcept { _accepted = true; }

    /// Marks the event as ignored.
    void ignore() noexcept { _accepted = false; }

    /// Returns whether the event has been accepted.
    [[nodiscard]] auto accepted() const noexcept -> bool { return _accepted; }

private:

    Type _type;
    bool _accepted{false};
};

} // namespace jb::core
