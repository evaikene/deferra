#include "cli_payload_binding.hpp"
#include "value_binding.hpp"
#include "value_snapshot.hpp"

#include <jb/core/command_argv.hpp>
#include <jb/core/text_validation.hpp>
#include <phpcxx/arguments.hpp>
#include <phpcxx/conversion.hpp>
#include <phpcxx/exception.hpp>
#include <phpcxx/object_binding.hpp>

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jb::php {
namespace {

// Keep the client-side limits aligned with the CLI protocol. The daemon remains
// authoritative, especially for sizes and UTF-8 after symbolic secret expansion.
constexpr std::size_t path_limit        = 4096;
constexpr std::size_t argument_limit    = 1024;
constexpr std::size_t environment_limit = 256;
constexpr std::size_t request_limit     = std::size_t{256} * 1024;

struct Text {
    std::string value;
    bool        secret                         = false;
    auto        operator<=>(Text const&) const = default;
};

struct Payload {
    std::string                                             executable;
    std::vector<Text>                                       arguments;
    std::map<std::string, std::optional<Text>, std::less<>> environment;
    std::string                                             directory = "/";
    std::vector<std::uint64_t>                              exits{0};
    auto                                                    operator<=>(Payload const&) const = default;
};

struct Configuration {
    explicit Configuration(Payload value) noexcept
        : value(std::move(value))
    {}

    auto trace(::php::GcVisitor& visitor) const noexcept -> void { visitor.add(owner.view()); }

    Payload                value;
    ::php::Value           owner;
    std::function<void()>  changed;
    ConfigurationEditCheck editable;
};

struct Policy : ::php::ObjectBindingPolicy<Configuration> {
    static constexpr bool cloneable = true;

    static auto clone(Configuration const& source) noexcept -> ::php::Result<Configuration>
    {
        // Only native values cross the clone boundary, never ownership or notification.
        return Configuration(source.value);
    }

    static auto compare(Configuration const& left, Configuration const& right) noexcept -> int
    {
        auto order = left.value <=> right.value;
        if (std::is_lt(order)) {
            return -1;
        }
        return std::is_gt(order) ? 1 : 0;
    }
};

static_assert(Policy::cloneable, "CLI payload clones must detach from their owner");

using Binding = ::php::ObjectBinding<Configuration, Policy>;

auto invalid(std::string message = "Invalid CLI payload value") -> ::php::Error
{
    return {.kind = ::php::ErrorKind::Value, .message = std::move(message)};
}

auto valid_text(std::string_view text) -> bool
{
    return text.find('\0') == std::string_view::npos && core::is_valid_utf8(text);
}

auto valid_executable(std::string_view text) -> bool
{
    return !text.empty() && text.size() <= path_limit && valid_text(text) &&
           (text.front() == '/' || text.find('/') == std::string_view::npos);
}

auto valid_name(std::string_view name) -> bool
{
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (name.empty() || !letter(name.front()) || name == "JOBU_JOB_ID" || name == "JOBU_RUN_ID" ||
        name == "JOBU_ATTEMPT") {
        return false;
    }
    for (char c : name) {
        if (!letter(c) && (c < '0' || c > '9')) {
            return false;
        }
    }
    return true;
}

auto read_text(::php::ValueView input) -> ::php::Result<Text>
{
    if (input.dereference().is_string()) {
        auto text = ::php::as_string(input).value().bytes();
        if (!valid_text(text)) {
            return invalid("CLI text must be UTF-8 without NUL");
        }
        return Text{.value = std::string(text)};
    }
    auto secret = secret_name_from_php(input);
    if (!secret) {
        return secret.error();
    }
    return Text{.value = std::move(secret).value(), .secret = true};
}

auto read_arguments(::php::ValueView input) -> ::php::Result<std::vector<Text>>
{
    auto array = ::php::as_array(input);
    if (!array) {
        return array.error();
    }
    auto* table = const_cast<zend_array*>(array.value().get());
    if (!zend_array_is_list(table) || zend_hash_num_elements(table) > argument_limit) {
        return invalid("Arguments must be a list of at most 1024 values");
    }
    std::vector<Text> result;
    for (auto const entry : array.value()) {
        auto text = read_text(entry.value);
        if (!text) {
            return text.error();
        }
        result.push_back(std::move(text).value());
    }
    return result;
}

auto read_exits(::php::ValueView input) -> ::php::Result<std::vector<std::uint64_t>>
{
    auto array = ::php::as_array(input);
    if (!array) {
        return array.error();
    }
    if (!zend_array_is_list(const_cast<zend_array*>(array.value().get())) || array.value().size() > 256) {
        return invalid("Expected exit codes must be a list of distinct integers from 0 to 255");
    }
    bool seen[256]{};
    for (auto const entry : array.value()) {
        auto code = ::php::as_integer(entry.value);
        if (!code) {
            return code.error();
        }
        if (code.value() < 0 || code.value() > 255 || seen[code.value()]) {
            return invalid("Expected exit codes must be distinct integers from 0 to 255");
        }
        seen[code.value()] = true;
    }
    std::vector<std::uint64_t> result;
    for (std::uint64_t code = 0; code != 256; ++code) {
        if (seen[code]) {
            result.push_back(code);
        }
    }
    return result;
}

struct Command {
    std::string       executable;
    std::vector<Text> arguments;
};

auto read_command(::php::Arguments const& args) -> ::php::Result<Command>
{
    auto input = args.string(1);
    if (!input) {
        return input.error();
    }
    if (!valid_text(input.value().bytes())) {
        return invalid("Command must be UTF-8 without NUL");
    }
    auto parsed = core::parse_command_argv(input.value().bytes());
    if (!parsed || !valid_executable(parsed->front())) {
        return invalid("Invalid command or executable");
    }
    std::vector<Text> arguments;
    for (std::size_t i = 1; i < parsed->size(); ++i) {
        arguments.push_back({.value = std::move((*parsed)[i])});
    }
    if (args.size() > 1) {
        auto extra = read_arguments(args.get(2));
        if (!extra) {
            return extra.error();
        }
        for (auto& text : extra.value()) {
            arguments.push_back(std::move(text));
        }
    }
    if (arguments.size() > argument_limit) {
        return invalid("CLI arguments exceed 1024 values");
    }
    return Command{.executable = std::move(parsed->front()), .arguments = std::move(arguments)};
}

auto commit(Configuration& target, Payload candidate) -> void
{
    if (target.value == candidate) {
        return;
    }
    target.value = std::move(candidate);
    if (target.changed) {
        target.changed();
    }
}

// PATH is a cross-field constraint: a caller may construct a bare executable
// before adding its explicit PATH. Enforce it only at the submission boundary.
auto validate_path(Payload const& payload) -> ::php::Result<void>
{
    if (payload.executable.front() == '/') {
        return {};
    }
    auto found = payload.environment.find("PATH");
    if (found == payload.environment.end() || !found->second || found->second->secret) {
        return invalid("A bare executable requires an explicit literal PATH");
    }
    std::string_view remaining = found->second->value;
    std::size_t      count     = 0;
    std::size_t      bytes     = 0;
    for (;;) {
        auto colon     = remaining.find(':');
        auto directory = remaining.substr(0, colon);
        if (directory.empty() || directory.front() != '/' || ++count > 256) {
            return invalid("PATH must contain at most 256 absolute nonempty directories");
        }
        bytes += directory.size() + (directory.back() == '/' ? 0U : 1U) + payload.executable.size() + 1U;
        if (bytes > request_limit) {
            return invalid("PATH candidates exceed the CLI size limit");
        }
        if (colon == std::string_view::npos) {
            return {};
        }
        remaining.remove_prefix(colon + 1);
    }
}

auto text_json(Text const& text) -> core::JsonValue
{
    if (text.secret) {
        return {.data = core::JsonValue::Object{{"secret", {.data = text.value}}}};
    }
    return {.data = text.value};
}

auto export_payload(Payload const& value) -> ::php::Result<core::JsonValue>
{
    auto path = validate_path(value);
    if (!path) {
        return path.error();
    }
    core::JsonValue::Array  arguments;
    core::JsonValue::Object environment;
    core::JsonValue::Array  exits;

    // Reserve argv/envp pointers and maximum injected metadata. Unresolved secret
    // bytes count as zero here; the daemon repeats accounting after resolution.
    std::size_t prepared   = 2 * sizeof(char*);
    auto        add_string = [&prepared](std::size_t bytes) { prepared += bytes + 1 + sizeof(char*); };
    add_string(value.executable.size());
    for (auto const& text : value.arguments) {
        arguments.push_back(text_json(text));
        add_string(text.secret ? 0 : text.value.size());
    }
    for (auto const& [name, text] : value.environment) {
        environment.emplace(name, text ? text_json(*text) : core::JsonValue{});
        if (text) {
            add_string(name.size() + 1 + (text->secret ? 0 : text->value.size()));
        }
    }
    add_string(std::string_view("JOBU_JOB_ID").size() + 1 + 36);
    add_string(std::string_view("JOBU_RUN_ID").size() + 1 + 36);
    add_string(std::string_view("JOBU_ATTEMPT").size() + 1 + 20);
    for (auto code : value.exits) {
        exits.push_back({.data = code});
    }
    core::JsonValue result{
        .data = core::JsonValue::Object{{"command", {.data = value.executable}},
                                        {"arguments", {.data = std::move(arguments)}},
                                        {"environment", {.data = std::move(environment)}},
                                        {"working_directory", {.data = value.directory}},
                                        {"expected_exit_codes", {.data = std::move(exits)}}}
    };
    auto encoded = core::serialize_json(result);
    if (!encoded || encoded->size() > request_limit || prepared > request_limit) {
        return invalid("CLI payload exceeds the size limit");
    }
    return result;
}

auto native(::php::ValueView input) -> ::php::Result<Configuration*>
{
    auto value = input.dereference();
    if (!value.is_object()) {
        return ::php::Error{.kind = ::php::ErrorKind::Type, .message = "Expected CliPayload"};
    }
    return Binding::get(Z_OBJ_P(value.get()));
}

auto loaded_text(core::JsonValue const& value) -> ::php::Result<Text>
{
    if (value.is_string()) {
        return Text{.value = value.as_string()};
    }
    auto const* secret = snapshot::field(value, "secret");
    if (!secret || !secret->is_string()) {
        return invalid("Invalid payload text in server reply");
    }
    return Text{.value = secret->as_string(), .secret = true};
}

auto php_text(Text const& text) -> ::php::Result<::php::Value>
{
    if (text.secret) {
        return secret_name_to_php(text.value);
    }
    zval slot{};
    ZVAL_STRINGL(&slot, text.value.data(), text.value.size());
    return ::php::Value::adopt(slot);
}

enum class Field : std::uint8_t {
    Executable,
    Arguments,
    Environment,
    Directory,
    Exits
};
enum class Edit : std::uint8_t {
    Command,
    Executable,
    Arguments,
    Argument,
    Environment,
    Unset,
    Clear,
    Directory,
    Exits
};

// Comparison happens after argument conversion: PHP coercion may have performed
// a nested edit. Only this setter's field is replaced, using its current value.
template <typename T>
auto update_field(T& target, T value) -> bool
{
    if (target == value) {
        return false;
    }
    target = std::move(value);
    return true;
}

auto edit(::php::Arguments const& args, Edit action, Payload& target, ConfigurationEditCheck const& editable)
    -> ::php::Result<bool>
{
    if (action == Edit::Command) {
        auto parsed = read_command(args);
        if (!parsed) {
            return parsed.error();
        }
        auto allowed = check_configuration_edit(editable);
        if (!allowed) {
            return allowed.error();
        }
        auto& command = parsed.value();
        if (target.executable == command.executable && target.arguments == command.arguments) {
            return false;
        }

        // Both fields have been validated. No PHP or notification runs between
        // their moves, so observers see only the complete executable/argv pair.
        target.executable = std::move(command.executable);
        target.arguments  = std::move(command.arguments);
        return true;
    }
    if (action == Edit::Arguments) {
        auto parsed = read_arguments(args.get(1));
        if (!parsed) {
            return parsed.error();
        }
        return update_field(target.arguments, std::move(parsed).value());
    }
    if (action == Edit::Argument) {
        auto parsed = read_text(args.get(1));
        if (!parsed) {
            return parsed.error();
        }
        if (target.arguments.size() == argument_limit) {
            return invalid("CLI arguments exceed 1024 values");
        }
        target.arguments.push_back(std::move(parsed).value());
        return true;
    }
    if (action == Edit::Exits) {
        auto parsed = read_exits(args.get(1));
        if (!parsed) {
            return parsed.error();
        }
        return update_field(target.exits, std::move(parsed).value());
    }

    auto input = args.string(1);
    if (!input) {
        return input.error();
    }
    auto allowed = check_configuration_edit(editable);
    if (!allowed) {
        return allowed.error();
    }
    auto text = input.value().bytes();
    if (action == Edit::Executable) {
        if (!valid_executable(text)) {
            return invalid("Invalid CLI executable");
        }
        return update_field(target.executable, std::string(text));
    }
    if (action == Edit::Directory) {
        if (text.empty() || text.front() != '/' || text.size() > path_limit || !valid_text(text)) {
            return invalid("Working directory must be an absolute UTF-8 path without NUL, at most 4096 bytes");
        }
        return update_field(target.directory, std::string(text));
    }

    if (!valid_name(text)) {
        return invalid("Invalid or reserved environment name");
    }
    if (action == Edit::Clear) {
        return target.environment.erase(std::string(text)) != 0;
    }
    if (!target.environment.contains(text) && target.environment.size() == environment_limit) {
        return invalid("Environment exceeds 256 entries");
    }
    std::optional<Text> value;
    if (action == Edit::Environment) {
        auto parsed = read_text(args.get(2));
        if (!parsed) {
            return parsed.error();
        }
        if (parsed.value().secret && (text == "PATH" || text.starts_with("JOBU_"))) {
            return invalid("PATH and JOBU_ environment values cannot be secret references");
        }
        value = std::move(parsed).value();
    }

    // Set/unset owns one environment entry, not the entire map. Absence is
    // distinct from an existing null removal, including for no-op detection.
    auto found = target.environment.find(text);
    if (found != target.environment.end() && found->second == value) {
        return false;
    }
    target.environment.insert_or_assign(std::string(text), std::move(value));
    return true;
}

auto construct(zend_execute_data* frame) noexcept -> void
{
    ::php::Arguments args(frame);
    auto             count = args.count(1, 2);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto parsed = read_command(args);
    if (!parsed) {
        ::php::raise(parsed.error());
        return;
    }
    Payload candidate;
    candidate.executable = std::move(parsed.value().executable);
    candidate.arguments  = std::move(parsed.value().arguments);
    auto result          = Binding::construct(Z_OBJ(frame->This), std::move(candidate));
    if (!result) {
        ::php::raise(result.error());
    }
}

auto mutate(zend_execute_data* frame, Edit action) noexcept -> void
{
    ::php::Arguments args(frame);
    auto             count =
        args.count(action == Edit::Environment ? 2 : 1, action == Edit::Environment || action == Edit::Command ? 2 : 1);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto target = Binding::get(Z_OBJ(frame->This));
    if (!target) {
        ::php::raise(target.error());
        return;
    }

    // Each edit validates into temporaries before touching its owned fields.
    // Successful nested edits during coercion remain independent, even if the
    // outer edit fails. Notify only for this edit's actual committed change.
    auto allowed = check_configuration_edit(target.value()->editable);
    if (!allowed) {
        ::php::raise(allowed.error());
        return;
    }
    auto changed = edit(args, action, target.value()->value, target.value()->editable);
    if (!changed) {
        ::php::raise(changed.error());
        return;
    }
    if (changed.value() && target.value()->changed) {
        target.value()->changed();
    }
}

auto read(zend_execute_data* frame, zval* output, Field field) noexcept -> void
{
    auto count = ::php::Arguments(frame).count(0, 0);
    if (!count) {
        ::php::raise(count.error());
        return;
    }
    auto target = Binding::get(Z_OBJ(frame->This));
    if (!target) {
        ::php::raise(target.error());
        return;
    }
    auto const& value = target.value()->value;
    if (field == Field::Executable || field == Field::Directory) {
        auto const& text = field == Field::Executable ? value.executable : value.directory;
        ZVAL_STRINGL(output, text.data(), text.size());
        return;
    }

    zval slot{};
    array_init(&slot);
    auto  owner = ::php::Value::adopt(slot);
    auto* array = Z_ARR_P(owner.get());
    if (field == Field::Exits) {
        for (auto code : value.exits) {
            zval item{};
            ZVAL_LONG(&item, static_cast<zend_long>(code));
            zend_hash_next_index_insert(array, &item);
        }
    }
    else if (field == Field::Arguments) {
        for (auto const& text : value.arguments) {
            auto item = php_text(text);
            if (!item) {
                ::php::raise(item.error());
                return;
            }
            auto released = item.value().release();
            zend_hash_next_index_insert(array, &released);
        }
    }
    else {
        for (auto const& [name, text] : value.environment) {
            auto item = text ? php_text(*text) : ::php::Result<::php::Value>(::php::Value::null());
            if (!item) {
                ::php::raise(item.error());
                return;
            }
            auto released = item.value().release();
            zend_hash_str_update(array, name.data(), name.size(), &released);
        }
    }
    *output = owner.release();
}

} // namespace

auto bind_cli_payload_class(zend_class_entry* entry) -> bool
{
    entry->ce_flags |= ZEND_ACC_NO_DYNAMIC_PROPERTIES;
    return static_cast<bool>(Binding::bind(entry));
}

auto equal_cli_payload(::php::ValueView left, ::php::ValueView right) -> bool
{
    return native(left).value()->value == native(right).value()->value;
}

auto cli_payload_to_php(core::JsonValue const& value) -> ::php::Result<::php::Value>
{
    // Read templates directly: tokenization and secret resolution would change
    // the server snapshot. Optional fields retain their protocol defaults.
    Payload     payload;
    auto const* command = snapshot::field(value, "command");
    if (!command || !command->is_string() || !valid_executable(command->as_string())) {
        return invalid("Invalid CLI executable in server reply");
    }
    payload.executable = command->as_string();
    if (auto const* arguments = snapshot::field(value, "arguments")) {
        if (!arguments->is_array()) {
            return invalid("Invalid CLI arguments in server reply");
        }
        for (auto const& item : arguments->as_array()) {
            auto text = loaded_text(item);
            if (!text) {
                return text.error();
            }
            payload.arguments.push_back(std::move(text).value());
        }
    }
    if (auto const* environment = snapshot::field(value, "environment")) {
        if (!environment->is_object()) {
            return invalid("Invalid CLI environment in server reply");
        }
        for (auto const& [name, item] : environment->as_object()) {
            if (item.is_null()) {
                payload.environment.emplace(name, std::nullopt);
                continue;
            }
            auto text = loaded_text(item);
            if (!text) {
                return text.error();
            }
            payload.environment.emplace(name, std::move(text).value());
        }
    }
    if (auto const* directory = snapshot::field(value, "working_directory")) {
        if (!directory->is_string()) {
            return invalid("Invalid CLI directory in server reply");
        }
        payload.directory = directory->as_string();
    }
    if (auto const* exits = snapshot::field(value, "expected_exit_codes")) {
        auto converted = snapshot::to_php(*exits);
        if (!converted) {
            return converted.error();
        }
        auto codes = read_exits(converted.value().view());
        if (!codes) {
            return codes.error();
        }
        payload.exits = std::move(codes).value();
    }
    auto checked = export_payload(payload);
    if (!checked) {
        return checked.error();
    }

    zval slot{};
    object_init_ex(&slot, snapshot::entry("jobu\\clipayload"));
    auto owner       = ::php::Value::adopt(slot);
    auto constructed = Binding::construct(Z_OBJ_P(owner.get()), std::move(payload));
    if (!constructed) {
        return constructed.error();
    }
    return owner;
}

auto cli_payload_from_php(::php::ValueView value) -> ::php::Result<core::JsonValue>
{
    auto source = native(value);
    if (!source) {
        return source.error();
    }
    return export_payload(source.value()->value);
}

auto replace_cli_payload(zend_object* target, ::php::ValueView source) -> ::php::Result<void>
{
    auto destination = Binding::get(target);
    if (!destination) {
        return destination.error();
    }
    auto supplied = native(source);
    if (!supplied) {
        return supplied.error();
    }
    auto checked = export_payload(supplied.value()->value);
    if (!checked) {
        return checked.error();
    }
    auto allowed = check_configuration_edit(destination.value()->editable);
    if (!allowed) {
        return allowed.error();
    }
    commit(*destination.value(), supplied.value()->value);
    return {};
}

auto prepare_cli_payload_restore(zend_object* target, ::php::ValueView source) -> ::php::Result<ConfigurationRestore>
{
    auto destination = Binding::get(target);
    if (!destination) {
        return destination.error();
    }
    auto supplied = native(source);
    if (!supplied) {
        return supplied.error();
    }
    auto checked = export_payload(supplied.value()->value);
    if (!checked) {
        return checked.error();
    }
    return ConfigurationRestore([target = destination.value(), value = supplied.value()->value]() mutable {
        target->value = std::move(value);
    });
}

auto attach_cli_payload(zend_object*           target,
                        ::php::ValueView       owner,
                        std::function<void()>  changed,
                        ConfigurationEditCheck editable) -> ::php::Result<void>
{
    auto config = Binding::get(target);
    if (!config) {
        return config.error();
    }
    if (!owner.dereference().is_object() || config.value()->owner.view().is_initialized()) {
        return invalid("Payload requires an object owner and may only be attached once");
    }
    config.value()->owner    = ::php::Value::retain(owner.dereference());
    config.value()->changed  = std::move(changed);
    config.value()->editable = std::move(editable);
    return {};
}

} // namespace jb::php

