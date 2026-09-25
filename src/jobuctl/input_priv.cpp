#include "input_priv.hpp"

#include "control_json.hpp"
#include "framing.hpp"
#include "history_json.hpp"
#include "json.hpp"
#include "management_json.hpp"
#include "secret_json.hpp"
#include "statistics_json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

namespace jb::jobuctl::detail {
namespace {

using namespace jb::core;
using namespace jb::jobu;

auto input_error(std::string code, std::string message) -> Error
{
    return {.category = ErrorCategory::InvalidArgument, .code = std::move(code), .message = std::move(message)};
}

auto read_bounded(std::istream& stream) -> Result<std::string, Error>
{
    // One extra byte distinguishes an oversized file from a file exactly at the RPC body limit.
    auto const limit = jb::rpc::FramingLimits{}.max_body_bytes;
    auto       text  = std::string{};
    auto       block = std::array<char, 8192>{};
    while (text.size() <= limit) {
        auto const count = std::min(block.size(), limit + 1U - text.size());
        stream.read(block.data(), static_cast<std::streamsize>(count));
        text.append(block.data(), static_cast<std::size_t>(stream.gcount()));
        if (stream.bad()) {
            return Result<std::string, Error>::failure(
                input_error("jobuctl.input.read_failed", "Unable to read request input"));
        }
        if (text.size() > limit) {
            return Result<std::string, Error>::failure(
                input_error("jobuctl.input.too_large", "Request input exceeds the RPC body limit"));
        }
        if (stream.eof()) {
            return Result<std::string, Error>::success(std::move(text));
        }
    }
    return Result<std::string, Error>::failure(
        input_error("jobuctl.input.too_large", "Request input exceeds the RPC body limit"));
}

auto read_secret_bytes(std::istream& stream) -> Result<ByteBuffer, Error>
{
    constexpr auto limit = std::size_t{65536};
    auto           bytes = ByteBuffer{};
    auto           block = std::array<char, 8192>{};

    // Read one byte beyond the raw-value limit; neither text decoding nor trimming is appropriate here.
    while (bytes.size() <= limit) {
        auto const count = std::min(block.size(), limit + 1U - bytes.size());
        stream.read(block.data(), static_cast<std::streamsize>(count));
        auto const chunk = as_bytes(std::string_view{block.data(), static_cast<std::size_t>(stream.gcount())});
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());

        if (stream.bad() || (stream.fail() && !stream.eof())) {
            return Result<ByteBuffer, Error>::failure(
                input_error("jobuctl.input.read_failed", "Unable to read secret input"));
        }
        if (bytes.size() > limit) {
            return Result<ByteBuffer, Error>::failure(
                input_error("jobuctl.input.too_large", "Secret input exceeds 65536 bytes"));
        }
        if (stream.eof()) {
            return Result<ByteBuffer, Error>::success(std::move(bytes));
        }
    }
    return Result<ByteBuffer, Error>::failure(
        input_error("jobuctl.input.too_large", "Secret input exceeds 65536 bytes"));
}

template <typename T>
auto request_from(Result<T, Error> decoded) -> Result<CommandRequest, Error>
{
    if (!decoded) {
        return Result<CommandRequest, Error>::failure(
            input_error("jobuctl.input.invalid_params", "Request parameters are invalid"));
    }
    return Result<CommandRequest, Error>::success(CommandRequest{std::move(decoded).value()});
}

auto decode_request(CommandKind kind, JsonValue const& value, AttributeRegistry const& registry)
    -> Result<CommandRequest, Error>
{
    if (kind == CommandKind::SystemInfo) {
        if (!value.as_object().empty()) {
            return Result<CommandRequest, Error>::failure(
                input_error("jobuctl.input.invalid_params", "Request parameters are invalid"));
        }
        return Result<CommandRequest, Error>::success(CommandRequest{});
    }

    switch (kind) {
        case CommandKind::QueueCreate:
            return request_from(create_queue_request_from_json(value, registry));
        case CommandKind::QueueGet:
        case CommandKind::QueueSuspend:
        case CommandKind::QueueResume:
        case CommandKind::QueueDelete:
            return request_from(queue_selector_from_json(value));
        case CommandKind::QueueList:
            return request_from(queue_list_request_from_json(value));
        case CommandKind::QueueUpdate:
            return request_from(update_queue_request_from_json(value, registry));
        case CommandKind::JobCreate:
            return request_from(create_job_request_from_json(value, registry));
        case CommandKind::JobGet:
        case CommandKind::JobSuspend:
        case CommandKind::JobResume:
            return request_from(job_id_from_json(value));
        case CommandKind::JobList:
            return request_from(job_list_request_from_json(value));
        case CommandKind::JobUpdate:
            return request_from(update_job_request_from_json(value, registry));
        case CommandKind::JobMove:
            return request_from(move_job_request_from_json(value));
        case CommandKind::JobDelete:
            return request_from(delete_job_request_from_json(value));
        case CommandKind::JobRunNow:
            return request_from(run_now_request_from_json(value));
        case CommandKind::RunGet:
            return request_from(run_get_request_from_json(value));
        case CommandKind::RunList:
            return request_from(run_list_request_from_json(value));
        case CommandKind::RunCancel:
            return request_from(cancel_run_request_from_json(value));
        case CommandKind::AttemptGet:
            return request_from(attempt_get_request_from_json(value));
        case CommandKind::AttemptList:
            return request_from(attempt_list_request_from_json(value));
        case CommandKind::AttemptOutput:
            return request_from(attempt_output_request_from_json(value));
        case CommandKind::SecretSet:
            return request_from(set_secret_request_from_json(value));
        case CommandKind::SecretList:
            return request_from(secret_list_request_from_json(value));
        case CommandKind::SecretDelete:
            return request_from(secret_delete_request_from_json(value));
        case CommandKind::SystemStats:
            return request_from(system_statistics_request_from_json(value));
        case CommandKind::QueueStats:
            return request_from(queue_statistics_request_from_json(value));
        case CommandKind::ScheduleValidate:
            return request_from(schedule_validate_request_from_json(value));
        case CommandKind::ScheduleNext:
            return request_from(schedule_next_request_from_json(value));
        case CommandKind::SystemInfo:
            break;
    }
    return Result<CommandRequest, Error>::failure(
        input_error("jobuctl.input.invalid_params", "Request parameters are invalid"));
}

} // namespace

