#include "queue_edit_priv.hpp"

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

constexpr auto writable_members = std::array{"name",
                                             "weight",
                                             "concurrency_limit",
                                             "recovery_policy",
                                             "defaults",
                                             "history_retention_seconds",
                                             "runnable_wait_warning_ms"};

auto edit_error(std::string code, std::string message) -> Error
{
    return {.category = ErrorCategory::InvalidArgument, .code = std::move(code), .message = std::move(message)};
}

auto make_json(auto value) -> JsonValue
{
    return {.data = std::move(value)};
}

auto minimal_update(JsonValue::Object const& original, JsonValue::Object const& edited) -> JsonValue
{
    auto params = JsonValue::Object{
        {"queue_id",          original.at("queue_id")         },
        {"expected_revision", original.at("expected_revision")},
    };

    // Omitted fields leave server values unchanged. A changed defaults object replaces the complete partial
    // layer, so removing one override is meaningful here, unlike omission from a job attribute patch.
    for (auto const* name : writable_members) {
        auto const member = edited.find(name);
        if (member != edited.end() && !same_json_value(original.at(name), member->second)) {
            params.emplace(name, member->second);
        }
    }
    return make_json(std::move(params));
}

} // namespace

QueueEditBaseline::QueueEditBaseline(JsonValue value)
    : _value{std::move(value)}
{}

auto QueueEditBaseline::from_queue(Queue const& queue, AttributeRegistry const& registry)
    -> Result<QueueEditBaseline, Error>
{
    auto encoded = queue_to_json(queue, registry);
    if (!encoded) {
        return Result<QueueEditBaseline, Error>::failure(
            edit_error("jobuctl.edit.invalid_baseline", "Queue cannot be projected for editing"));
    }

    // The public encoder preserves the partial queue-default layer and its attribute scope. Select only update
    // values; response state/timestamps must never enter a draft or become accidental update parameters.
    auto& source = std::get<JsonValue::Object>(encoded->data);
    auto  draft  = JsonValue::Object{
        {"queue_id",          std::move(source.at("id"))      },
        {"expected_revision", std::move(source.at("revision"))},
    };
    for (auto const* name : writable_members) {
        draft.emplace(name, std::move(source.at(name)));
    }
    return Result<QueueEditBaseline, Error>::success(QueueEditBaseline{make_json(std::move(draft))});
}

auto format_queue_edit_draft(JsonValue const& value) -> Result<std::string, Error>
{
    constexpr auto instructions = "// JSONC draft: comments are ignored; trailing commas are not allowed.\n"
                                  "// Keep queue_id and expected_revision unchanged.\n"
                                  "// Omitted top-level fields leave server values unchanged.\n"
                                  "// defaults replaces the whole queue-default layer; {} clears it.\n"
                                  "// Set history_retention_seconds to null to inherit the daemon policy.\n"
                                  "// Exit without effective changes to cancel the update.\n\n";
    return format_edit_draft(EditResource::Queue, value, instructions);
}

auto prepare_queue_update(QueueEditBaseline const& baseline, std::string_view draft, AttributeRegistry const& registry)
    -> Result<std::optional<PreparedQueueUpdate>, Error>
{
    using EditResult = Result<std::optional<PreparedQueueUpdate>, Error>;

    auto parsed = parse_edit_draft(EditResource::Queue, draft);
    if (!parsed) {
        return EditResult::failure(std::move(parsed).error());
    }

    auto const& original = baseline.value().as_object();
    auto const& edited   = parsed->as_object();
    // Freeze both identity and revision. Never repair draft edits or resolve a replacement name: a queue that
    // later reuses the original name must not receive this snapshot's update.
    for (auto const* name : {"queue_id", "expected_revision"}) {
        auto const member = edited.find(name);
        if (member == edited.end() || !same_json_value(original.at(name), member->second)) {
            return EditResult::failure(
                edit_error("jobuctl.edit.identity_changed", "Queue identity and revision must remain unchanged"));
        }
    }

    // Validate all retained fields before removing unchanged values. A validation-only baseline weight makes
    // identity-only drafts decodable without weakening the strict codec's nonempty-update rule.
    auto validation = edited;
    validation.try_emplace("weight", original.at("weight"));
    auto validated = decode_command_request(CommandKind::QueueUpdate, make_json(std::move(validation)), registry);
    if (!validated) {
        return EditResult::failure(std::move(validated).error());
    }

    auto params = minimal_update(original, edited);
    if (params.as_object().size() == 2) {
        return EditResult::success(std::nullopt);
    }

    auto decoded = decode_command_request(CommandKind::QueueUpdate, params, registry);
    if (!decoded) {
        return EditResult::failure(std::move(decoded).error());
    }
    auto request = std::get<UpdateQueueRequest>(std::move(decoded).value());

    // Recovery and submission must agree even when the attribute codec accepts another numeric representation.
    // Re-encoding preserves the selected top-level fields and the original UUID/revision.
    auto outgoing = update_queue_request_to_json(request, registry);
    if (!outgoing) {
        return EditResult::failure(edit_error("jobuctl.input.invalid_params", "Request parameters are invalid"));
    }
    return EditResult::success(PreparedQueueUpdate{
        .params  = std::move(outgoing).value(),
        .request = std::move(request),
    });
}

} // namespace jb::jobuctl::detail