ZEND_METHOD(JobU_CliPayload, __construct)
{
    (void)return_value;
    jb::php::construct(execute_data);
}

ZEND_METHOD(JobU_CliPayload, getExecutable)
{
    jb::php::read(execute_data, return_value, jb::php::Field::Executable);
}

ZEND_METHOD(JobU_CliPayload, getArguments)
{
    jb::php::read(execute_data, return_value, jb::php::Field::Arguments);
}

ZEND_METHOD(JobU_CliPayload, getEnvironment)
{
    jb::php::read(execute_data, return_value, jb::php::Field::Environment);
}

ZEND_METHOD(JobU_CliPayload, getWorkingDirectory)
{
    jb::php::read(execute_data, return_value, jb::php::Field::Directory);
}

ZEND_METHOD(JobU_CliPayload, getExpectedExitCodes)
{
    jb::php::read(execute_data, return_value, jb::php::Field::Exits);
}

ZEND_METHOD(JobU_CliPayload, setCommand)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Command);
}

ZEND_METHOD(JobU_CliPayload, setExecutable)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Executable);
}

ZEND_METHOD(JobU_CliPayload, setArguments)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Arguments);
}

ZEND_METHOD(JobU_CliPayload, addArgument)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Argument);
}

ZEND_METHOD(JobU_CliPayload, setEnvironment)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Environment);
}

ZEND_METHOD(JobU_CliPayload, unsetEnvironment)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Unset);
}

ZEND_METHOD(JobU_CliPayload, clearEnvironment)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Clear);
}

ZEND_METHOD(JobU_CliPayload, setWorkingDirectory)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Directory);
}

ZEND_METHOD(JobU_CliPayload, setExpectedExitCodes)
{
    (void)return_value;
    jb::php::mutate(execute_data, jb::php::Edit::Exits);
}
