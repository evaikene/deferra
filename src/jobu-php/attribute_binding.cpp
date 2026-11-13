#include "attribute_binding.hpp"
#include "client_binding.hpp"

#include <jb/jobu/attribute_registry.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/conversion.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>

#include <Zend/zend_enum.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace jb::php {
namespace {

auto registry() -> jobu::StandardAttributeRegistry const&
{
    static jobu::StandardAttributeRegistry const instance;
    return instance;
}

auto class_entry(std::string_view name) -> zend_class_entry*
{
    return static_cast<zend_class_entry*>(zend_hash_str_find_ptr(EG(class_table), name.data(), name.size()));
}

auto invalid(std::string message = "Invalid attribute value") -> ::php::Error
{
    return {.kind = ::php::ErrorKind::Value, .message = std::move(message)};
}

// Shared storage contains no Zend references. Each wrapper owns its own traced
// edges: a view retains its parent, and a root retains its attached resource.
// Thus native shared ownership never hides PHP cycles from Zend's collector.
struct Layer {
    jobu::AttributeSet     values;
    std::function<void()>  changed;
    ConfigurationEditCheck editable;
};

struct Configuration {
    explicit Configuration(jobu::AttributeScope scope, std::optional<jobu::JobType> runner = {}) noexcept
        : layer(std::make_shared<Layer>())
        , scope(scope)
        , runner(runner)
    {}

    auto trace(::php::GcVisitor& visitor) const noexcept -> void
    {
        visitor.add(owner.view());
        visitor.add(cli.view());
        visitor.add(http.view());
    }

    auto visible(jobu::AttributeDefinition const& definition) const noexcept -> bool
    {
        return !runner || jobu::attribute_applies_to(definition, *runner);
    }

    std::shared_ptr<Layer>       layer;
    jobu::AttributeScope         scope;
    std::optional<jobu::JobType> runner;
    ::php::Value                 owner;
    ::php::Value                 cli;
    ::php::Value                 http;
};

auto visible_values(Configuration const& value) -> jobu::AttributeSet
{
    jobu::AttributeSet result;
    for (auto const& [name, item] : value.layer->values) {
        if (value.visible(*registry().find(name))) {
            result.emplace(name, item);
        }
    }
    return result;
}

// The standard registry admits only scalar values and string lists. Compare
// directly without allocating, invoking PHP, or serializing during Zend compare.
auto compare_value(jobu::AttributeValue const& left, jobu::AttributeValue const& right) noexcept -> int
{
    if (left.data.index() != right.data.index()) {
        return left.data.index() < right.data.index() ? -1 : 1;
    }
    return std::visit(
        [&](auto const& value) -> int {
            using T           = std::decay_t<decltype(value)>;
            auto const& other = std::get<T>(right.data);
            if constexpr (std::is_same_v<T, jobu::AttributeValue::List>) {
                auto l = value.begin();
                auto r = other.begin();
                for (; l != value.end() && r != other.end(); ++l, ++r) {
                    if (auto order = compare_value(*l, *r); order != 0) {
                        return order;
                    }
                }
                if (value.size() < other.size()) {
                    return -1;
                }
                return value.size() > other.size() ? 1 : 0;
            }
            else if constexpr (std::is_same_v<T, jobu::AttributeValue::Map> || std::is_same_v<T, core::ByteBuffer>) {
                // These alternatives cannot enter a standard configuration.
                return 0;
            }
            else {
                if (value < other) {
                    return -1;
                }
                return value > other ? 1 : 0;
            }
        },
        left.data);
}

auto compare_configuration(Configuration const& left, Configuration const& right) noexcept -> int
{
    auto       l  = left.layer->values.begin();
    auto       r  = right.layer->values.begin();
    auto const le = left.layer->values.end();
    auto const re = right.layer->values.end();
    for (;;) {
        while (l != le && !left.visible(*registry().find(l->first))) {
            ++l;
        }
        while (r != re && !right.visible(*registry().find(r->first))) {
            ++r;
        }
        if (l == le) {
            return r == re ? 0 : -1;
        }
        if (r == re) {
            return 1;
        }
        if (auto order = l->first.compare(r->first); order != 0) {
            return order;
        }
        if (auto order = compare_value(l->second, r->second); order != 0) {
            return order;
        }
        ++l;
        ++r;
    }
}

struct ConfigurationPolicy : ::php::ObjectBindingPolicy<Configuration> {
    static constexpr bool cloneable = true;

