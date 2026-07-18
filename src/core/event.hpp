/**
 * @file event.hpp
 * @brief Defines the base type for events dispatched to `Object` instances.
 *
 * `Event` carries a numeric type identifier and an accepted state. Events are
 * ignored by default; a handler can call `accept()` when it has handled the
 * event, or `ignore()` to leave it available for other handlers. The
 * `None` and `CoreBase` identifiers are reserved by the library, while user
 * event types should start at `User` or higher.
 *
 * Define application-specific events by deriving from `Event` and passing a
 * unique type identifier to the base constructor:
 *
 * \code{.cpp}
 * class DataEvent : public jb::core::Event {
 * public:
 *     static constexpr Type TypeId = User + 1;
 *
 *     explicit DataEvent(int value)
 *         : Event(TypeId)
 *         , value(value)
 *     {}
 *
 *     int value;
 * };
 * \endcode
 *
 * A dispatcher can inspect the type, process the matching event, and mark it
 * as accepted:
 *
 * \code{.cpp}
 * void handle(jb::core::Event& event)
 * {
 *     if (event.type() == DataEvent::TypeId) {
 *         auto& data = static_cast<DataEvent&>(event);
 *         // Process data.value.
 *         event.accept();
 *     }
 * }
 * \endcode
 */
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
