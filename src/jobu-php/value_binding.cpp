#include "value_binding.hpp"

#include <jb/core/text_validation.hpp>
#include <jb/jobu/attribute.hpp>

#include <phpcxx/arguments.hpp>
#include <phpcxx/conversion.hpp>
#include <phpcxx/error.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>

extern "C" {
#include <ext/date/php_date.h>
}

#include <chrono>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::php {
namespace {

// Native snapshots own no Zend references. In particular, changing a supplied
// DateTime/DateTimeZone later cannot change an already constructed schedule.
struct SecretValue {
    explicit SecretValue(std::string name) noexcept
        : name(std::move(name))
    {}

    auto trace([[maybe_unused]] ::php::GcVisitor& visitor) const noexcept -> void {}

    std::string name;
};

struct ScheduleValue {
    explicit ScheduleValue(jobu::JobCreationSchedule value) noexcept
        : value(std::move(value))
    {}

    auto trace([[maybe_unused]] ::php::GcVisitor& visitor) const noexcept -> void {}

    jobu::JobCreationSchedule value;
};

struct SecretPolicy : ::php::ObjectBindingPolicy<SecretValue> {
    static auto compare(SecretValue const& left, SecretValue const& right) noexcept -> int
    {
        return left.name.compare(right.name);
    }
};

struct SchedulePolicy : ::php::ObjectBindingPolicy<ScheduleValue> {
    static auto compare(ScheduleValue const& left, ScheduleValue const& right) noexcept -> int
    {
        // Within OnceSchedule, the unresolved now marker sorts before absolute
        // instants. Binding comparison already rejects different PHP classes.
        if (left.value.index() != right.value.index()) {
            return left.value.index() < right.value.index() ? -1 : 1;
        }
        if (std::holds_alternative<jobu::ImmediateSchedule>(left.value)) {
            return 0;
        }
        if (auto const* once = std::get_if<jobu::OnceSchedule>(&left.value)) {
            auto const at = std::get<jobu::OnceSchedule>(right.value).planned_at;
            if (once->planned_at < at) {
                return -1;
            }
            return once->planned_at > at ? 1 : 0;
        }

        auto const& left_cron        = std::get<jobu::CronScheduleInput>(left.value);
        auto const& right_cron       = std::get<jobu::CronScheduleInput>(right.value);
        auto const  expression_order = left_cron.expression.compare(right_cron.expression);
        if (expression_order != 0) {
            return expression_order;
        }
        // Optional ordering keeps omission distinct from explicit UTC and sorts
        // omission first, without resolving the daemon's default timezone.
        if (left_cron.timezone < right_cron.timezone) {
            return -1;
        }
        return left_cron.timezone > right_cron.timezone ? 1 : 0;
    }
};

using SecretBinding   = ::php::ObjectBinding<SecretValue, SecretPolicy>;
using ScheduleBinding = ::php::ObjectBinding<ScheduleValue, SchedulePolicy>;

auto class_entry(std::string_view name) -> zend_class_entry*
{
    return static_cast<zend_class_entry*>(zend_hash_str_find_ptr(EG(class_table), name.data(), name.size()));
}

auto native_schedule(::php::ValueView value) -> ::php::Result<ScheduleValue*>
{
    auto object = ::php::as_object(value, class_entry("jobu\\schedule"));
    if (!object) {
        return object.error();
    }
    return ScheduleBinding::get(const_cast<zend_object*>(object.value()));
}

auto timezone_name(::php::ValueView value) -> ::php::Result<std::string>
{
    if (::php::has_exception()) {
        return ::php::pending_exception();
    }
    auto object = ::php::as_object(value, php_date_get_timezone_ce());
    if (!object) {
        return object.error();
    }

    // Read native state, never an overridable getName(). Fixed offsets and
    // abbreviations have different daemon semantics and must not be normalized.
    auto const* zone = php_timezone_obj_from_obj(const_cast<zend_object*>(object.value()));
    if (!zone->initialized || zone->type != TIMELIB_ZONETYPE_ID || !zone->tzi.tz) {
        return ::php::Error{.kind    = ::php::ErrorKind::Value,
                            .message = "timezone must be an initialized named timezone"};
    }
    return std::string(zone->tzi.tz->name);
}

auto timezone_to_php(std::string const& name) -> ::php::Result<::php::Value>
{
    if (::php::has_exception()) {
        return ::php::pending_exception();
    }
    zval slot{};
    object_init_ex(&slot, php_date_get_timezone_ce());
    auto owner = ::php::Value::adopt(slot);
    zval argument{};
    ZVAL_STRINGL(&argument, name.data(), name.size());
    auto argument_owner = ::php::Value::adopt(argument);
    zend_call_known_instance_method_with_1_params(php_date_get_timezone_ce()->constructor,
                                                  Z_OBJ_P(owner.get()),
                                                  nullptr,
                                                  const_cast<zval*>(argument_owner.get()));
    if (::php::has_exception()) {
        return ::php::pending_exception();
    }
    auto checked = timezone_name(owner.view());
    if (!checked) {
        return checked.error();
    }
    return owner;
}

auto make_schedule(jobu::JobCreationSchedule value) -> ::php::Result<::php::Value>
{
    if (::php::has_exception()) {
        return ::php::pending_exception();
    }
    auto* entry = class_entry(std::holds_alternative<jobu::CronScheduleInput>(value) ? "jobu\\cronschedule"
                                                                                     : "jobu\\onceschedule");
    zval  slot{};
    object_init_ex(&slot, entry);
    auto owner  = ::php::Value::adopt(slot);
    auto native = ScheduleBinding::construct(Z_OBJ_P(owner.get()), std::move(value));
    if (!native) {
        return native.error();
    }
    return owner;
}

auto construct_secret(zend_execute_data* frame) noexcept -> void
{
    ::php::Arguments arguments(frame);
    auto             count = arguments.count(1, 1);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto name = arguments.string(1);
    if (!name) {
        ::php::raise(name.error());
        return;
    }
    auto text = name.value().bytes();
    if (text.size() > 128 || !jobu::is_valid_attribute_name(text)) {
        ::php::raise({.kind = ::php::ErrorKind::Value, .message = "invalid secret reference name", .argument = 1});
        return;
    }
    auto native = SecretBinding::construct(Z_OBJ(frame->This), std::string(text));
    if (!native) {
        ::php::raise(native.error());
    }
}

auto construct_once(zend_execute_data* frame) noexcept -> void
{
    ::php::Arguments arguments(frame);
    auto             count = arguments.count(1, 1);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto at = time_from_php(arguments.get(1));
    if (!at) {
        ::php::raise(at.error());
        return;
    }
    auto native =
        ScheduleBinding::construct(Z_OBJ(frame->This), jobu::JobCreationSchedule{jobu::OnceSchedule{at.value()}});
    if (!native) {
        ::php::raise(native.error());
    }
}

auto construct_cron(zend_execute_data* frame) noexcept -> void
{
    ::php::Arguments arguments(frame);
    auto             count = arguments.count(1, 2);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto expression = arguments.string(1);
    if (!expression) {
        ::php::raise(expression.error());
        return;
    }
    auto text = expression.value().bytes();
    if (text.empty() || text.find('\0') != std::string_view::npos || !core::is_valid_utf8(text)) {
        ::php::raise({.kind     = ::php::ErrorKind::Value,
                      .message  = "cron expression must be nonempty UTF-8 without NUL",
                      .argument = 1});
        return;
    }

    // Preserve omission through creation/update conversion; only the daemon may
    // resolve its configured default. Cron grammar validation also stays remote.
    std::optional<std::string> zone;
    auto                       input = arguments.get(2).dereference();
    if (input.is_initialized() && !input.is_null()) {
        auto name = timezone_name(input);
        if (!name) {
            ::php::raise(name.error());
            return;
        }
        zone = std::move(name).value();
    }
    auto schedule = jobu::CronScheduleInput{.expression = std::string(text), .timezone = std::move(zone)};
    auto native   = ScheduleBinding::construct(Z_OBJ(frame->This), jobu::JobCreationSchedule{std::move(schedule)});
    if (!native) {
        ::php::raise(native.error());
    }
}

// All zero-argument getters share the count check, but preserve their explicit
// value operations below. Results transfer ownership only after full success.
auto read_schedule(zend_execute_data* frame) -> ::php::Result<ScheduleValue*>
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        return count.error();
    }
    return ScheduleBinding::get(Z_OBJ(frame->This));
}

