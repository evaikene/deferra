#include "job_edit_priv.hpp"

#include "command_line_priv.hpp"
#include "framing.hpp"
#include "input_priv.hpp"
#include "management_json.hpp"

#include <array>
#include <cstdint>
#include <utility>

namespace jb::jobuctl::detail {
namespace {

using namespace jb::core;
using namespace jb::jobu;

constexpr auto writable_members = std::array{"name", "type", "schedule", "priority", "attributes", "payload"};

auto edit_error(std::string code, std::string message) -> Error
{
    return {.category = ErrorCategory::InvalidArgument, .code = std::move(code), .message = std::move(message)};
}

auto draft_too_large() -> Error
{
    return edit_error("jobuctl.edit.too_large", "Job edit draft exceeds 16 MiB");
}

auto make_json(auto value) -> JsonValue
{
    return {.data = std::move(value)};
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

auto serialize_edit_json(JsonValue const& value) -> Result<std::string, Error>
{
    // Reject excessive nesting before entering the serializer's recursive tree conversion.
    if (!within_depth_limit(value)) {
        return Result<std::string, Error>::failure(
            edit_error("jobuctl.edit.invalid_json", "Job edit JSON exceeds the nesting limit"));
    }
    auto compact = serialize_json(value);
    if (!compact) {
        return Result<std::string, Error>::failure(
            edit_error("jobuctl.edit.invalid_json", "Job edit JSON cannot be serialized"));
    }
    return compact;
}

auto same_json_value(JsonValue const& left, JsonValue const& right) -> bool
{
    // Positive typed integers serialize as unsigned on parsing. Compare their values without narrowing or
    // treating floating-point replacements as integers; the normal field codecs must still reject those.
    if (left.is_int() && right.is_uint()) {
        return left.as_int() >= 0 && static_cast<std::uint64_t>(left.as_int()) == right.as_uint();
    }
    if (left.is_uint() && right.is_int()) {
        return right.as_int() >= 0 && left.as_uint() == static_cast<std::uint64_t>(right.as_int());
    }

    if (left.is_array() && right.is_array()) {
        auto const& a = left.as_array();
        auto const& b = right.as_array();
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t index = 0; index < a.size(); ++index) {
            if (!same_json_value(a[index], b[index])) {
                return false;
            }
        }
        return true;
    }

    if (left.is_object() && right.is_object()) {
        auto const& a = left.as_object();
        auto const& b = right.as_object();
        if (a.size() != b.size()) {
            return false;
        }
        // Object maps already have canonical key ordering; only arrays retain source ordering.
        auto other = b.begin();
        for (auto const& [key, value] : a) {
            if (key != other->first || !same_json_value(value, other->second)) {
                return false;
            }
            ++other;
        }
        return true;
    }
    return left == right;
}

auto append_fragment(std::string& output, std::string_view fragment) -> bool
{
    if (fragment.size() > max_job_edit_draft_bytes - output.size()) {
        return false;
    }
    output.append(fragment);
    return true;
}

auto append_line(std::string& output, std::size_t depth) -> bool
{
    auto const spaces = depth * 2;
    if (1 + spaces > max_job_edit_draft_bytes - output.size()) {
        return false;
    }
    output.push_back('\n');
    output.append(spaces, ' ');
    return true;
}

auto format_compact_json(std::string_view compact) -> Result<std::string, Error>
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
                return Result<std::string, Error>::failure(draft_too_large());
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
            return Result<std::string, Error>::failure(draft_too_large());
        }
    }

    if (!append_fragment(output, "\n")) {
        return Result<std::string, Error>::failure(draft_too_large());
    }
    return Result<std::string, Error>::success(std::move(output));
}

auto changed_attributes(JsonValue::Object const& original, JsonValue::Object const& edited) -> JsonValue::Object
{
    auto changes = JsonValue::Object{};
    for (auto const& [name, value] : edited) {
        auto const previous = original.find(name);
        if (previous == original.end() || !same_json_value(previous->second, value)) {
            changes.emplace(name, value);
        }
    }
    return changes;
}

auto minimal_update(JsonValue::Object const& original, JsonValue::Object const& edited) -> JsonValue
{
    auto params = JsonValue::Object{
        {"job_id",            original.at("job_id")           },
        {"expected_revision", original.at("expected_revision")},
    };

    // Deletion means omission, including individual attributes. Payloads and schedules instead replace the
    // whole value. In particular, leaving a legacy HTTP payload untouched must never resubmit it for admission.
    for (auto const* name : writable_members) {
        auto const member = edited.find(name);
        if (member == edited.end()) {
            continue;
        }
        if (std::string_view{name} == "attributes") {
            auto changes = changed_attributes(original.at(name).as_object(), member->second.as_object());
            if (!changes.empty()) {
                params.emplace(name, make_json(std::move(changes)));
            }
        }
        else if (!same_json_value(original.at(name), member->second)) {
            params.emplace(name, member->second);
        }
    }
    return make_json(std::move(params));
}

} // namespace

