#include "client_binding.hpp"

#include <phpcxx/arguments.hpp>
#include <phpcxx/error.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>

#include <Zend/zend_exceptions.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace jb::php {
namespace {

class Duration final {
public:
    explicit Duration(std::int64_t milliseconds) noexcept
        : _milliseconds(milliseconds)
    {}

    auto milliseconds() const noexcept -> std::chrono::milliseconds { return std::chrono::milliseconds{_milliseconds}; }

    auto trace([[maybe_unused]] ::php::GcVisitor& visitor) const noexcept -> void {}

private:
    std::int64_t _milliseconds;
};

struct ClientPolicy : ::php::ObjectBindingPolicy<Client> {

    static auto trace([[maybe_unused]] Client const& client, [[maybe_unused]] ::php::GcVisitor& visitor) noexcept
        -> void
    {}
};

using DurationBinding = ::php::ObjectBinding<Duration>;
using ClientBinding   = ::php::ObjectBinding<Client, ClientPolicy>;

auto class_entry(std::string_view name) -> zend_class_entry*
{
    // These lowercase names were registered during this runtime's MINIT. Look
    // them up without autoload or caching a pointer across Embed runtime cycles.
    return static_cast<zend_class_entry*>(zend_hash_str_find_ptr(EG(class_table), name.data(), name.size()));
}

auto category_name(core::ErrorCategory category) -> char const*
{
    switch (category) {
        case core::ErrorCategory::InvalidArgument:
            return "invalid_argument";
        case core::ErrorCategory::NotFound:
            return "not_found";
        case core::ErrorCategory::Conflict:
            return "conflict";
        case core::ErrorCategory::PermissionDenied:
            return "permission_denied";
        case core::ErrorCategory::Unavailable:
            return "unavailable";
        case core::ErrorCategory::ResourceExhausted:
            return "resource_exhausted";
        case core::ErrorCategory::Cancelled:
            return "cancelled";
        case core::ErrorCategory::Timeout:
            return "timeout";
        case core::ErrorCategory::Io:
            return "io";
        case core::ErrorCategory::Unsupported:
            return "unsupported";
        case core::ErrorCategory::Internal:
            return "internal";
    }
    return "internal";
}

auto raise_constructor_error(core::Error const& error) -> void
{
    if (::php::has_exception()) {
        return;
    }
    if (error.category == core::ErrorCategory::InvalidArgument && error.code == "php.client.invalid_timeout") {
        ::php::raise({.kind = ::php::ErrorKind::Value, .message = error.message, .argument = 2});
        return;
    }

    // Constructor-only translation. Do not forward socket paths, backend detail,
    // or remote text. Operation/domain/unknown-outcome mapping belongs to 2.4.
    auto        name    = std::string_view{"jobu\\exception"};
    auto const* message = "Unable to initialize the JobU client";
    if (error.category == core::ErrorCategory::Timeout) {
        name    = "jobu\\timeoutexception";
        message = "The JobU connection deadline expired";
    }
    else if (error.category == core::ErrorCategory::Unsupported) {
        name    = "jobu\\unsupportedfeatureexception";
        message = "The daemon does not support the required JobU API and capabilities";
    }
    else if (error.category == core::ErrorCategory::Unavailable || error.category == core::ErrorCategory::Io ||
             error.code == "jobu.client.invalid_response") {
        name    = "jobu\\connectionexception";
        message = "Unable to connect to JobU or complete its handshake";
    }
    ::php::raise({.kind = ::php::ErrorKind::Runtime, .message = message}, class_entry(name));
    if (EG(exception)) {
        auto* base = class_entry("jobu\\exception");
        zend_update_property_stringl(base,
                                     EG(exception),
                                     "_errorCode",
                                     sizeof("_errorCode") - 1,
                                     error.code.data(),
                                     error.code.size());
        zend_update_property_string(base,
                                    EG(exception),
                                    "_category",
                                    sizeof("_category") - 1,
                                    category_name(error.category));
    }
}

auto make_duration(zend_execute_data* execute_data, zval* return_value, std::int64_t scale) noexcept -> void
{
    ::php::Arguments arguments(execute_data);
    auto             count = arguments.count(1, 1);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto value = arguments.integer(1);
    if (!value) {
        ::php::raise(value.error());
        return;
    }
    if (value.value() > std::numeric_limits<std::int64_t>::max() / scale ||
        value.value() < std::numeric_limits<std::int64_t>::min() / scale) {
        ::php::raise({.kind     = ::php::ErrorKind::Value,
                      .message  = "Duration exceeds the integer millisecond range",
                      .argument = 1});
        return;
    }

    object_init_ex(return_value, execute_data->func->common.scope);
    auto native = DurationBinding::construct(Z_OBJ_P(return_value), value.value() * scale);
    if (!native) {
        ::php::raise(native.error());
    }
}

auto construct_client(zend_execute_data* execute_data) noexcept -> void
{
    auto const       start = core::Clock::now();
    ::php::Arguments arguments(execute_data);
    auto             count = arguments.count(1, 2);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto endpoint = arguments.string(1);
    if (!endpoint) {
        ::php::raise(endpoint.error());
        return;
    }
    if (endpoint.value().bytes().empty() || endpoint.value().bytes().find('\0') != std::string_view::npos) {
        ::php::raise({.kind     = ::php::ErrorKind::Value,
                      .message  = "Socket path must be nonempty and contain no NUL bytes",
                      .argument = 1});
        return;
    }

    auto timeout = std::chrono::milliseconds{10000};
    auto value   = arguments.get(2).dereference();
    if (value.get() && !value.is_null()) {
        auto object = arguments.object(2, class_entry("jobu\\duration"));
        if (!object) {
            ::php::raise(object.error());
            return;
        }
        auto duration = DurationBinding::get(Z_OBJ_P(value.get()));
        if (!duration) {
            ::php::raise(duration.error());
            return;
        }
        timeout = duration.value()->milliseconds();
    }
    auto deadline = make_deadline(start, timeout);
    if (!deadline) {
        raise_constructor_error(deadline.error());
        return;
    }

    auto native = ClientBinding::construct(Z_OBJ(execute_data->This));
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    auto* request = current_request();
    if (!request) {
        ::php::raise({.kind = ::php::ErrorKind::Runtime, .message = "No active JobU request"});
        return;
    }
    auto initialized =
        native.value()->initialize(*request, std::string(endpoint.value().bytes()), deadline.value(), timeout);
    if (!initialized) {
        raise_constructor_error(initialized.error());
    }
}

auto close_client(zend_execute_data* execute_data) noexcept -> void
{
    auto count = ::php::Arguments(execute_data).count(0, 0);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto native = native_client(Z_OBJ(execute_data->This));
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    native.value()->close();
}

} // namespace