    static auto clone(Configuration const& source) noexcept -> ::php::Result<Configuration>
    {
        // A cloned typed queue view becomes an ordinary detached job layer.
        auto          scope = source.runner ? jobu::AttributeScope::Job : source.scope;
        Configuration result(scope, source.runner);
        result.layer->values = visible_values(source);
        return result;
    }

    static auto compare(Configuration const& left, Configuration const& right) noexcept -> int
    {
        return compare_configuration(left, right);
    }
};

static_assert(ConfigurationPolicy::cloneable, "Mutable configuration must support detached cloning");

using Binding = ::php::ObjectBinding<Configuration, ConfigurationPolicy>;

auto configuration(::php::ValueView input) -> ::php::Result<Configuration*>
{
    auto value = input.dereference();
    if (!value.is_object()) {
        return ::php::Error{.kind = ::php::ErrorKind::Type, .message = "Expected configuration object"};
    }
    return Binding::get(Z_OBJ_P(value.get()));
}

auto definition(Configuration const& config, std::string_view name) -> ::php::Result<jobu::AttributeDefinition const*>
{
    auto const* result = registry().find(name);
    if (!result || !result->scopes.test(config.scope) || !config.visible(*result)) {
        return invalid("Unknown or inapplicable attribute name");
    }
    return result;
}

auto from_php(::php::ValueView input, jobu::AttributeDefinition const& def) -> ::php::Result<jobu::AttributeValue>
{
    using jobu::AttributeValue;
    auto value = input.dereference();
    switch (def.type) {
        case jobu::AttributeType::Boolean: {
            auto item = ::php::as_bool(value);
            if (!item) {
                return item.error();
            }
            return AttributeValue{.data = item.value()};
        }
        case jobu::AttributeType::Integer: {
            auto item = ::php::as_integer(value);
            if (!item) {
                return item.error();
            }
            return AttributeValue{.data = static_cast<std::int64_t>(item.value())};
        }
        case jobu::AttributeType::Number: {
            if (value.is_long()) {
                return AttributeValue{.data = static_cast<double>(value.get_long())};
            }
            auto item = ::php::as_double(value);
            if (!item) {
                return item.error();
            }
            return AttributeValue{.data = item.value()};
        }
        case jobu::AttributeType::String: {
            auto item = ::php::as_string(value);
            if (!item) {
                return item.error();
            }
            return AttributeValue{.data = std::string(item.value().bytes())};
        }
        case jobu::AttributeType::Duration: {
            auto item = duration_milliseconds(value);
            if (!item) {
                return item.error();
            }
            // Check before scaling milliseconds into the native duration unit.
            using Milliseconds = std::chrono::milliseconds;
            if (item.value() < std::chrono::duration_cast<Milliseconds>(core::Duration::min()) ||
                item.value() > std::chrono::duration_cast<Milliseconds>(core::Duration::max())) {
                return invalid("Duration exceeds the native attribute range");
            }
            return AttributeValue{.data = std::chrono::duration_cast<core::Duration>(item.value())};
        }
        case jobu::AttributeType::List: {
            auto array = ::php::as_array(value);
            if (!array) {
                return array.error();
            }
            if (!zend_array_is_list(const_cast<zend_array*>(array.value().get()))) {
                return invalid("Attribute requires a list of strings");
            }
            AttributeValue::List items;
            for (auto entry : array.value()) {
                auto item = ::php::as_string(entry.value);
                if (!item) {
                    return item.error();
                }
                items.push_back({.data = std::string(item.value().bytes())});
            }
            return AttributeValue{.data = std::move(items)};
        }
        default:
            return invalid("Unsupported attribute representation");
    }
}

auto to_php(jobu::AttributeValue const& input) -> ::php::Result<::php::Value>
{
    zval slot{};
    if (auto const* value = std::get_if<bool>(&input.data)) {
        ZVAL_BOOL(&slot, *value);
    }
    else if (auto const* value = std::get_if<std::int64_t>(&input.data)) {
        ZVAL_LONG(&slot, *value);
    }
    else if (auto const* value = std::get_if<double>(&input.data)) {
        ZVAL_DOUBLE(&slot, *value);
    }
    else if (auto const* value = std::get_if<std::string>(&input.data)) {
        ZVAL_STRINGL(&slot, value->data(), value->size());
    }
    else if (auto const* value = std::get_if<core::Duration>(&input.data)) {
        return duration_to_php(std::chrono::duration_cast<std::chrono::milliseconds>(*value));
    }
    else if (auto const* values = std::get_if<jobu::AttributeValue::List>(&input.data)) {
        array_init(&slot);
        auto owner = ::php::Value::adopt(slot);
        for (auto const& value : *values) {
            auto converted = to_php(value);
            if (!converted) {
                return converted.error();
            }
            auto item = converted.value().release();
            add_next_index_zval(const_cast<zval*>(owner.get()), &item);
        }
        return owner;
    }
    else {
        return invalid("Unsupported attribute representation");
    }
    return ::php::Value::adopt(slot);
}

auto notify(Configuration const& value) -> void
{
    if (value.layer->changed) {
        value.layer->changed();
    }
}

auto write(Configuration& config, std::string_view name, ::php::ValueView input) -> ::php::Result<void>
{
    auto def = definition(config, name);
    if (!def) {
        return def.error();
    }
    auto value = from_php(input, *def.value());
    if (!value) {
        return value.error();
    }
    auto checked = registry().validate(name, value.value(), config.scope);
    if (!checked) {
        return invalid(checked.error().message);
    }

    auto allowed = check_configuration_edit(config.layer->editable);
    if (!allowed) {
        return allowed.error();
    }
    auto found = config.layer->values.find(name);
    if (found != config.layer->values.end() && compare_value(found->second, value.value()) == 0) {
        return {};
    }
    config.layer->values.insert_or_assign(std::string(name), std::move(value).value());
    notify(config);
    return {};
}

auto validate_layer(Configuration const& config, jobu::AttributeSet const& values) -> ::php::Result<void>
{
    for (auto const& [name, value] : values) {
        auto def = definition(config, name);
        if (!def) {
            return def.error();
        }
    }
    // Existing public codec checks partial cross-field relationships without
    // materializing defaults or depending on daemon/persistence implementation.
    auto checked = jobu::attribute_set_to_json(values, registry(), config.scope);
    if (!checked) {
        return invalid(checked.error().message);
    }
    return {};
}

auto construct(zend_execute_data* frame, bool defaults) noexcept -> void
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    std::optional<jobu::JobType> runner;
    if (instanceof_function(Z_OBJCE(frame->This), class_entry("jobu\\clijobattributes"))) {
        runner = jobu::JobType::Cli;
    }
    if (instanceof_function(Z_OBJCE(frame->This), class_entry("jobu\\httpjobattributes"))) {
        runner = jobu::JobType::Http;
    }
    auto result = Binding::construct(Z_OBJ(frame->This),
                                     defaults ? jobu::AttributeScope::QueueDefault : jobu::AttributeScope::Job,
                                     runner);
    if (!result) {
        ::php::raise(result.error());
    }
}

