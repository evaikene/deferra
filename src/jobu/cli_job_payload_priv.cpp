#include "cli_job_payload_priv.hpp"

#include "attempt.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu::detail {

namespace {

template <typename T>
using DecodeResult = jb::core::Result<T, JobPayloadIssue>;

constexpr std::size_t kCanonicalUuidBytes{36};
constexpr std::size_t kMaximumAttemptDigits{std::numeric_limits<AttemptNumber>::digits10 + 1U};

constexpr auto kReservedEnvironmentNames = std::array{
    std::string_view{"JOBU_JOB_ID"},
    std::string_view{"JOBU_RUN_ID"},
    std::string_view{"JOBU_ATTEMPT"},
};

auto member(jb::core::JsonValue::Object const& object, std::string_view name) -> jb::core::JsonValue const*
{
    auto const iterator = object.find(name);
    return iterator == object.end() ? nullptr : &iterator->second;
}

auto contains_nul(std::string_view text) -> bool
{
    return text.find('\0') != std::string_view::npos;
}

auto valid_environment_name(std::string_view name) -> bool
{
    auto const letter = [](char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'; };
    if (name.empty() || !letter(name.front())) {
        return false;
    }
    for (char ch : name) {
        if (!letter(ch) && (ch < '0' || ch > '9')) {
            return false;
        }
    }
    return true;
}

auto reserved_environment_name(std::string_view name) -> bool
{
    return std::ranges::find(kReservedEnvironmentNames, name) != kReservedEnvironmentNames.end();
}

/// Bound each increment before addition because callers may construct JsonValue trees without using the JSON parser.
auto add_bytes(std::size_t& total, std::size_t amount, std::size_t limit) -> bool
{
    if (total > limit || amount > limit - total) {
        return false;
    }
    total += amount;
    return true;
}

auto decode_command(jb::core::JsonValue::Object const& object) -> DecodeResult<std::string>
{
    auto const* command = member(object, "command");
    if (command == nullptr || !command->is_string() || command->as_string().empty()) {
        return DecodeResult<std::string>::failure(JobPayloadIssue::MissingCommand);
    }

    auto const& text = command->as_string();
    if (text.size() > maximum_cli_path_bytes || contains_nul(text) ||
        (text.front() != '/' && text.find('/') != std::string::npos)) {
        return DecodeResult<std::string>::failure(JobPayloadIssue::InvalidCommand);
    }
    return DecodeResult<std::string>::success(text);
}

auto decode_arguments(jb::core::JsonValue const* value) -> DecodeResult<std::vector<std::string>>
{
    auto arguments = std::vector<std::string>{};
    if (value == nullptr) {
        return DecodeResult<std::vector<std::string>>::success(std::move(arguments));
    }
    if (!value->is_array() || value->as_array().size() > maximum_cli_arguments) {
        return DecodeResult<std::vector<std::string>>::failure(JobPayloadIssue::InvalidArguments);
    }

    arguments.reserve(value->as_array().size());
    for (auto const& argument : value->as_array()) {
        if (!argument.is_string() || contains_nul(argument.as_string())) {
            return DecodeResult<std::vector<std::string>>::failure(JobPayloadIssue::InvalidArguments);
        }
        arguments.push_back(argument.as_string());
    }
    return DecodeResult<std::vector<std::string>>::success(std::move(arguments));
}

auto decode_working_directory(jb::core::JsonValue const* value) -> DecodeResult<std::string>
{
    if (value == nullptr) {
        return DecodeResult<std::string>::success("/");
    }
    if (!value->is_string()) {
        return DecodeResult<std::string>::failure(JobPayloadIssue::InvalidWorkingDirectory);
    }

    auto const& directory = value->as_string();
    if (directory.empty() || directory.front() != '/' || directory.size() > maximum_cli_path_bytes ||
        contains_nul(directory)) {
        return DecodeResult<std::string>::failure(JobPayloadIssue::InvalidWorkingDirectory);
    }
    return DecodeResult<std::string>::success(directory);
}

auto decode_environment(jb::core::JsonValue const* value) -> DecodeResult<CliEnvironmentPatch>
{
    auto environment = CliEnvironmentPatch{};
    if (value == nullptr) {
        return DecodeResult<CliEnvironmentPatch>::success(std::move(environment));
    }
    if (!value->is_object() || value->as_object().size() > maximum_cli_environment_entries) {
        return DecodeResult<CliEnvironmentPatch>::failure(JobPayloadIssue::InvalidEnvironment);
    }

    for (auto const& [name, data] : value->as_object()) {
        if (!valid_environment_name(name) || reserved_environment_name(name)) {
            return DecodeResult<CliEnvironmentPatch>::failure(JobPayloadIssue::InvalidEnvironment);
        }
        if (data.is_null()) {
            environment.emplace(name, std::nullopt);
            continue;
        }
        if (!data.is_string() || contains_nul(data.as_string())) {
            return DecodeResult<CliEnvironmentPatch>::failure(JobPayloadIssue::InvalidEnvironment);
        }
        environment.emplace(name, data.as_string());
    }
    return DecodeResult<CliEnvironmentPatch>::success(std::move(environment));
}

auto validate_bare_command_path(std::string_view command, CliEnvironmentPatch const& environment) -> JobPayloadIssue
{
    if (command.front() == '/') {
        return JobPayloadIssue::None;
    }

    auto const path = environment.find("PATH");
    if (path == environment.end() || !path->second) {
        return JobPayloadIssue::InvalidPath;
    }

    // Mirror Process candidate accounting without depending on its private implementation.
    std::string_view remaining{*path->second};
    std::size_t      entries{0};
    std::size_t      bytes{0};
    while (true) {
        auto const colon     = remaining.find(':');
        auto const directory = remaining.substr(0, colon);
        if (directory.empty() || directory.front() != '/' || entries == maximum_cli_path_entries) {
            return JobPayloadIssue::InvalidPath;
        }

        auto const separator_bytes = directory.back() == '/' ? std::size_t{0} : std::size_t{1};
        if (!add_bytes(bytes, directory.size(), maximum_cli_path_candidate_bytes) ||
            !add_bytes(bytes, separator_bytes, maximum_cli_path_candidate_bytes) ||
            !add_bytes(bytes, command.size(), maximum_cli_path_candidate_bytes) ||
            !add_bytes(bytes, 1, maximum_cli_path_candidate_bytes)) {
            return JobPayloadIssue::InvalidPath;
        }
        ++entries;

        if (colon == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(colon + 1U);
    }
    return JobPayloadIssue::None;
}

auto decode_expected_exit_codes(jb::core::JsonValue const* value) -> DecodeResult<CliExpectedExitCodes>
{
    auto expected = CliExpectedExitCodes{};
    if (value == nullptr) {
        expected.set(0);
        return DecodeResult<CliExpectedExitCodes>::success(expected);
    }
    if (!value->is_array() || value->as_array().size() > expected.size()) {
        return DecodeResult<CliExpectedExitCodes>::failure(JobPayloadIssue::InvalidExpectedExitCodes);
    }

    for (auto const& code : value->as_array()) {
        if (!code.is_uint() || code.as_uint() >= expected.size() || expected.test(code.as_uint())) {
            return DecodeResult<CliExpectedExitCodes>::failure(JobPayloadIssue::InvalidExpectedExitCodes);
        }
        expected.set(code.as_uint());
    }
    return DecodeResult<CliExpectedExitCodes>::success(expected);
}

auto prepared_request_fits(CliJobPayload const& payload) -> bool
{
    // This duplicates Process's deterministic argv/envp formula intentionally. JobU also reserves its maximum
    // metadata values so every accepted definition remains under the Process bound when concrete IDs are injected.
    std::size_t bytes{2U * sizeof(char*)};
    auto const  add_string = [&bytes](std::size_t size) {
        return add_bytes(bytes, size, maximum_cli_prepared_request_bytes) &&
               add_bytes(bytes, 1U + sizeof(char*), maximum_cli_prepared_request_bytes);
    };
    auto const add_environment = [&bytes, &add_string](std::string_view name, std::size_t value_size) {
        return add_bytes(bytes, name.size(), maximum_cli_prepared_request_bytes) &&
               add_bytes(bytes, 1U, maximum_cli_prepared_request_bytes) && add_string(value_size);
    };

    if (!add_string(payload.command.size())) {
        return false;
    }
    for (auto const& argument : payload.arguments) {
        if (!add_string(argument.size())) {
            return false;
        }
    }
    for (auto const& [name, value] : payload.environment) {
        if (value && !add_environment(name, value->size())) {
            return false;
        }
    }

    return add_environment("JOBU_JOB_ID", kCanonicalUuidBytes) && add_environment("JOBU_RUN_ID", kCanonicalUuidBytes) &&
           add_environment("JOBU_ATTEMPT", kMaximumAttemptDigits);
}

} // namespace

auto decode_cli_job_payload(jb::core::JsonValue const& payload) -> DecodeResult<CliJobPayload>
{
    if (!payload.is_object()) {
        return DecodeResult<CliJobPayload>::failure(JobPayloadIssue::NotObject);
    }
    auto const& object = payload.as_object();

    auto command = decode_command(object);
    if (!command) {
        return DecodeResult<CliJobPayload>::failure(command.error());
    }
    auto arguments = decode_arguments(member(object, "arguments"));
    if (!arguments) {
        return DecodeResult<CliJobPayload>::failure(arguments.error());
    }
    auto working_directory = decode_working_directory(member(object, "working_directory"));
    if (!working_directory) {
        return DecodeResult<CliJobPayload>::failure(working_directory.error());
    }
    auto environment = decode_environment(member(object, "environment"));
    if (!environment) {
        return DecodeResult<CliJobPayload>::failure(environment.error());
    }

    auto const path_issue = validate_bare_command_path(*command, *environment);
    if (path_issue != JobPayloadIssue::None) {
        return DecodeResult<CliJobPayload>::failure(path_issue);
    }

    auto expected_exit_codes = decode_expected_exit_codes(member(object, "expected_exit_codes"));
    if (!expected_exit_codes) {
        return DecodeResult<CliJobPayload>::failure(expected_exit_codes.error());
    }

    auto decoded = CliJobPayload{
        .command             = std::move(command).value(),
        .arguments           = std::move(arguments).value(),
        .working_directory   = std::move(working_directory).value(),
        .environment         = std::move(environment).value(),
        .expected_exit_codes = std::move(expected_exit_codes).value(),
    };
    if (!prepared_request_fits(decoded)) {
        return DecodeResult<CliJobPayload>::failure(JobPayloadIssue::PreparedRequestTooLarge);
    }
    return DecodeResult<CliJobPayload>::success(std::move(decoded));
}

} // namespace jb::jobu::detail