auto bind_client_classes(zend_class_entry* duration, zend_class_entry* client) -> bool
{
    return DurationBinding::bind(duration) && ClientBinding::bind(client);
}

auto native_client(zend_object* object) -> ::php::Result<Client*>
{
    return ClientBinding::get(object);
}

} // namespace jb::php

ZEND_METHOD(JobU_Duration, __construct)
{
    (void)execute_data;
    (void)return_value;
    // Even Reflection must not manufacture a supposedly initialized value.
    []() noexcept { ::php::raise({.kind = ::php::ErrorKind::Runtime, .message = "Use a Duration factory"}); }();
}

ZEND_METHOD(JobU_Duration, milliseconds)
{
    jb::php::make_duration(execute_data, return_value, 1);
}

ZEND_METHOD(JobU_Duration, seconds)
{
    jb::php::make_duration(execute_data, return_value, 1000);
}

ZEND_METHOD(JobU_Duration, minutes)
{
    jb::php::make_duration(execute_data, return_value, 60000);
}

ZEND_METHOD(JobU_Duration, hours)
{
    jb::php::make_duration(execute_data, return_value, 3600000);
}

ZEND_METHOD(JobU_Duration, days)
{
    jb::php::make_duration(execute_data, return_value, 86400000);
}

ZEND_METHOD(JobU_Client, __construct)
{
    (void)return_value;
    jb::php::construct_client(execute_data);
}

ZEND_METHOD(JobU_Client, close)
{
    (void)return_value;
    jb::php::close_client(execute_data);
}