auto return_value(::php::Result<::php::Value> result, zval* output) -> void
{
    if (!result) {
        ::php::raise(result.error());
        return;
    }
    *output = result.value().release();
}

} // namespace

auto bind_value_classes(zend_class_entry* secret, zend_class_entry* once, zend_class_entry* cron) -> bool
{
    for (auto* entry : {secret, once, cron}) {
        entry->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    }
    return SecretBinding::bind(secret) && ScheduleBinding::bind(once) && ScheduleBinding::bind(cron);
}

auto uuid_from_php(::php::ValueView value) -> ::php::Result<core::Uuid>
{
    auto text = ::php::as_string(value);
    if (!text) {
        return text.error();
    }
    auto uuid = core::Uuid::parse(text.value().bytes());
    if (!uuid || uuid.value().to_string() != text.value().bytes()) {
        return ::php::Error{.kind = ::php::ErrorKind::Value, .message = "UUID must use canonical lowercase spelling"};
    }
    return uuid.value();
}

auto time_from_php(::php::ValueView value) -> ::php::Result<core::UtcTimePoint>
{
    auto timestamp = ::php::as_timestamp(value);
    if (!timestamp) {
        return timestamp.error();
    }
    auto microseconds = ::php::unix_microseconds(timestamp.value());
    if (!microseconds) {
        return microseconds.error();
    }
    using Duration     = core::UtcClock::duration;
    using Microseconds = std::chrono::microseconds;
    // Truncation toward zero gives the inclusive representable microsecond
    // bounds even when the clock stores nanoseconds (as on Linux).
    static_assert(std::ratio_divide<Microseconds::period, Duration::period>::den == 1);
    if (microseconds.value() < std::chrono::duration_cast<Microseconds>(Duration::min()).count() ||
        microseconds.value() > std::chrono::duration_cast<Microseconds>(Duration::max()).count()) {
        return ::php::Error{.kind = ::php::ErrorKind::Value, .message = "timestamp is outside the native clock range"};
    }
    return core::UtcTimePoint{std::chrono::duration_cast<Duration>(Microseconds{microseconds.value()})};
}

