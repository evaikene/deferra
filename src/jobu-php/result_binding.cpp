#include "result_binding.hpp"
#include "value_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/core/byte_buffer.hpp>
#include <jb/jobu/history_json.hpp>
#include <jb/jobu/secret_json.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/exception.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace jb::php {
namespace {
using core::JsonValue;
using snapshot::field;
using snapshot::invalid;
using snapshot::Kind;

// Check native trees before recursive serialization or PHP allocation. Transport parsing
// is already bounded, but adapters can also receive directly constructed native values.
auto bounded_tree(JsonValue const& value, std::size_t depth, std::size_t& budget) -> bool
{
    if (budget == 0 || ((value.is_array() || value.is_object()) && depth >= 64)) {
        return false;
    }
    --budget;
    auto consume = [&budget](std::size_t bytes) {
        if (bytes > budget) {
            return false;
        }
        budget -= bytes;
        return true;
    };
    if (value.is_string()) {
        return consume(value.as_string().size());
    }
    if (value.is_array()) {
        for (auto const& child : value.as_array()) {
            if (!bounded_tree(child, depth + 1, budget)) {
                return false;
            }
        }
    }
    if (value.is_object()) {
        for (auto const& [name, child] : value.as_object()) {
            if (!consume(name.size()) || !bounded_tree(child, depth + 1, budget)) {
                return false;
            }
        }
    }
    return true;
}

auto initialized(zend_execute_data* frame, std::uint32_t arguments) -> ::php::Result<snapshot::Value*>
{
    auto count = ::php::Arguments(frame).count(arguments, arguments);
    if (!count) {
        return count.error();
    }
    return snapshot::Binding::get(Z_OBJ(frame->This));
}

auto bounded_integer(JsonValue const* value, std::int64_t minimum, std::int64_t maximum) -> JsonValue const*
{
    if (!value) {
        return nullptr;
    }
    if (value->is_int() && value->as_int() >= minimum && value->as_int() <= maximum) {
        return value;
    }
    if (value->is_uint() && value->as_uint() >= static_cast<std::uint64_t>(minimum) &&
        value->as_uint() <= static_cast<std::uint64_t>(maximum)) {
        return value;
    }
    return nullptr;
}

auto result_fields(snapshot::Value const& value) -> JsonValue const&
{
    return *field(value.fields, "result");
}

auto result_get(zend_execute_data* frame,
                zval*              output,
                std::string_view   name,
                Kind               kind,
                std::int64_t       minimum = 0,
                std::int64_t       maximum = INT64_MAX,
                std::string_view   parent  = {}) noexcept -> void
{
    auto native = initialized(frame, 0);
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    auto const* fields = &result_fields(*native.value());
    if (!parent.empty()) {
        fields = field(*fields, parent);
    }
    auto const* value = fields ? field(*fields, name) : nullptr;
    if (kind == Kind::Integer || kind == Kind::Duration) {
        value = bounded_integer(value, minimum, maximum);
    }
    snapshot::return_value(snapshot::project(value, kind), output);
}

auto generic_get(zend_execute_data* frame, zval* output, bool existence) noexcept -> void
{
    auto native = initialized(frame, 1);
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    auto name = ::php::Arguments(frame).string(1);
    if (!name) {
        ::php::raise(name.error());
        return;
    }
    auto const* value = field(result_fields(*native.value()), name.value().bytes());
    if (existence) {
        ZVAL_BOOL(output, value != nullptr);
        return;
    }
    snapshot::return_value(snapshot::project(value, Kind::Generic), output);
}

auto capture_get(zend_execute_data* frame, zval* output, std::string_view name, Kind kind) noexcept -> void
{
    auto native = initialized(frame, 1);
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    auto channel = snapshot::enum_text(::php::Arguments(frame).get(1), "jobu\\outputchannel");
    if (!channel) {
        ::php::raise(channel.error());
        return;
    }
    auto const& type     = field(native.value()->fields, "type")->as_string();
    auto const& selected = channel.value();
    auto        valid =
        type == "cli" ? selected == "stdout" || selected == "stderr" : selected == "body" || selected == "headers";
    if (!valid) {
        ::php::raise(invalid("Channel does not belong to this result type"));
        return;
    }
    auto const* capture = field(result_fields(*native.value()), selected);
    auto const* value   = capture ? field(*capture, name) : nullptr;
    if (kind == Kind::Integer) {
        value = bounded_integer(value, 0, INT64_MAX);
    }
    snapshot::return_value(snapshot::project(value, kind), output);
}

auto api_version(zend_execute_data* frame, zval* output, std::string_view name) noexcept -> void
{
    auto native = initialized(frame, 0);
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    snapshot::return_value(snapshot::project(field(*field(native.value()->fields, "api_version"), name), Kind::Integer),
                           output);
}

auto has_capability(zend_execute_data* frame, zval* output) noexcept -> void
{
    auto native = initialized(frame, 1);
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    auto name = ::php::Arguments(frame).string(1);
    if (!name) {
        ::php::raise(name.error());
        return;
    }
    for (auto const& capability : field(native.value()->fields, "capabilities")->as_array()) {
        if (capability.as_string() == name.value().bytes()) {
            ZVAL_TRUE(output);
            return;
        }
    }
    ZVAL_FALSE(output);
}

auto output_data(zend_execute_data* frame, zval* output) noexcept -> void
{
    auto native = initialized(frame, 0);
    if (!native) {
        ::php::raise(native.error());
        return;
    }
    // The native codec alone owns encoding rules. Decode each slice before exposing
    // bytes, including base64 slices that split a UTF-8 sequence across boundaries.
    auto chunk = jobu::attempt_output_chunk_from_json(native.value()->fields);
    if (!chunk) {
        ::php::raise(invalid());
        return;
    }
    if (chunk->data.empty()) {
        // Empty vectors may have a null data pointer; Zend requires a valid pointer
        // even for a zero-length string. Availability remains in the status getter.
        ZVAL_EMPTY_STRING(output);
        return;
    }
    auto bytes = core::as_string_view(chunk->data);
    ZVAL_STRINGL(output, bytes.data(), bytes.size());
}

auto private_constructor() noexcept -> void
{
    ::php::raise(invalid("This snapshot is created by JobU"));
}
} // namespace

