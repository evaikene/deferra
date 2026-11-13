#include "client_binding.hpp"
#include "operation.hpp"
#include "page_binding.hpp"
#include "resource_state.hpp"
#include "result_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/core/byte_buffer.hpp>
#include <jb/jobu/attribute.hpp>
#include <jb/jobu/secret.hpp>
#include <phpcxx/arguments.hpp>

#include <string>
#include <utility>
#include <variant>

namespace jb::php {
namespace {
auto secret_name(::php::Arguments const& args) -> ::php::Result<std::string>
{
    auto value = args.string(1);
    if (!value) {
        return value.error();
    }
    auto name = value.value().bytes();
    if (name.size() > 128 || !jobu::is_valid_attribute_name(name)) {
        return snapshot::invalid("Invalid secret name");
    }
    return std::string(name);
}

auto set_secret(zend_execute_data* frame) -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(2, 3);
    if (!count) {
        return count.error();
    }
    auto client = native_client(Z_OBJ(frame->This));
    if (!client) {
        return client.error();
    }

    // Own the name before value coercion can execute PHP. Secret bytes live only
    // in this call's frozen request; result objects contain metadata exclusively.
    auto name = secret_name(args);
    if (!name) {
        return name.error();
    }
    auto value = args.string(2);
    if (!value) {
        return value.error();
    }
    if (value.value().bytes().size() > 65'536) {
        return snapshot::invalid("Secret value must contain at most 65536 bytes");
    }
    auto bytes = core::as_bytes(value.value().bytes());
    auto request =
        jobu::SetSecretRequest{.name = std::move(name).value(), .value = core::ByteBuffer(bytes.begin(), bytes.end())};
    auto deadline = operation_deadline(*client.value(), args, 3, start);
    if (!deadline) {
        return deadline.error();
    }
    auto operation = Operation::bind("secret.set", &jobu::ControlClient::set_secret, std::move(request));
    if (!operation) {
        return snapshot::invalid("Invalid secret request");
    }
    auto reply = resource_call(::php::ValueView(&frame->This), deadline.value(), operation.value());
    if (!reply) {
        return reply.error();
    }
    auto* metadata = std::get_if<jobu::SecretMetadata>(&reply.value());
    if (!metadata) {
        return snapshot::invalid("Expected secret metadata");
    }
    return secret_metadata_to_php(*metadata);
}

auto list_secrets(zend_execute_data* frame) -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(0, 2);
    if (!count) {
        return count.error();
    }
    auto client = native_client(Z_OBJ(frame->This));
    if (!client) {
        return client.error();
    }
    auto limit = args.size() >= 1 ? args.integer(1) : ::php::Result<zend_long>(100);
    if (!limit) {
        return limit.error();
    }
    if (limit.value() < 1 || limit.value() > 200) {
        return snapshot::invalid("Secret list limit must be between 1 and 200");
    }
    auto deadline = operation_deadline(*client.value(), args, 2, start);
    if (!deadline) {
        return deadline.error();
    }

    // The existing source owns name-keyset continuation and metadata projection.
    auto source = secret_page_source({.limit = static_cast<std::size_t>(limit.value())});
    return make_page(::php::ValueView(&frame->This), std::move(source), deadline.value());
}

auto delete_secret(zend_execute_data* frame) -> ::php::Result<::php::Value>
{
    auto             start = core::Clock::now();
    ::php::Arguments args(frame);
    auto             count = args.count(1, 2);
    if (!count) {
        return count.error();
    }
    auto client = native_client(Z_OBJ(frame->This));
    if (!client) {
        return client.error();
    }
    auto name = secret_name(args);
    if (!name) {
        return name.error();
    }
    auto deadline = operation_deadline(*client.value(), args, 2, start);
    if (!deadline) {
        return deadline.error();
    }

    auto operation = Operation::bind("secret.delete", &jobu::ControlClient::delete_secret, std::move(name).value());
    auto reply     = resource_call(::php::ValueView(&frame->This), deadline.value(), operation);
    if (!reply) {
        return reply.error();
    }
    if (!std::holds_alternative<jobu::EmptyReply>(reply.value())) {
        return snapshot::invalid("Expected an empty secret deletion reply");
    }
    zval slot;
    ZVAL_NULL(&slot);
    return ::php::Value::adopt(slot);
}
} // namespace
} // namespace jb::php

ZEND_METHOD(JobU_Client, setSecret)
{
    jb::php::snapshot::return_value(jb::php::set_secret(execute_data), return_value);
}

ZEND_METHOD(JobU_Client, listSecrets)
{
    jb::php::snapshot::return_value(jb::php::list_secrets(execute_data), return_value);
}

ZEND_METHOD(JobU_Client, deleteSecret)
{
    jb::php::snapshot::return_value(jb::php::delete_secret(execute_data), return_value);
}