auto time_to_php(core::UtcTimePoint value) -> ::php::Result<::php::Value>
{
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(value.time_since_epoch());
    if (us != value.time_since_epoch()) {
        return ::php::Error{.kind    = ::php::ErrorKind::Value,
                            .message = "timestamp must have exact microsecond precision"};
    }
    return ::php::datetime_immutable(::php::timestamp_from_microseconds(us.count()));
}

auto secret_name_from_php(::php::ValueView value) -> ::php::Result<std::string>
{
    auto object = ::php::as_object(value, class_entry("jobu\\secretreference"));
    if (!object) {
        return object.error();
    }
    auto native = SecretBinding::get(const_cast<zend_object*>(object.value()));
    if (!native) {
        return native.error();
    }
    return native.value()->name;
}

auto creation_schedule_from_php(::php::ValueView value) -> ::php::Result<jobu::JobCreationSchedule>
{
    auto native = native_schedule(value);
    if (!native) {
        return native.error();
    }
    return native.value()->value;
}

auto update_schedule_from_php(::php::ValueView value) -> ::php::Result<jobu::JobScheduleInput>
{
    auto schedule = creation_schedule_from_php(value);
    if (!schedule) {
        return schedule.error();
    }
    if (auto* once = std::get_if<jobu::OnceSchedule>(&schedule.value())) {
        return jobu::JobScheduleInput{*once};
    }
    if (auto* cron = std::get_if<jobu::CronScheduleInput>(&schedule.value())) {
        return jobu::JobScheduleInput{std::move(*cron)};
    }
    return ::php::Error{.kind = ::php::ErrorKind::Value, .message = "symbolic now is valid only for job creation"};
}