enum class Access : std::uint8_t {
    Get,
    Has,
    Set,
    Clear
};

// Typed methods use Zend's normal weak/strict scalar argument rules; the generic
// mixed setter remains definition-directed and never coerces strings to numbers.
auto typed_scalar(::php::Arguments const& arguments, jobu::AttributeType type) -> ::php::Result<::php::Value>
{
    zval slot{};
    switch (type) {
        case jobu::AttributeType::Boolean: {
            auto parsed = arguments.boolean(1);
            if (!parsed) {
                return parsed.error();
            }
            ZVAL_BOOL(&slot, parsed.value());
            break;
        }
        case jobu::AttributeType::Integer: {
            auto parsed = arguments.integer(1);
            if (!parsed) {
                return parsed.error();
            }
            ZVAL_LONG(&slot, parsed.value());
            break;
        }
        case jobu::AttributeType::Number: {
            auto parsed = arguments.floating(1);
            if (!parsed) {
                return parsed.error();
            }
            ZVAL_DOUBLE(&slot, parsed.value());
            break;
        }
        case jobu::AttributeType::String: {
            auto parsed = arguments.string(1);
            if (!parsed) {
                return parsed.error();
            }
            auto text = parsed.value().bytes();
            ZVAL_STRINGL(&slot, text.data(), text.size());
            break;
        }
        default:
            return ::php::Value::retain(arguments.get(1).dereference());
    }
    return ::php::Value::adopt(slot);
}