auto bind_result_classes(zend_class_entry* system,
                         zend_class_entry* secret,
                         zend_class_entry* base,
                         zend_class_entry* cli,
                         zend_class_entry* http,
                         zend_class_entry* output) -> bool
{
    // Seal construction through a final private base constructor; user subclasses
    // cannot fabricate initialized storage for inherited native getters.
    return snapshot::bind(system) && snapshot::bind(secret) && snapshot::bind(base) && snapshot::bind(cli) &&
           snapshot::bind(http) && snapshot::bind(output);
}

auto system_info_to_php(jobu::SystemInfo const& value) -> ::php::Result<::php::Value>
{
    auto wire = jobu::system_info_to_json(value);
    if (!jobu::system_info_from_json(wire)) {
        return invalid();
    }
    return snapshot::make("jobu\\systeminfo", std::move(wire));
}

auto secret_metadata_to_php(jobu::SecretMetadata const& value) -> ::php::Result<::php::Value>
{
    // Check PHP date precision before publishing a snapshot with infallible typed getters.
    auto created = time_to_php(value.created_at);
    if (!created) {
        return created.error();
    }
    auto updated = time_to_php(value.updated_at);
    if (!updated) {
        return updated.error();
    }
    auto wire = jobu::secret_metadata_to_json(value);
    if (!wire || !jobu::secret_metadata_from_json(wire.value())) {
        return invalid();
    }
    return snapshot::make("jobu\\secretmetadata", std::move(wire).value());
}

auto execution_result_to_php(jobu::JobType type, std::optional<JsonValue> const& value) -> ::php::Result<::php::Value>
{
    if (::php::has_exception()) {
        return ::php::pending_exception();
    }
    if (!value) {
        return ::php::Value::null();
    }
    if (!value->is_object() || (type != jobu::JobType::Cli && type != jobu::JobType::Http)) {
        return invalid();
    }
    // Validate depth first; serialization then rejects invalid UTF-8/non-finite numbers.
    // The content budget matches the completion producers, not the larger RPC frame budget.
    auto budget = std::size_t{256} * 1024U;
    if (!bounded_tree(*value, 0, budget)) {
        return invalid("Invalid execution result snapshot");
    }
    auto encoded = core::serialize_json(*value);
    if (!encoded || encoded->size() > std::size_t{256} * 1024U) {
        return invalid("Invalid execution result snapshot");
    }
    auto cli = type == jobu::JobType::Cli;
    return snapshot::make(
        cli ? "jobu\\cliexecutionresult" : "jobu\\httpexecutionresult",
        JsonValue{
            .data = JsonValue::Object{{"type", {.data = std::string(cli ? "cli" : "http")}}, {"result", *value}}
    });
}

