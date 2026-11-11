#include "client_binding.hpp"

#include <phpcxx/arguments.hpp>
#include <phpcxx/error.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>

#include <Zend/zend_exceptions.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <jb/rpc/protocol.hpp>

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

struct DurationPolicy : ::php::ObjectBindingPolicy<Duration> {
    static auto compare(Duration const& left, Duration const& right) noexcept -> int
    {
        // Relational comparison avoids overflow at the signed duration limits.
        if (left.milliseconds() < right.milliseconds()) {
            return -1;
        }
        return left.milliseconds() > right.milliseconds() ? 1 : 0;
    }
};

struct ClientPolicy : ::php::ObjectBindingPolicy<Client> {

    static auto trace([[maybe_unused]] Client const& client, [[maybe_unused]] ::php::GcVisitor& visitor) noexcept
        -> void
    {}
};

using DurationBinding = ::php::ObjectBinding<Duration, DurationPolicy>;
using ClientBinding   = ::php::ObjectBinding<Client, ClientPolicy>;

auto class_entry(std::string_view name) -> zend_class_entry*
{
    // These lowercase names were registered during this runtime's MINIT. Look
    // them up without autoload or caching a pointer across Embed runtime cycles.
    return static_cast<zend_class_entry*>(zend_hash_str_find_ptr(EG(class_table), name.data(), name.size()));
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

    // Connection diagnostics use fixed text rather than socket paths or peer detail.
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
    duration->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    return DurationBinding::bind(duration) && ClientBinding::bind(client);
}

auto native_client(zend_object* object) -> ::php::Result<Client*>
{
    return ClientBinding::get(object);
}

auto operation_deadline(Client const&           client,
                        ::php::Arguments const& arguments,
                        std::uint32_t           position,
                        core::TimePoint         start) -> ::php::Result<core::TimePoint>
{
    std::optional<std::chrono::milliseconds> timeout;
    auto                                     value = arguments.get(position).dereference();
    if (value.get() && !value.is_null()) {
        auto object = arguments.object(position, class_entry("jobu\\duration"));
        if (!object) {
            return object.error();
        }
        auto duration = DurationBinding::get(Z_OBJ_P(value.get()));
        if (!duration) {
            return duration.error();
        }
        timeout = duration.value()->milliseconds();
    }
    auto deadline = client.deadline(start, timeout);
    if (!deadline) {
        return ::php::Error{.kind = ::php::ErrorKind::Value, .message = deadline.error().message, .argument = position};
    }
    return deadline.value();
}

auto raise_operation_failure(Failure const& failure, Operation const& operation) noexcept -> void
{
    if (::php::has_exception()) {
        return;
    }
    if (auto const* local = std::get_if<core::Error>(&failure.error);
        local && !failure.outcome_unknown &&
        (local->code == "jobu.protocol.invalid_request" || local->code == "php.client.invalid_timeout")) {
        ::php::raise({.kind = ::php::ErrorKind::Value, .message = "Invalid JobU operation argument"});
        return;
    }
    auto        metadata = failure_metadata(failure);
    auto        name     = std::string_view{"jobu\\exception"};
    auto const* message  = "The JobU operation failed";
    bool const  unknown  = mutation_outcome_unknown(operation, failure);
    if (unknown) {
        name    = "jobu\\outcomeunknownexception";
        message = "The JobU operation may have committed; its outcome is unknown";
    }
    else if (metadata.code == "jobu.history.invalid_cursor" || metadata.code == "jobu.statistics.invalid_cursor") {
        name    = "jobu\\invalidcursorexception";
        message = "The JobU cursor is invalid or expired";
    }
    else if (metadata.code == "jobu.queue.revision_conflict" || metadata.code == "jobu.job.revision_conflict") {
        name    = "jobu\\revisionconflictexception";
        message = "The JobU resource revision has changed";
    }
    else if (metadata.category == "not_found") {
        name    = "jobu\\notfoundexception";
        message = "The JobU resource was not found";
    }
    else if (metadata.category == "timeout") {
        name    = "jobu\\timeoutexception";
        message = "The JobU operation deadline expired";
    }
    else if (metadata.category == "unsupported") {
        name    = "jobu\\unsupportedfeatureexception";
        message = "The requested JobU feature is unsupported";
    }
    else if (metadata.code == "php.resource.unsaved_changes") {
        name    = "jobu\\unsavedchangesexception";
        message = "The JobU resource has unsaved changes";
    }
    else if (failure.kind == jobu::ControlFailureKind::Local &&
             (metadata.category == "unavailable" || metadata.category == "io" ||
              metadata.code == "rpc.protocol_error" || metadata.code == "jobu.client.invalid_response" ||
              metadata.code == "php.client.closed")) {
        name    = "jobu\\connectionexception";
        message = "The JobU connection is unavailable";
    }

    // Keep remote numeric codes in Exception::getCode and stable string metadata
    // in separate properties. Neither peer text nor arbitrary data is displayed.
    auto const* remote = std::get_if<rpc::RpcError>(&failure.error);
    zend_throw_exception(class_entry(name), message, remote ? remote->code : 0);
    auto set =
        [](zend_class_entry* scope, char const* property, std::size_t length, std::optional<std::string> const& value) {
            if (value) {
                zend_update_property_stringl(scope, EG(exception), property, length, value->data(), value->size());
            }
        };
    auto* base = class_entry("jobu\\exception");
    set(base, "_errorCode", sizeof("_errorCode") - 1, metadata.code);
    set(base, "_category", sizeof("_category") - 1, metadata.category);
    if (unknown) {
        auto* outcome = class_entry("jobu\\outcomeunknownexception");
        set(outcome, "_operation", sizeof("_operation") - 1, operation.name());
        set(outcome, "_idempotencyKey", sizeof("_idempotencyKey") - 1, operation.key());
    }
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

ZEND_METHOD(JobU_Duration, getMilliseconds)
{
    [&]() noexcept {
        auto count = php::Arguments(execute_data).count(0, 0);
        if (!count) {
            php::raise(count.error());
            return;
        }
        auto native = jb::php::DurationBinding::get(Z_OBJ(execute_data->This));
        if (!native) {
            php::raise(native.error());
            return;
        }
        ZVAL_LONG(return_value, native.value()->milliseconds().count());
    }();
}