auto access(zend_execute_data* frame,
            zval*              output,
            Access             action,
            std::string_view   typed_name = {},
            bool               retry_enum = false) noexcept -> void
{
    ::php::Arguments args(frame);
    bool const       typed    = !typed_name.empty();
    auto const       expected = (typed ? 0U : 1U) + (action == Access::Set ? 1U : 0U);
    auto             count    = args.count(expected, expected);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    std::string_view name = typed_name;
    if (!typed) {
        auto text = args.string(1);
        if (!text) {
            ::php::raise(text.error());
            return;
        }
        name = text.value().bytes();
    }
    auto& config = *native.value();
    auto  def    = definition(config, name);
    if (!def) {
        ::php::raise(def.error());
        return;
    }

    if (action == Access::Set) {
        auto         input = args.get(typed ? 1 : 2);
        ::php::Value parsed_owner;
        if (typed && !retry_enum) {
            auto parsed = typed_scalar(args, def.value()->type);
            if (!parsed) {
                ::php::raise(parsed.error());
                return;
            }
            parsed_owner = std::move(parsed).value();
            input        = parsed_owner.view();
        }
        if (retry_enum) {
            auto object = ::php::as_object(input, class_entry("jobu\\retrymode"));
            if (!object) {
                ::php::raise(object.error());
                return;
            }
            input = ::php::ValueView(zend_enum_fetch_case_value(const_cast<zend_object*>(object.value())));
        }
        auto result = write(config, name, input);
        if (!result) {
            ::php::raise(result.error());
        }
        return;
    }
    auto found = config.layer->values.find(name);
    if (action == Access::Clear) {
        auto allowed = check_configuration_edit(config.layer->editable);
        if (!allowed) {
            ::php::raise(allowed.error());
            return;
        }
        if (found != config.layer->values.end()) {
            config.layer->values.erase(found);
            notify(config);
        }
        return;
    }
    if (action == Access::Has) {
        ZVAL_BOOL(output, found != config.layer->values.end());
        return;
    }
    if (found == config.layer->values.end()) {
        ZVAL_NULL(output);
        return;
    }
    if (retry_enum) {
        auto const& text = std::get<std::string>(found->second.data);
        auto*       object =
            zend_enum_get_case_cstr(class_entry("jobu\\retrymode"), text == "blocking" ? "Blocking" : "Reschedule");
        ZVAL_OBJ_COPY(output, object);
        return;
    }
    auto result = to_php(found->second);
    if (!result) {
        ::php::raise(result.error());
        return;
    }
    *output = result.value().release();
}

auto view(zend_execute_data* frame, zval* output, jobu::JobType runner) noexcept -> void
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto native = Binding::get(Z_OBJ(frame->This));
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    auto& root   = *native.value();
    auto& cached = runner == jobu::JobType::Cli ? root.cli : root.http;
    if (!cached.view().is_initialized()) {
        zval slot{};
        object_init_ex(
            &slot,
            class_entry(runner == jobu::JobType::Cli ? "jobu\\clijobattributes" : "jobu\\httpjobattributes"));
        auto owner   = ::php::Value::adopt(slot);
        auto created = Binding::construct(Z_OBJ_P(owner.get()), root.scope, std::optional{runner});
        if (!created) {
            ::php::raise(created.error());
            return;
        }
        created.value()->layer = root.layer;
        created.value()->owner = ::php::Value::retain(::php::ValueView(&frame->This));
        cached                 = std::move(owner);
    }
    ZVAL_COPY(output, cached.get());
}

} // namespace

auto bind_attribute_classes(zend_class_entry* attributes,
                            zend_class_entry* cli,
                            zend_class_entry* http,
                            zend_class_entry* defaults) -> bool
{
    for (auto* entry : {attributes, cli, http, defaults}) {
        entry->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
        if (!Binding::bind(entry)) {
            return false;
        }
    }
    return true;
}