auto load_json_object(std::filesystem::path const& path) -> Result<JsonValue, Error>
{
    auto       file   = std::ifstream{};
    auto*      stream = &std::cin;
    auto const stdin  = path == std::filesystem::path{"-"};
    if (!stdin) {
        file.open(path, std::ios::binary);
        if (!file) {
            return Result<JsonValue, Error>::failure(
                input_error("jobuctl.input.open_failed", "Unable to open request file"));
        }
        stream = &file;
    }

    auto text = read_bounded(*stream);
    if (!text) {
        return Result<JsonValue, Error>::failure(std::move(text).error());
    }
    auto value = parse_json(*text);
    if (!value || !value->is_object()) {
        return Result<JsonValue, Error>::failure(
            input_error("jobuctl.input.invalid_json", "Request input must contain one JSON params object"));
    }
    return Result<JsonValue, Error>::success(std::move(*value));
}

auto load_request_file(Command& command, StandardAttributeRegistry const& registry) -> Result<void, Error>
{
    if (!command.request_file) {
        return Result<void, Error>::success();
    }
    auto value = load_json_object(*command.request_file);
    if (!value) {
        return Result<void, Error>::failure(std::move(value).error());
    }
    auto request = decode_request(command.kind, *value, registry);
    if (!request) {
        return Result<void, Error>::failure(std::move(request).error());
    }
    command.request = std::move(request).value();
    return Result<void, Error>::success();
}

auto load_secret_input(Command& command) -> Result<void, Error>
{
    if (!command.secret_input) {
        return Result<void, Error>::success();
    }

    auto  file   = std::ifstream{};
    auto* stream = &std::cin;
    if (command.secret_input->source == SecretInput::Source::File) {
        file.open(command.secret_input->file, std::ios::binary);
        if (!file) {
            return Result<void, Error>::failure(input_error("jobuctl.input.open_failed", "Unable to open secret file"));
        }
        stream = &file;
    }

    auto bytes = read_secret_bytes(*stream);
    if (!bytes) {
        return Result<void, Error>::failure(std::move(bytes).error());
    }
    std::get<SetSecretRequest>(command.request).value = std::move(bytes).value();
    return Result<void, Error>::success();
}

} // namespace jb::jobuctl::detail