auto output_chunk_to_php(jobu::AttemptOutputChunk const& value) -> ::php::Result<::php::Value>
{
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<zend_long>::max());
    if (value.offset > maximum || value.bytes_returned > maximum || value.retained_bytes > maximum ||
        (value.next_offset && *value.next_offset > maximum) || (value.total_bytes && *value.total_bytes > maximum) ||
        (value.omitted_bytes && *value.omitted_bytes > maximum)) {
        return invalid("Output metadata exceeds the PHP integer range");
    }
    auto wire = jobu::attempt_output_chunk_to_json(value);
    if (!wire || !jobu::attempt_output_chunk_from_json(wire.value())) {
        return invalid();
    }
    return snapshot::make("jobu\\outputchunk", std::move(wire).value());
}
} // namespace jb::php

ZEND_METHOD(JobU_SystemInfo, __construct)
{
    (void)execute_data;
    (void)return_value;
    jb::php::private_constructor();
}

ZEND_METHOD(JobU_SecretMetadata, __construct)
{
    (void)execute_data;
    (void)return_value;
    jb::php::private_constructor();
}

ZEND_METHOD(JobU_ExecutionResult, __construct)
{
    (void)execute_data;
    (void)return_value;
    jb::php::private_constructor();
}

ZEND_METHOD(JobU_OutputChunk, __construct)
{
    (void)execute_data;
    (void)return_value;
    jb::php::private_constructor();
}