auto attributes_from_php(::php::ValueView value) -> ::php::Result<jobu::AttributeSet>
{
    auto config = configuration(value);
    if (!config) {
        return config.error();
    }
    auto result = visible_values(*config.value());
    auto valid  = validate_layer(*config.value(), result);
    if (!valid) {
        return valid.error();
    }
    return result;
}

auto merge_attributes(zend_object* target, ::php::ValueView source) -> ::php::Result<void>
{
    if (!instanceof_function(target->ce, class_entry("jobu\\jobattributes"))) {
        return invalid("Merge target must be JobAttributes");
    }
    auto object = ::php::as_object(source, class_entry("jobu\\jobattributes"));
    if (!object) {
        return object.error();
    }
    auto config = Binding::get(target);
    if (!config) {
        return config.error();
    }
    auto supplied = attributes_from_php(source);
    if (!supplied) {
        return supplied.error();
    }
    auto candidate = visible_values(*config.value());
    for (auto const& [name, value] : supplied.value()) {
        candidate.insert_or_assign(name, value);
    }
    auto valid = validate_layer(*config.value(), candidate);
    if (!valid) {
        return valid.error();
    }

    auto allowed = check_configuration_edit(config.value()->layer->editable);
    if (!allowed) {
        return allowed.error();
    }
    bool changed = false;
    // Commit only supplied keys, preserving the other runner in a queue view.
    for (auto& [name, value] : supplied.value()) {
        auto found = config.value()->layer->values.find(name);
        if (found == config.value()->layer->values.end() || compare_value(found->second, value) != 0) {
            config.value()->layer->values.insert_or_assign(name, std::move(value));
            changed = true;
        }
    }
    if (changed) {
        notify(*config.value());
    }
    return {};
}

auto job_attributes_to_php(jobu::JobType runner, jobu::AttributeSet values) -> ::php::Result<::php::Value>
{
    zval        slot{};
    auto const* name = runner == jobu::JobType::Cli ? "jobu\\clijobattributes" : "jobu\\httpjobattributes";
    object_init_ex(&slot, class_entry(name));
    auto owner  = ::php::Value::adopt(slot);
    auto native = Binding::construct(Z_OBJ_P(owner.get()), jobu::AttributeScope::Job, std::optional{runner});
    if (!native) {
        return native.error();
    }
    auto replacement = prepare_attributes_restore(Z_OBJ_P(owner.get()), std::move(values));
    if (!replacement) {
        return replacement.error();
    }
    std::move(replacement).value().commit();
    return owner;
}

auto defaults_to_php(jobu::AttributeSet values) -> ::php::Result<::php::Value>
{
    zval slot{};
    object_init_ex(&slot, class_entry("jobu\\queuedefaults"));
    auto owner  = ::php::Value::adopt(slot);
    auto native = Binding::construct(Z_OBJ_P(owner.get()), jobu::AttributeScope::QueueDefault);
    if (!native) {
        return native.error();
    }
    auto replacement = prepare_attributes_restore(Z_OBJ_P(owner.get()), std::move(values));
    if (!replacement) {
        return replacement.error();
    }
    std::move(replacement).value().commit();
    return owner;
}

auto replace_defaults(zend_object* target, ::php::ValueView source) -> ::php::Result<void>
{
    if (!instanceof_function(target->ce, class_entry("jobu\\queuedefaults"))) {
        return invalid("Replacement target must be QueueDefaults");
    }
    auto object = ::php::as_object(source, class_entry("jobu\\queuedefaults"));
    if (!object) {
        return object.error();
    }
    auto config = Binding::get(target);
    if (!config) {
        return config.error();
    }
    auto supplied = attributes_from_php(source);
    if (!supplied) {
        return supplied.error();
    }
    auto source_config = configuration(source);
    if (!source_config) {
        return source_config.error();
    }
    auto allowed = check_configuration_edit(config.value()->layer->editable);
    if (!allowed) {
        return allowed.error();
    }
    if (compare_configuration(*config.value(), *source_config.value()) == 0) {
        return {};
    }
    config.value()->layer->values = std::move(supplied).value();
    notify(*config.value());
    return {};
}