JobEditBaseline::JobEditBaseline(JsonValue value)
    : _value{std::move(value)}
{}

auto JobEditBaseline::from_job(JobDefinition const& job, AttributeRegistry const& registry)
    -> Result<JobEditBaseline, Error>
{
    auto encoded = job_to_json(job, registry);
    if (!encoded) {
        return Result<JobEditBaseline, Error>::failure(
            edit_error("jobuctl.edit.invalid_baseline", "Job definition cannot be projected for editing"));
    }

    // Select writable values from the public response encoding; response-only fields never enter the draft.
    auto& source = std::get<JsonValue::Object>(encoded->data);
    auto  draft  = JsonValue::Object{
        {"job_id",            std::move(source.at("id"))      },
        {"expected_revision", std::move(source.at("revision"))},
    };
    for (auto const* name : writable_members) {
        draft.emplace(name, std::move(source.at(name)));
    }
    return Result<JobEditBaseline, Error>::success(JobEditBaseline{make_json(std::move(draft))});
}

auto format_job_edit_json(JsonValue const& value) -> Result<std::string, Error>
{
    auto compact = serialize_edit_json(value);
    if (!compact) {
        return Result<std::string, Error>::failure(std::move(compact).error());
    }
    if (compact->size() > max_job_edit_draft_bytes) {
        return Result<std::string, Error>::failure(draft_too_large());
    }
    return format_compact_json(*compact);
}

auto serialize_job_edit_request(JsonValue const& params) -> Result<std::string, Error>
{
    auto compact = serialize_edit_json(params);
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

auto prepare_job_update(JobEditBaseline const& baseline, std::string_view draft, AttributeRegistry const& registry)
    -> Result<std::optional<PreparedJobUpdate>, Error>
{
    using EditResult = Result<std::optional<PreparedJobUpdate>, Error>;

    if (draft.size() > max_job_edit_draft_bytes) {
        return EditResult::failure(draft_too_large());
    }
    auto parsed = parse_json(draft);
    if (!parsed || !parsed->is_object()) {
        return EditResult::failure(
            edit_error("jobuctl.edit.invalid_json", "Job edit draft must contain one valid JSON object"));
    }

    auto const& original = baseline.value().as_object();
    auto const& edited   = parsed->as_object();
    // Check fixed values before constructing any request. The codec alone accepts a different identity or zero
    // revision, and silently restoring them would conceal an operator's attempt to edit another snapshot.
    for (auto const* name : {"job_id", "expected_revision"}) {
        auto const member = edited.find(name);
        if (member == edited.end() || !same_json_value(original.at(name), member->second)) {
            return EditResult::failure(
                edit_error("jobuctl.edit.identity_changed", "Job identity and revision must remain unchanged"));
        }
    }

    // Validate every retained value, even for a no-op. A baseline priority makes identity-only and empty-attribute
    // drafts decodable without weakening the codec's empty-update rule. This copy is never submitted.
    auto validation = edited;
    validation.try_emplace("priority", original.at("priority"));
    auto validated = decode_command_request(CommandKind::JobUpdate, make_json(std::move(validation)), registry);
    if (!validated) {
        return EditResult::failure(std::move(validated).error());
    }

    auto params = minimal_update(original, edited);
    if (params.as_object().size() == 2) {
        return EditResult::success(std::nullopt);
    }

    // The same strict decoder serves files and editing. Return both owning forms so recovery can save exactly
    // what submission would send, rather than accidentally replaying the full draft's legacy payload.
    auto decoded = decode_command_request(CommandKind::JobUpdate, params, registry);
    if (!decoded) {
        return EditResult::failure(std::move(decoded).error());
    }
    auto request  = std::get<UpdateJobRequest>(std::move(decoded).value());
    // Use the typed client's encoding for recovery too: timestamps and numeric attributes can have an accepted
    // input spelling that differs from their outgoing representation. This still contains only selected fields.
    auto outgoing = update_job_request_to_json(request, registry);
    if (!outgoing) {
        return EditResult::failure(edit_error("jobuctl.input.invalid_params", "Request parameters are invalid"));
    }
    return EditResult::success(PreparedJobUpdate{
        .params  = std::move(outgoing).value(),
        .request = std::move(request),
    });
}

} // namespace jb::jobuctl::detail
