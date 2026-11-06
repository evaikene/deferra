#pragma once

#include <phpcxx/object_binding.hpp>
#include <phpcxx/value.hpp>

// Shared fixtures for native Embed tests and independent module consumers.
namespace phpcxx_test {

struct Counts {
    int constructed = 0;
    int destroyed   = 0;
    int live        = 0;
};

auto counts() noexcept -> Counts;

// Explicitly aligned, non-standard-layout state exercises byte storage rather
// than relying on offsetof(T) or a separate heap allocation for the native value.
class alignas(ZEND_MM_ALIGNMENT) Native {
public:
    Native(zend_long number, php::Value reference) noexcept;
    Native(Native&& other) noexcept;
    virtual ~Native() noexcept;

    Native(Native const&)                    = delete;
    auto operator=(Native const&) -> Native& = delete;
    auto operator=(Native&&) -> Native&      = delete;

    auto trace(php::GcVisitor& visitor) const noexcept -> void;

    auto number() const noexcept -> zend_long { return _number; }

    auto set_number(zend_long number) noexcept -> void { _number = number; }

    auto reference() const& noexcept -> php::ValueView { return _reference.view(); }

    auto set_reference(php::Value value) noexcept -> void;

private:
    zend_long  _number;
    php::Value _reference;
};

struct ClonePolicy : php::ObjectBindingPolicy<Native> {
    static constexpr bool cloneable = true;
    static auto           clone(Native const& source) noexcept -> php::Result<Native>;
};

using Binding       = php::ObjectBinding<Native, ClonePolicy>;
using LockedBinding = php::ObjectBinding<Native>;

// A binding must not require moves when cloning is disabled.
struct Immobile {
    Immobile() noexcept                          = default;
    Immobile(Immobile const&)                    = delete;
    Immobile(Immobile&&)                         = delete;
    auto operator=(Immobile const&) -> Immobile& = delete;
    auto operator=(Immobile&&) -> Immobile&      = delete;
    ~Immobile() noexcept                         = default;

    auto trace([[maybe_unused]] php::GcVisitor& visitor) const noexcept -> void {}
};

using ImmobileBinding = php::ObjectBinding<Immobile>;

auto register_classes() noexcept -> zend_result;

} // namespace phpcxx_test