auto attribute_snapshot(::php::ValueView value) -> ::php::Result<jobu::AttributeSet>
{
    auto config = configuration(value);
    if (!config) {
        return config.error();
    }
    return visible_values(*config.value());
}

auto equal_attributes(jobu::AttributeSet const& left, jobu::AttributeSet const& right) -> bool
{
    if (left.size() != right.size()) {
        return false;
    }
    auto other = right.begin();
    for (auto const& [name, value] : left) {
        if (name != other->first || compare_value(value, other->second) != 0) {
            return false;
        }
        ++other;
    }
    return true;
}

auto changed_attributes(jobu::AttributeSet const& baseline, jobu::AttributeSet const& current)
    -> ::php::Result<jobu::AttributeSet>
{
    for (auto const& [name, value] : baseline) {
        if (!current.contains(name)) {
            return invalid("Job attributes cannot be removed");
        }
    }
    jobu::AttributeSet changes;
    for (auto const& [name, value] : current) {
        auto found = baseline.find(name);
        if (found == baseline.end() || compare_value(found->second, value) != 0) {
            changes.emplace(name, value);
        }
    }
    return changes;
}

auto prepare_attributes_restore(zend_object* target, jobu::AttributeSet values) -> ::php::Result<ConfigurationRestore>
{
    auto config = Binding::get(target);
    if (!config) {
        return config.error();
    }
    if (config.value()->scope == jobu::AttributeScope::QueueDefault && config.value()->runner) {
        return invalid("Restore the QueueDefaults root, not a typed view");
    }
    auto valid = validate_layer(*config.value(), values);
    if (!valid) {
        return valid.error();
    }
    return ConfigurationRestore(
        [target = config.value(), values = std::move(values)]() mutable { target->layer->values = std::move(values); });
}

auto attach_attributes(zend_object*           target,
                       ::php::ValueView       owner,
                       std::function<void()>  changed,
                       ConfigurationEditCheck editable) -> ::php::Result<void>
{
    auto config = Binding::get(target);
    if (!config) {
        return config.error();
    }
    if (!owner.dereference().is_object()) {
        return invalid("Configuration owner must be an object");
    }
    if (config.value()->owner.view().is_initialized() || config.value()->cli.view().is_initialized() ||
        config.value()->http.view().is_initialized()) {
        return invalid("Configuration is already attached or published");
    }
    config.value()->owner           = ::php::Value::retain(owner.dereference());
    config.value()->layer->changed  = std::move(changed);
    config.value()->layer->editable = std::move(editable);
    return {};
}

} // namespace jb::php

ZEND_METHOD(JobU_JobAttributes, __construct)
{
    (void)return_value;
    jb::php::construct(execute_data, false);
}

ZEND_METHOD(JobU_JobAttributes, get)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get);
}

ZEND_METHOD(JobU_JobAttributes, has)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Has);
}

ZEND_METHOD(JobU_JobAttributes, set)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set);
}

ZEND_METHOD(JobU_QueueDefaults, __construct)
{
    (void)return_value;
    jb::php::construct(execute_data, true);
}

ZEND_METHOD(JobU_QueueDefaults, get)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get);
}

ZEND_METHOD(JobU_QueueDefaults, has)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Has);
}

ZEND_METHOD(JobU_QueueDefaults, set)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set);
}

ZEND_METHOD(JobU_QueueDefaults, clear)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Clear);
}

ZEND_METHOD(JobU_JobAttributes, getTimeout)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "job.timeout");
}

ZEND_METHOD(JobU_JobAttributes, setTimeout)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "job.timeout");
}

ZEND_METHOD(JobU_JobAttributes, getOutputCapture)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "output.capture");
}

ZEND_METHOD(JobU_JobAttributes, setOutputCapture)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "output.capture");
}

ZEND_METHOD(JobU_JobAttributes, getRetryInitialDelay)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "retry.initial_delay");
}

ZEND_METHOD(JobU_JobAttributes, setRetryInitialDelay)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "retry.initial_delay");
}

ZEND_METHOD(JobU_JobAttributes, getRetryMaxDelay)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "retry.max_delay");
}

ZEND_METHOD(JobU_JobAttributes, setRetryMaxDelay)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "retry.max_delay");
}

ZEND_METHOD(JobU_JobAttributes, getRetryMaxAttempts)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "retry.max_attempts");
}

