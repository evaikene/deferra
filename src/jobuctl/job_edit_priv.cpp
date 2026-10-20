#include "job_edit_priv.hpp"

#include "command_line_priv.hpp"
#include "edit_document_priv.hpp"
#include "edit_resource_priv.hpp"
#include "input_priv.hpp"
#include "management_json.hpp"

#include <array>
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

auto make_json(auto value) -> JsonValue
{
    return {.data = std::move(value)};
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

auto format_job_edit_draft(JsonValue const& value) -> Result<std::string, Error>
{
    constexpr auto instructions = "// JSONC draft: comments are ignored; trailing commas are not allowed.\n"
                                  "// Keep job_id and expected_revision unchanged.\n"
                                  "// Omitted fields leave server values unchanged.\n"
                                  "// Exit without effective changes to cancel the update.\n\n";
    return format_edit_draft(EditResource::Job, value, instructions);
}

auto prepare_job_update(JobEditBaseline const& baseline, std::string_view draft, AttributeRegistry const& registry)
    -> Result<std::optional<PreparedJobUpdate>, Error>
{
    using EditResult = Result<std::optional<PreparedJobUpdate>, Error>;

    auto parsed = parse_edit_draft(EditResource::Job, draft);
    if (!parsed) {
        return EditResult::failure(std::move(parsed).error());
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
