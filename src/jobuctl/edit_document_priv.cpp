#include "edit_document_priv.hpp"

#include "framing.hpp"

#include <fmt/format.h>

#include <utility>

namespace jb::jobuctl::detail {
namespace {

using namespace jb::core;

auto edit_error(std::string code, std::string message) -> Error
{
    return {.category = ErrorCategory::InvalidArgument, .code = std::move(code), .message = std::move(message)};
}

auto within_depth_limit(JsonValue const& value, std::size_t depth = 0) -> bool
{
    if (!value.is_array() && !value.is_object()) {
        return true;
    }
    if (depth >= JsonLimits{}.max_depth) {
        return false;
    }

    if (value.is_array()) {
        for (auto const& item : value.as_array()) {
            if (!within_depth_limit(item, depth + 1)) {
                return false;
            }
        }
    }
    else {
        for (auto const& [key, item] : value.as_object()) {
            if (!within_depth_limit(item, depth + 1)) {
                return false;
            }
        }
    }
    return true;
}

auto serialize_edit_json(EditResource resource, JsonValue const& value) -> Result<std::string, Error>
{
    // Reject excessive nesting before entering the serializer's recursive tree conversion.
    if (!within_depth_limit(value)) {
        return Result<std::string, Error>::failure(
            edit_error("jobuctl.edit.invalid_json",
                       fmt::format("{} edit JSON exceeds the nesting limit", describe_edit_resource(resource).title)));
    }
    auto compact = serialize_json(value);
    if (!compact) {
        return Result<std::string, Error>::failure(
            edit_error("jobuctl.edit.invalid_json",
                       fmt::format("{} edit JSON cannot be serialized", describe_edit_resource(resource).title)));
    }
    return compact;
}

auto append_fragment(std::string& output, std::string_view fragment) -> bool
{
    if (fragment.size() > max_edit_draft_bytes - output.size()) {
        return false;
    }
    output.append(fragment);
    return true;
}

auto append_line(std::string& output, std::size_t depth) -> bool
{
    auto const spaces = depth * 2;
    if (1 + spaces > max_edit_draft_bytes - output.size()) {
        return false;
    }
    output.push_back('\n');
    output.append(spaces, ' ');
    return true;
}

auto format_compact_json(EditResource resource, std::string_view compact) -> Result<std::string, Error>
{
    auto output  = std::string{};
    auto depth   = std::size_t{0};
    auto quoted  = false;
    auto escaped = false;

    // Serialization has already validated escaping. Track quotes and backslashes so payload text is never
    // interpreted as punctuation; every insertion is bounded before it grows the human-readable draft.
    for (std::size_t index = 0; index < compact.size(); ++index) {
        auto const byte = compact[index];
        if (quoted) {
            if (!append_fragment(output, compact.substr(index, 1))) {
                return Result<std::string, Error>::failure(edit_draft_too_large(resource));
            }
            if (escaped) {
                escaped = false;
            }
            else if (byte == '\\') {
                escaped = true;
            }
            else if (byte == '"') {
                quoted = false;
            }
            continue;
        }

        auto appended = true;
        switch (byte) {
            case '"':
                quoted   = true;
                appended = append_fragment(output, compact.substr(index, 1));
                break;
            case '{':
            case '[':
                appended = append_fragment(output, compact.substr(index, 1));
                ++depth;
                if (compact[index + 1] != '}' && compact[index + 1] != ']') {
                    appended = appended && append_line(output, depth);
                }
                break;
            case '}':
            case ']':
                --depth;
                if (compact[index - 1] != '{' && compact[index - 1] != '[') {
                    appended = append_line(output, depth);
                }
                appended = appended && append_fragment(output, compact.substr(index, 1));
                break;
            case ',':
                appended = append_fragment(output, ",") && append_line(output, depth);
                break;
            case ':':
                appended = append_fragment(output, ": ");
                break;
            default:
                appended = append_fragment(output, compact.substr(index, 1));
                break;
        }
        if (!appended) {
            return Result<std::string, Error>::failure(edit_draft_too_large(resource));
        }
    }

    if (!append_fragment(output, "\n")) {
        return Result<std::string, Error>::failure(edit_draft_too_large(resource));
    }
    return Result<std::string, Error>::success(std::move(output));
}

} // namespace

auto edit_draft_too_large(EditResource resource) -> Error
{
    return edit_error("jobuctl.edit.too_large",
                      fmt::format("{} edit draft exceeds 16 MiB", describe_edit_resource(resource).title));
}

auto format_edit_json(EditResource resource, JsonValue const& value) -> Result<std::string, Error>
{
    auto compact = serialize_edit_json(resource, value);
    if (!compact) {
        return Result<std::string, Error>::failure(std::move(compact).error());
    }
    if (compact->size() > max_edit_draft_bytes) {
        return Result<std::string, Error>::failure(edit_draft_too_large(resource));
    }
    return format_compact_json(resource, *compact);
}

auto format_edit_draft(EditResource resource, JsonValue const& value, std::string_view instructions)
    -> Result<std::string, Error>
{
    if (instructions.size() > max_edit_draft_bytes) {
        return Result<std::string, Error>::failure(edit_draft_too_large(resource));
    }
    auto json = format_edit_json(resource, value);
    if (!json) {
        return Result<std::string, Error>::failure(std::move(json).error());
    }

    auto draft = std::string{instructions};
    if (!append_fragment(draft, *json)) {
        return Result<std::string, Error>::failure(edit_draft_too_large(resource));
    }
    return Result<std::string, Error>::success(std::move(draft));
}

auto parse_edit_draft(EditResource resource, std::string_view draft) -> Result<JsonValue, Error>
{
    if (draft.size() > max_edit_draft_bytes) {
        return Result<JsonValue, Error>::failure(edit_draft_too_large(resource));
    }

    auto parsed = parse_json_with_comments(draft);
    if (!parsed || !parsed->is_object()) {
        return Result<JsonValue, Error>::failure(edit_error(
            "jobuctl.edit.invalid_json",
            fmt::format("{} edit draft must contain one valid JSONC object", describe_edit_resource(resource).title)));
    }
    return parsed;
}

auto serialize_edit_request(EditResource resource, JsonValue const& params) -> Result<std::string, Error>
{
    auto compact = serialize_edit_json(resource, params);
    if (!compact) {
        return Result<std::string, Error>::failure(std::move(compact).error());
    }
    // Recovery is consumed by ordinary request-file mode. Draft indentation and its larger allowance cannot
    // enter this artifact; full RPC framing and domain payload limits still apply when it is submitted.
    if (compact->size() > jb::rpc::FramingLimits{}.max_body_bytes) {
        return Result<std::string, Error>::failure(
            edit_error("jobuctl.edit.request_too_large", "Minimal update request exceeds the request-file size limit"));
    }
    return compact;
}

} // namespace jb::jobuctl::detail