ZEND_METHOD(JobU_JobAttributes, setRetryMaxAttempts)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "retry.max_attempts");
}

ZEND_METHOD(JobU_JobAttributes, getRetryJitter)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "retry.jitter");
}

ZEND_METHOD(JobU_JobAttributes, setRetryJitter)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "retry.jitter");
}

ZEND_METHOD(JobU_JobAttributes, getRetryMultiplier)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "retry.multiplier");
}

ZEND_METHOD(JobU_JobAttributes, setRetryMultiplier)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "retry.multiplier");
}

ZEND_METHOD(JobU_JobAttributes, getRetryMode)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "retry.mode", true);
}

ZEND_METHOD(JobU_JobAttributes, setRetryMode)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "retry.mode", true);
}

ZEND_METHOD(JobU_JobAttributes, getRetryStrategy)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "retry.strategy");
}

ZEND_METHOD(JobU_JobAttributes, setRetryStrategy)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "retry.strategy");
}

ZEND_METHOD(JobU_CliJobAttributes, getRetryExitCodes)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "cli.retry_exit_codes");
}

ZEND_METHOD(JobU_CliJobAttributes, setRetryExitCodes)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "cli.retry_exit_codes");
}

ZEND_METHOD(JobU_CliJobAttributes, getTerminationGrace)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "cli.termination_grace");
}

ZEND_METHOD(JobU_CliJobAttributes, setTerminationGrace)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "cli.termination_grace");
}

ZEND_METHOD(JobU_CliJobAttributes, getStdoutLimit)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "output.stdout_limit");
}

ZEND_METHOD(JobU_CliJobAttributes, setStdoutLimit)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "output.stdout_limit");
}

ZEND_METHOD(JobU_CliJobAttributes, getStderrLimit)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "output.stderr_limit");
}

ZEND_METHOD(JobU_CliJobAttributes, setStderrLimit)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "output.stderr_limit");
}

ZEND_METHOD(JobU_HttpJobAttributes, getFollowRedirects)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "http.follow_redirects");
}

ZEND_METHOD(JobU_HttpJobAttributes, setFollowRedirects)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "http.follow_redirects");
}

ZEND_METHOD(JobU_HttpJobAttributes, getIdempotencyKey)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "http.idempotency_key");
}

ZEND_METHOD(JobU_HttpJobAttributes, setIdempotencyKey)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "http.idempotency_key");
}

ZEND_METHOD(JobU_HttpJobAttributes, getTlsVerify)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "http.tls_verify");
}

ZEND_METHOD(JobU_HttpJobAttributes, setTlsVerify)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "http.tls_verify");
}

ZEND_METHOD(JobU_HttpJobAttributes, getMaxRedirects)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "http.max_redirects");
}

ZEND_METHOD(JobU_HttpJobAttributes, setMaxRedirects)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "http.max_redirects");
}

ZEND_METHOD(JobU_HttpJobAttributes, getHttpBodyLimit)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "output.http_body_limit");
}

ZEND_METHOD(JobU_HttpJobAttributes, setHttpBodyLimit)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "output.http_body_limit");
}

ZEND_METHOD(JobU_HttpJobAttributes, getHttpHeadersLimit)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "output.http_headers_limit");
}

ZEND_METHOD(JobU_HttpJobAttributes, setHttpHeadersLimit)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "output.http_headers_limit");
}

ZEND_METHOD(JobU_HttpJobAttributes, getRetryErrors)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "http.retry_errors");
}

ZEND_METHOD(JobU_HttpJobAttributes, setRetryErrors)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "http.retry_errors");
}

ZEND_METHOD(JobU_HttpJobAttributes, getRetryStatuses)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Get, "http.retry_statuses");
}

ZEND_METHOD(JobU_HttpJobAttributes, setRetryStatuses)
{
    jb::php::access(execute_data, return_value, jb::php::Access::Set, "http.retry_statuses");
}

ZEND_METHOD(JobU_QueueDefaults, cli)
{
    jb::php::view(execute_data, return_value, jb::jobu::JobType::Cli);
}

ZEND_METHOD(JobU_QueueDefaults, http)
{
    jb::php::view(execute_data, return_value, jb::jobu::JobType::Http);
}
