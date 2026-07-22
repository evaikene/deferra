#include "job_validation_priv.hpp"

#include "json.hpp"

#include <cstddef>

namespace jb::jobu::detail {

namespace {

constexpr std::size_t kMaximumJobNameBytes = 256;

auto is_continuation(unsigned char byte) noexcept -> bool
{
    return byte >= 0x80U && byte <= 0xbfU;
}

auto is_valid_utf8(std::string_view text) noexcept -> bool
{
    auto const* bytes = reinterpret_cast<unsigned char const*>(text.data());
    auto        index = std::size_t{0};
    while (index < text.size()) {
        auto const first = bytes[index];
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1 >= text.size() || !is_continuation(bytes[index + 1])) {
                return false;
            }
            index += 2;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2 >= text.size() || !is_continuation(bytes[index + 2])) {
                return false;
            }
            auto const second       = bytes[index + 1];
            auto const valid_second = first == 0xe0U ? second >= 0xa0U && second <= 0xbfU
                                    : first == 0xedU ? second >= 0x80U && second <= 0x9fU
                                                     : is_continuation(second);
            if (!valid_second) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3 >= text.size() || !is_continuation(bytes[index + 2]) || !is_continuation(bytes[index + 3])) {
                return false;
            }
            auto const second       = bytes[index + 1];
            auto const valid_second = first == 0xf0U ? second >= 0x90U && second <= 0xbfU
                                    : first == 0xf4U ? second >= 0x80U && second <= 0x8fU
                                                     : is_continuation(second);
            if (!valid_second) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

auto has_ascii_control(std::string_view text) noexcept -> bool
{
    for (auto const character : text) {
        auto const byte = static_cast<unsigned char>(character);
        if (byte <= 0x1fU || byte == 0x7fU) {
            return true;
        }
    }
    return false;
}

auto member(jb::rpc::JsonValue::Object const& object, std::string_view name) -> jb::rpc::JsonValue const*
{
    auto const iterator = object.find(name);
    return iterator == object.end() ? nullptr : &iterator->second;
}

auto structurally_valid_cli(jb::rpc::JsonValue::Object const& object) -> JobPayloadIssue
{
    auto const* command = member(object, "command");
    if (command == nullptr || !command->is_string() || command->as_string().empty()) {
        return JobPayloadIssue::MissingCommand;
    }
    auto const* arguments = member(object, "arguments");
    if (arguments == nullptr) {
        return JobPayloadIssue::None;
    }
    if (!arguments->is_array()) {
        return JobPayloadIssue::InvalidArguments;
    }
    for (auto const& argument : arguments->as_array()) {
        if (!argument.is_string()) {
            return JobPayloadIssue::InvalidArguments;
        }
    }
    return JobPayloadIssue::None;
}

auto structurally_valid_http(jb::rpc::JsonValue::Object const& object) -> JobPayloadIssue
{
    auto const* url = member(object, "url");
    if (url == nullptr || !url->is_string() || url->as_string().empty()) {
        return JobPayloadIssue::MissingUrl;
    }
    auto const* method = member(object, "method");
    if (method != nullptr && (!method->is_string() || method->as_string().empty())) {
        return JobPayloadIssue::InvalidMethod;
    }
    return JobPayloadIssue::None;
}

} // anonymous namespace

auto is_valid_job_name(std::string_view name) noexcept -> bool
{
    return name.size() <= kMaximumJobNameBytes && is_valid_utf8(name) && !has_ascii_control(name);
}

auto job_payload_issue(JobType type, jb::rpc::JsonValue const& payload) -> JobPayloadIssue
{
    if (!payload.is_object()) {
        return JobPayloadIssue::NotObject;
    }

    auto issue = JobPayloadIssue::UnknownType;
    switch (type) {
        case JobType::Cli:
            issue = structurally_valid_cli(payload.as_object());
            break;
        case JobType::Http:
            issue = structurally_valid_http(payload.as_object());
            break;
    }
    if (issue != JobPayloadIssue::None) {
        return issue;
    }

    auto serialized = jb::rpc::serialize_json(payload);
    if (!serialized) {
        return JobPayloadIssue::InvalidJson;
    }
    if (serialized->size() > maximum_job_document_bytes) {
        return JobPayloadIssue::TooLarge;
    }
    return JobPayloadIssue::None;
}

auto job_payload_issue_text(JobPayloadIssue issue) noexcept -> std::string_view
{
    switch (issue) {
        case JobPayloadIssue::None:
            return "none";
        case JobPayloadIssue::UnknownType:
            return "unknown_type";
        case JobPayloadIssue::NotObject:
            return "not_object";
        case JobPayloadIssue::MissingCommand:
            return "missing_command";
        case JobPayloadIssue::InvalidArguments:
            return "invalid_arguments";
        case JobPayloadIssue::MissingUrl:
            return "missing_url";
        case JobPayloadIssue::InvalidMethod:
            return "invalid_method";
        case JobPayloadIssue::InvalidJson:
            return "invalid_json";
        case JobPayloadIssue::TooLarge:
            return "too_large";
    }
    return "unknown_issue";
}

} // namespace jb::jobu::detail