auto schedule_to_php(jobu::JobSchedule const& value) -> ::php::Result<::php::Value>
{
    if (auto const* once = std::get_if<jobu::OnceSchedule>(&value)) {
        auto checked = time_to_php(once->planned_at);
        if (!checked) {
            return checked.error();
        }
        return make_schedule(*once);
    }
    auto const& cron    = std::get<jobu::CronSchedule>(value);
    auto        checked = timezone_to_php(cron.timezone);
    if (!checked) {
        return checked.error();
    }
    return make_schedule(jobu::CronScheduleInput{.expression = cron.expression, .timezone = cron.timezone});
}

} // namespace jb::php

ZEND_METHOD(JobU_SecretReference, __construct)
{
    (void)return_value;
    jb::php::construct_secret(execute_data);
}

ZEND_METHOD(JobU_SecretReference, getName)
{
    [&]() noexcept {
        auto count = php::Arguments(execute_data).count(0, 0);
        if (!count) {
            php::raise(count.error());
            return;
        }
        auto name = jb::php::secret_name_from_php(php::ValueView(&execute_data->This));
        if (!name) {
            php::raise(name.error());
            return;
        }
        ZVAL_STRINGL(return_value, name.value().data(), name.value().size());
    }();
}

ZEND_METHOD(JobU_OnceSchedule, __construct)
{
    (void)return_value;
    jb::php::construct_once(execute_data);
}

ZEND_METHOD(JobU_OnceSchedule, now)
{
    [&]() noexcept {
        auto count = php::Arguments(execute_data).count(0, 0);
        if (!count) {
            php::raise(count.error());
            return;
        }
        jb::php::return_value(jb::php::make_schedule(jb::jobu::ImmediateSchedule{}), return_value);
    }();
}

ZEND_METHOD(JobU_OnceSchedule, isNow)
{
    [&]() noexcept {
        auto native = jb::php::read_schedule(execute_data);
        if (!native) {
            php::raise(native.error());
            return;
        }
        ZVAL_BOOL(return_value, std::holds_alternative<jb::jobu::ImmediateSchedule>(native.value()->value));
    }();
}

ZEND_METHOD(JobU_OnceSchedule, getAt)
{
    [&]() noexcept {
        auto native = jb::php::read_schedule(execute_data);
        if (!native) {
            php::raise(native.error());
            return;
        }
        auto const* once = std::get_if<jb::jobu::OnceSchedule>(&native.value()->value);
        if (!once) {
            ZVAL_NULL(return_value);
            return;
        }
        jb::php::return_value(jb::php::time_to_php(once->planned_at), return_value);
    }();
}

ZEND_METHOD(JobU_CronSchedule, __construct)
{
    (void)return_value;
    jb::php::construct_cron(execute_data);
}

ZEND_METHOD(JobU_CronSchedule, getExpression)
{
    [&]() noexcept {
        auto native = jb::php::read_schedule(execute_data);
        if (!native) {
            php::raise(native.error());
            return;
        }
        auto const& expression = std::get<jb::jobu::CronScheduleInput>(native.value()->value).expression;
        ZVAL_STRINGL(return_value, expression.data(), expression.size());
    }();
}

ZEND_METHOD(JobU_CronSchedule, getTimezone)
{
    [&]() noexcept {
        auto native = jb::php::read_schedule(execute_data);
        if (!native) {
            php::raise(native.error());
            return;
        }
        auto const& zone = std::get<jb::jobu::CronScheduleInput>(native.value()->value).timezone;
        if (!zone) {
            ZVAL_NULL(return_value);
            return;
        }
        jb::php::return_value(jb::php::timezone_to_php(*zone), return_value);
    }();
}