ZEND_METHOD(JobU_SystemInfo, getDaemonVersion)
{
    jb::php::snapshot::get(execute_data, return_value, "daemon_version", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_SystemInfo, getCapabilities)
{
    jb::php::snapshot::get(execute_data, return_value, "capabilities", jb::php::snapshot::Kind::Generic);
}

ZEND_METHOD(JobU_SecretMetadata, getName)
{
    jb::php::snapshot::get(execute_data, return_value, "name", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_SecretMetadata, getCreatedAt)
{
    jb::php::snapshot::get(execute_data, return_value, "created_at", jb::php::snapshot::Kind::Date);
}

ZEND_METHOD(JobU_SecretMetadata, getUpdatedAt)
{
    jb::php::snapshot::get(execute_data, return_value, "updated_at", jb::php::snapshot::Kind::Date);
}

ZEND_METHOD(JobU_ExecutionResult, getType)
{
    jb::php::snapshot::get(execute_data, return_value, "type", jb::php::snapshot::Kind::Enum, "jobu\\jobtype");
}

ZEND_METHOD(JobU_OutputChunk, getRunId)
{
    jb::php::snapshot::get(execute_data, return_value, "run_id", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_OutputChunk, getAttemptNumber)
{
    jb::php::snapshot::get(execute_data, return_value, "attempt_number", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_OutputChunk, getChannel)
{
    jb::php::snapshot::get(execute_data, return_value, "channel", jb::php::snapshot::Kind::Enum, "jobu\\outputchannel");
}

ZEND_METHOD(JobU_OutputChunk, getStatus)
{
    jb::php::snapshot::get(execute_data, return_value, "status", jb::php::snapshot::Kind::Enum, "jobu\\outputstatus");
}

ZEND_METHOD(JobU_OutputChunk, getOffset)
{
    jb::php::snapshot::get(execute_data, return_value, "offset", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_OutputChunk, getBytesReturned)
{
    jb::php::snapshot::get(execute_data, return_value, "bytes_returned", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_OutputChunk, getNextOffset)
{
    jb::php::snapshot::get(execute_data, return_value, "next_offset", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_OutputChunk, getRetainedBytes)
{
    jb::php::snapshot::get(execute_data, return_value, "retained_bytes", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_OutputChunk, getTotalBytes)
{
    jb::php::snapshot::get(execute_data, return_value, "total_bytes", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_OutputChunk, getOmittedBytes)
{
    jb::php::snapshot::get(execute_data, return_value, "omitted_bytes", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_OutputChunk, isTruncated)
{
    jb::php::snapshot::get(execute_data, return_value, "truncated", jb::php::snapshot::Kind::Boolean);
}

ZEND_METHOD(JobU_OutputChunk, isCaptureLost)
{
    jb::php::snapshot::get(execute_data, return_value, "capture_lost", jb::php::snapshot::Kind::Boolean);
}

ZEND_METHOD(JobU_SystemInfo, getApiMajor)
{
    jb::php::api_version(execute_data, return_value, "major");
}

ZEND_METHOD(JobU_SystemInfo, getApiMinor)
{
    jb::php::api_version(execute_data, return_value, "minor");
}

ZEND_METHOD(JobU_SystemInfo, hasCapability)
{
    jb::php::has_capability(execute_data, return_value);
}

ZEND_METHOD(JobU_OutputChunk, getData)
{
    jb::php::output_data(execute_data, return_value);
}

ZEND_METHOD(JobU_ExecutionResult, getOutcome)
{
    jb::php::result_get(execute_data, return_value, "outcome", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_ExecutionResult, getReason)
{
    jb::php::result_get(execute_data, return_value, "reason", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_ExecutionResult, getErrorCode)
{
    jb::php::result_get(execute_data, return_value, "error_code", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_ExecutionResult, getOutcomeUnknown)
{
    jb::php::result_get(execute_data, return_value, "outcome_unknown", jb::php::snapshot::Kind::Boolean);
}

ZEND_METHOD(JobU_CliExecutionResult, getExitCode)
{
    jb::php::result_get(execute_data, return_value, "exit_code", jb::php::snapshot::Kind::Integer, 0, 255);
}

ZEND_METHOD(JobU_CliExecutionResult, getSignal)
{
    jb::php::result_get(execute_data, return_value, "signal", jb::php::snapshot::Kind::Integer, 1, INT32_MAX);
}

ZEND_METHOD(JobU_CliExecutionResult, getCaptureLost)
{
    jb::php::result_get(execute_data, return_value, "capture_lost", jb::php::snapshot::Kind::Boolean);
}

ZEND_METHOD(JobU_HttpExecutionResult, getStatus)
{
    jb::php::result_get(execute_data, return_value, "status", jb::php::snapshot::Kind::Integer, 100, 599);
}

ZEND_METHOD(JobU_HttpExecutionResult, getDuration)
{
    jb::php::result_get(execute_data, return_value, "duration_ms", jb::php::snapshot::Kind::Duration);
}

ZEND_METHOD(JobU_HttpExecutionResult, getRedirects)
{
    jb::php::result_get(execute_data, return_value, "redirects", jb::php::snapshot::Kind::Integer, 0, UINT32_MAX);
}

ZEND_METHOD(JobU_HttpExecutionResult, getTlsVerified)
{
    jb::php::result_get(execute_data, return_value, "tls_verified", jb::php::snapshot::Kind::Boolean);
}

ZEND_METHOD(JobU_HttpExecutionResult, getErrorCategory)
{
    jb::php::result_get(execute_data, return_value, "error_category", jb::php::snapshot::Kind::String);
}

ZEND_METHOD(JobU_HttpExecutionResult, getRetryAfterAccepted)
{
    jb::php::result_get(execute_data,
                        return_value,
                        "accepted",
                        jb::php::snapshot::Kind::Boolean,
                        0,
                        INT64_MAX,
                        "retry_after");
}

ZEND_METHOD(JobU_HttpExecutionResult, getRetryAfterClamped)
{
    jb::php::result_get(execute_data,
                        return_value,
                        "clamped",
                        jb::php::snapshot::Kind::Boolean,
                        0,
                        INT64_MAX,
                        "retry_after");
}

ZEND_METHOD(JobU_ExecutionResult, has)
{
    jb::php::generic_get(execute_data, return_value, true);
}

ZEND_METHOD(JobU_ExecutionResult, get)
{
    jb::php::generic_get(execute_data, return_value, false);
}

ZEND_METHOD(JobU_ExecutionResult, getCapturedBytes)
{
    jb::php::capture_get(execute_data, return_value, "captured_bytes", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_ExecutionResult, getTotalBytes)
{
    jb::php::capture_get(execute_data, return_value, "total_bytes", jb::php::snapshot::Kind::Integer);
}

ZEND_METHOD(JobU_ExecutionResult, isTruncated)
{
    jb::php::capture_get(execute_data, return_value, "truncated", jb::php::snapshot::Kind::Boolean);
}
