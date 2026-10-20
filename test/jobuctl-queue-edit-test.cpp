#include "queue_edit_priv.hpp"

#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "edit_document_priv.hpp"
#include "edit_resource_priv.hpp"
#include "framing.hpp"
#include "input_priv.hpp"
#include "management_json.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobuctl::detail;
using namespace std::chrono_literals;

namespace {

auto make_json(auto value) -> JsonValue
{
    return {.data = std::move(value)};
}

auto object(JsonValue& value) -> JsonValue::Object&
{
    return std::get<JsonValue::Object>(value.data);
}

auto parsed(std::string_view text) -> JsonValue
{
    auto value = parse_json(text);
    REQUIRE(value);
    return std::move(value).value();
}

auto serialized(JsonValue const& value) -> std::string
{
    auto text = serialize_json(value);
    REQUIRE(text);
    return std::move(text).value();
}

auto sample_queue() -> Queue
{
    auto id = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    auto at = parse_utc_timestamp("2026-10-20T12:00:00.123456Z");
    REQUIRE(id);
    REQUIRE(at);

    auto errors = AttributeValue::List{
        {.data = std::string{"connect"}},
        {.data = std::string{"timeout"}},
    };
    auto defaults = AttributeSet{
        {"http.retry_errors",  {.data = std::move(errors)}},
        {"job.timeout",        {.data = Duration{2500ms}} },
        {"retry.max_attempts", {.data = std::int64_t{3}}  },
    };

    return {
        .id                    = *id,
        .revision              = 7,
        .name                  = "reports",
        .state                 = QueueState::Suspended,
        .weight                = 3,
        .concurrency_limit     = 4,
        .recovery_policy       = RecoveryPolicy::RetryInterrupted,
        .defaults              = std::move(defaults),
        .history_retention     = 2h,
        .runnable_wait_warning = 1500ms,
        .created_at            = *at,
        .updated_at            = *at,
        .deleted_at            = *at,
    };
}

auto baseline_for(Queue const& queue, AttributeRegistry const& registry) -> QueueEditBaseline
{
    auto baseline = QueueEditBaseline::from_queue(queue, registry);
    REQUIRE(baseline);
    return std::move(baseline).value();
}

auto prepare(QueueEditBaseline const& baseline, JsonValue const& draft, AttributeRegistry const& registry)
    -> Result<std::optional<PreparedQueueUpdate>, Error>
{
    return prepare_queue_update(baseline, serialized(draft), registry);
}

auto prepared_update(QueueEditBaseline const& baseline, JsonValue const& draft, AttributeRegistry const& registry)
    -> PreparedQueueUpdate
{
    auto result = prepare(baseline, draft, registry);
    REQUIRE(result);
    REQUIRE(*result);
    return std::move(**result);
}

void check_noop(QueueEditBaseline const& baseline, JsonValue const& draft, AttributeRegistry const& registry)
{
    auto result = prepare(baseline, draft, registry);
    REQUIRE(result);
    CHECK_FALSE(*result);
}

void check_safe_failure(Result<std::optional<PreparedQueueUpdate>, Error> const& result)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("private-draft-marker") == std::string::npos);
    CHECK(result.error().detail.empty());
}

/// Supplies a known attribute that is deliberately unavailable to queue defaults.
class JobOnlyRegistry final : public AttributeRegistry {
public:
    [[nodiscard]] auto find(std::string_view name) const noexcept -> AttributeDefinition const* override
    {
        return name == _definition.name ? &_definition : nullptr;
    }

    [[nodiscard]] auto validate(std::string_view name, AttributeValue const& value, AttributeScope scope) const
        -> Result<void, Error> override
    {
        if (name != _definition.name || !_definition.scopes.test(scope) || !std::holds_alternative<bool>(value.data)) {
            return Result<void, Error>::failure({
                .category = ErrorCategory::InvalidArgument,
                .code     = "test.attribute.invalid",
                .message  = "private-draft-marker",
            });
        }
        return Result<void, Error>::success();
    }

    [[nodiscard]] auto definitions() const -> std::span<AttributeDefinition const> override
    {
        return {&_definition, 1};
    }

private:
    AttributeDefinition _definition{
        .name             = "job.only",
        .type             = AttributeType::Boolean,
        .scopes           = {AttributeScope::Job},
        .built_in_default = {.data = false},
        .description      = "Job-only test attribute",
    };
};

} // namespace

TEST_CASE("Queue edit projection keeps actual configuration and only partial defaults", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      queue    = sample_queue();
    auto                      baseline = baseline_for(queue, registry);
    auto const&               draft    = baseline.value().as_object();
    REQUIRE(draft.size() == 9);
    CHECK(draft.at("queue_id").as_string() == queue.id.to_string());
    CHECK(draft.at("expected_revision").as_uint() == 7);
    CHECK(draft.at("name").as_string() == "reports");
    CHECK(draft.at("weight").as_uint() == 3);
    CHECK(draft.at("concurrency_limit").as_uint() == 4);
    CHECK(draft.at("recovery_policy").as_string() == "retry_interrupted");
    CHECK(draft.at("history_retention_seconds").as_int() == 7200);
    CHECK(draft.at("runnable_wait_warning_ms").as_int() == 1500);
    CHECK(draft.at("defaults").as_object().size() == 3);
    CHECK(draft.at("defaults").as_object().at("job.timeout").as_int() == 2500);

    // Materialized daemon/job values differ deliberately; they must not enter the captured queue layer.
    auto daemon_defaults = AttributeSet{
        {"output.capture", {.data = std::string{"always"}}},
    };
    auto job_attributes = AttributeSet{
        {"retry.max_attempts", {.data = std::int64_t{5}}},
    };
    auto materialized = materialize_attributes(registry, daemon_defaults, queue.defaults, job_attributes);
    REQUIRE(materialized);
    CHECK(materialized->size() > draft.at("defaults").as_object().size());
    CHECK_FALSE(draft.at("defaults").as_object().contains("output.capture"));
    CHECK(draft.at("defaults").as_object().at("retry.max_attempts").as_int() == 3);
    for (auto const* name : {"id",
                             "revision",
                             "queue_name",
                             "state",
                             "created_at",
                             "updated_at",
                             "deleted_at",
                             "job_count",
                             "statistics",
                             "idempotency_key"}) {
        CHECK_FALSE(draft.contains(name));
    }

    queue.defaults.clear();
    queue.history_retention.reset();
    auto inherited = baseline_for(queue, registry);
    CHECK(inherited.value().as_object().at("defaults").as_object().empty());
    CHECK(inherited.value().as_object().at("history_retention_seconds").is_null());
}

TEST_CASE("Queue edit snapshots own values and fail safely when public encoding fails", "[jobuctl][edit][queue]")
{
    auto baseline = [] {
        StandardAttributeRegistry registry;
        auto                      queue  = sample_queue();
        auto                      result = baseline_for(queue, registry);
        queue.name                       = "changed after capture";
        queue.defaults.clear();
        return result;
    }();
    CHECK(baseline.value().as_object().at("name").as_string() == "reports");
    CHECK(baseline.value().as_object().at("defaults").as_object().size() == 3);

    StandardAttributeRegistry registry;
    auto                      queue = sample_queue();
    queue.revision                  = 0;
    auto invalid_revision           = QueueEditBaseline::from_queue(queue, registry);
    REQUIRE_FALSE(invalid_revision);
    CHECK(invalid_revision.error().code == "jobuctl.edit.invalid_baseline");
    queue = sample_queue();
    queue.defaults.emplace("private-draft-marker", AttributeValue{.data = true});
    auto invalid_defaults = QueueEditBaseline::from_queue(queue, registry);
    REQUIRE_FALSE(invalid_defaults);
    CHECK(invalid_defaults.error().message == "Queue cannot be projected for editing");
    CHECK(invalid_defaults.error().detail.empty());
}

TEST_CASE("Queue drafts show the specified instructions and comment-only edits are no-ops", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      queue = sample_queue();
    queue.name                      = "reports // literal /* markers */ \"quoted\"\nnext";
    auto baseline                   = baseline_for(queue, registry);
    auto draft                      = format_queue_edit_draft(baseline.value());
    REQUIRE(draft);
    CHECK(draft->starts_with("// JSONC draft: comments are ignored; trailing commas are not allowed.\n"
                             "// Keep queue_id and expected_revision unchanged.\n"
                             "// Omitted top-level fields leave server values unchanged.\n"
                             "// defaults replaces the whole queue-default layer; {} clears it.\n"
                             "// Set history_retention_seconds to null to inherit the daemon policy.\n"
                             "// Exit without effective changes to cancel the update.\n\n"));
    CHECK_FALSE(parse_json(*draft));
    auto noop = prepare_queue_update(baseline, *draft, registry);
    REQUIRE(noop);
    CHECK_FALSE(*noop);

    // Replace all instructions and insert CRLF/inline/block comments without touching quoted string data.
    auto commented = serialized(baseline.value());
    commented.insert(1, "\r\n /* changed instructions */ // inline comment\r\n");
    commented         += "\r\n// another note\r\n";
    auto saved         = commented;
    auto parsed_draft  = parse_edit_draft(EditResource::Queue, commented);
    REQUIRE(parsed_draft);
    CHECK(parsed_draft->as_object().at("name").as_string() == queue.name);
    auto edited = prepare_queue_update(baseline, commented, registry);
    REQUIRE(edited);
    CHECK_FALSE(*edited);
    CHECK(commented == saved);
}

TEST_CASE("Queue no-ops tolerate omitted fields and reordered objects", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_queue(), registry);
    check_noop(baseline, baseline.value(), registry);

    auto draft = baseline.value();
    for (auto const* name : {"name",
                             "weight",
                             "concurrency_limit",
                             "recovery_policy",
                             "defaults",
                             "history_retention_seconds",
                             "runnable_wait_warning_ms"}) {
        object(draft).erase(name);
        check_noop(baseline, draft, registry);
    }
    CHECK(draft.as_object().size() == 2);
    CHECK_FALSE(decode_command_request(CommandKind::QueueUpdate, draft, registry));

    auto        reversed = std::string{"{"};
    auto const& original = baseline.value().as_object();
    for (auto member = original.rbegin(); member != original.rend(); ++member) {
        if (member != original.rbegin()) {
            reversed += ',';
        }
        reversed += serialized(make_json(member->first)) + ':' + serialized(member->second);
    }
    reversed       += '}';
    auto reordered  = prepare_queue_update(baseline, reversed, registry);
    REQUIRE(reordered);
    CHECK_FALSE(*reordered);
}

TEST_CASE("Queue scalar changes submit only their changed top-level value", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_queue(), registry);
    for (auto const& [name, replacement] : std::array{
             std::pair{"name",                     "\"nightly\""         },
             std::pair{"weight",                   "5"                   },
             std::pair{"concurrency_limit",        "6"                   },
             std::pair{"recovery_policy",          "\"fail_interrupted\""},
             std::pair{"runnable_wait_warning_ms", "0"                   }
    }) {
        CAPTURE(name);
        auto draft             = baseline.value();
        object(draft).at(name) = parsed(replacement);
        auto update            = prepared_update(baseline, draft, registry);
        CHECK(update.params.as_object().size() == 3);
        CHECK(update.params.as_object().contains(name));
        CHECK(update.params.as_object().at("queue_id") == baseline.value().as_object().at("queue_id"));
        CHECK(update.params.as_object().at("expected_revision") ==
              baseline.value().as_object().at("expected_revision"));
        auto outgoing = update_queue_request_to_json(update.request, registry);
        REQUIRE(outgoing);
        CHECK(serialized(*outgoing) == serialized(update.params));
    }
}

TEST_CASE("Queue defaults replace the full layer including removals and clearing", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_queue(), registry);
    auto                      draft    = baseline.value();
    object(object(draft).at("defaults")).erase("job.timeout");
    auto removed = prepared_update(baseline, draft, registry);
    REQUIRE(removed.request.defaults);
    CHECK(removed.request.defaults->size() == 2);
    CHECK_FALSE(removed.request.defaults->contains("job.timeout"));
    CHECK(removed.request.defaults->contains("retry.max_attempts"));
    CHECK(removed.params.as_object().size() == 3);
    CHECK(removed.params.as_object().at("defaults").as_object().size() == 2);

    object(draft).at("defaults") = make_json(JsonValue::Object{});
    auto cleared                 = prepared_update(baseline, draft, registry);
    REQUIRE(cleared.request.defaults);
    CHECK(cleared.request.defaults->empty());
    CHECK(cleared.params.as_object().at("defaults").as_object().empty());
    object(draft).erase("defaults");
    check_noop(baseline, draft, registry);

    draft                                                         = baseline.value();
    object(object(draft).at("defaults")).at("retry.max_attempts") = make_json(std::uint64_t{5});
    auto replaced                                                 = prepared_update(baseline, draft, registry);
    REQUIRE(replaced.request.defaults);
    CHECK(replaced.request.defaults->size() == 3);
    CHECK(std::get<std::int64_t>(replaced.request.defaults->at("retry.max_attempts").data) == 5);
    CHECK(replaced.request.defaults->contains("job.timeout"));

    auto queue = sample_queue();
    queue.defaults.clear();
    auto empty = baseline_for(queue, registry);
    check_noop(empty, empty.value(), registry);
}

TEST_CASE("Queue retention preserves inheritance unlimited concrete and omitted meanings", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    for (auto const original : {std::optional<std::chrono::seconds>{}, std::optional{0s}, std::optional{3600s}}) {
        auto queue              = sample_queue();
        queue.history_retention = original;
        auto baseline           = baseline_for(queue, registry);
        for (auto const target : {std::optional<std::chrono::seconds>{}, std::optional{0s}, std::optional{3600s}}) {
            auto draft = baseline.value();
            object(draft).at("history_retention_seconds") =
                target ? make_json(std::int64_t{target->count()}) : make_json(JsonNull{});
            if (original == target) {
                check_noop(baseline, draft, registry);
                continue;
            }
            auto update = prepared_update(baseline, draft, registry);
            CHECK(update.params.as_object().size() == 3);
            REQUIRE(update.request.history_retention);
            CHECK(*update.request.history_retention == target);
            CHECK(update.params.as_object().at("history_retention_seconds").is_null() == !target.has_value());
        }
        auto omitted = baseline.value();
        object(omitted).erase("history_retention_seconds");
        object(omitted).at("weight") = make_json(std::uint64_t{5});
        auto update                  = prepared_update(baseline, omitted, registry);
        CHECK_FALSE(update.request.history_retention);
        CHECK_FALSE(update.params.as_object().contains("history_retention_seconds"));
    }
}

TEST_CASE("Queue identity and revision cannot be removed changed or replaced with a name", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_queue(), registry);
    for (auto const* name : {"queue_id", "expected_revision"}) {
        auto draft = baseline.value();
        object(draft).erase(name);
        auto missing = prepare(baseline, draft, registry);
        check_safe_failure(missing);
        CHECK(missing.error().code == "jobuctl.edit.identity_changed");
    }
    for (auto const* replacement : {"0", "8", "-1", "7.0", "null", "true", "\"7\"", "18446744073709551615"}) {
        auto draft                            = baseline.value();
        object(draft).at("expected_revision") = parsed(replacement);
        auto changed                          = prepare(baseline, draft, registry);
        check_safe_failure(changed);
        CHECK(changed.error().code == "jobuctl.edit.identity_changed");
    }
    for (auto const* replacement :
         {"\"10112233-4455-6677-8899-aabbccddeeff\"", "\"private-draft-marker\"", "null", "true", "7"}) {
        auto draft                   = baseline.value();
        object(draft).at("queue_id") = parsed(replacement);
        check_safe_failure(prepare(baseline, draft, registry));
    }
    auto draft = baseline.value();
    object(draft).emplace("queue_name", make_json(std::string{"reports"}));
    check_safe_failure(prepare(baseline, draft, registry));
    object(draft).erase("queue_id");
    check_safe_failure(prepare(baseline, draft, registry));
}

TEST_CASE("Queue drafts validate retained fields and reject unknown or read-only members", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_queue(), registry);
    for (auto const* name : {"id",
                             "revision",
                             "state",
                             "created_at",
                             "updated_at",
                             "deleted_at",
                             "statistics",
                             "idempotency_key",
                             "private-draft-marker"}) {
        auto draft = baseline.value();
        object(draft).emplace(name, make_json(std::string{"private-draft-marker"}));
        check_safe_failure(prepare(baseline, draft, registry));
    }
    for (auto const& [name, replacement] : std::array{
             std::pair{"name",                      "null"                                             },
             std::pair{"name",                      "false"                                            },
             std::pair{"weight",                    "null"                                             },
             std::pair{"weight",                    "-1"                                               },
             std::pair{"weight",                    "3.0"                                              },
             std::pair{"weight",                    "4294967296"                                       },
             std::pair{"weight",                    "\"3\""                                            },
             std::pair{"concurrency_limit",         "null"                                             },
             std::pair{"concurrency_limit",         "true"                                             },
             std::pair{"concurrency_limit",         "4.0"                                              },
             std::pair{"concurrency_limit",         "-1"                                               },
             std::pair{"concurrency_limit",         "4294967296"                                       },
             std::pair{"recovery_policy",           "null"                                             },
             std::pair{"recovery_policy",           "\"private-draft-marker\""                         },
             std::pair{"recovery_policy",           "1"                                                },
             std::pair{"defaults",                  "null"                                             },
             std::pair{"defaults",                  "[]"                                               },
             std::pair{"defaults",                  R"({"job.timeout":null})"                          },
             std::pair{"defaults",                  R"({"job.timeout":2500.0})"                        },
             std::pair{"defaults",                  R"({"retry.max_attempts":3.0})"                    },
             std::pair{"defaults",                  R"({"private-draft-marker":1})"                    },
             std::pair{"defaults",                  R"({"retry.initial_delay":10,"retry.max_delay":9})"},
             std::pair{"history_retention_seconds", "true"                                             },
             std::pair{"history_retention_seconds", "7200.0"                                           },
             std::pair{"history_retention_seconds", "\"7200\""                                         },
             std::pair{"history_retention_seconds", "9223372036854775808"                              },
             std::pair{"runnable_wait_warning_ms",  "null"                                             },
             std::pair{"runnable_wait_warning_ms",  "false"                                            },
             std::pair{"runnable_wait_warning_ms",  "1500.0"                                           },
             std::pair{"runnable_wait_warning_ms",  "9223372036854775808"                              }
    }) {
        CAPTURE(name, replacement);
        auto draft             = baseline.value();
        object(draft).at(name) = parsed(replacement);
        check_safe_failure(prepare(baseline, draft, registry));
    }

    JobOnlyRegistry restricted;
    auto            queue = sample_queue();
    queue.defaults.clear();
    auto scoped                  = baseline_for(queue, restricted);
    auto draft                   = scoped.value();
    object(draft).at("defaults") = parsed(R"({"job.only":true})");
    check_safe_failure(prepare(scoped, draft, restricted));
    queue.defaults.emplace("job.only", AttributeValue{.data = true});
    CHECK_FALSE(QueueEditBaseline::from_queue(queue, restricted));
}

TEST_CASE("Queue comparison preserves integer range and array order", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      queue = sample_queue();
    queue.revision                  = static_cast<QueueRevision>(std::numeric_limits<std::int64_t>::max());
    queue.weight                    = std::numeric_limits<std::uint32_t>::max();
    queue.concurrency_limit         = std::numeric_limits<std::uint32_t>::max();
    queue.history_retention         = std::chrono::seconds{std::numeric_limits<std::int64_t>::max()};
    queue.runnable_wait_warning     = std::chrono::milliseconds::zero();
    auto baseline                   = baseline_for(queue, registry);
    check_noop(baseline, baseline.value(), registry);

    auto signed_zero = serialized(baseline.value());
    auto position    = signed_zero.find("\"runnable_wait_warning_ms\":0");
    REQUIRE(position != std::string::npos);
    signed_zero.insert(position + std::string_view{"\"runnable_wait_warning_ms\":"}.size(), "-");
    auto noop = prepare_queue_update(baseline, signed_zero, registry);
    REQUIRE(noop);
    CHECK_FALSE(*noop);

    auto  draft  = baseline.value();
    auto& errors = std::get<JsonValue::Array>(object(object(draft).at("defaults")).at("http.retry_errors").data);
    std::swap(errors.front(), errors.back());
    auto reordered = prepared_update(baseline, draft, registry);
    CHECK(reordered.request.expected_revision == queue.revision);
    CHECK(reordered.params.as_object().size() == 3);
    CHECK(reordered.params.as_object().at("defaults").as_object().size() == 3);
    CHECK(reordered.params.as_object()
              .at("defaults")
              .as_object()
              .at("http.retry_errors")
              .as_array()
              .front()
              .as_string() == "timeout");
}

TEST_CASE("Queue preparation leaves service-level domain admission to the daemon", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      baseline            = baseline_for(sample_queue(), registry);
    auto                      draft               = baseline.value();
    // These are structurally typed values; the shared request codec delegates domain bounds to the service.
    object(draft).at("name")                      = make_json(std::string{});
    object(draft).at("weight")                    = make_json(std::uint64_t{0});
    object(draft).at("concurrency_limit")         = make_json(std::uint64_t{0});
    object(draft).at("history_retention_seconds") = make_json(std::int64_t{-1});
    object(draft).at("runnable_wait_warning_ms")  = make_json(std::int64_t{-1});
    auto update                                   = prepared_update(baseline, draft, registry);
    CHECK(update.request.name == "");
    CHECK(update.request.weight == 0);
    CHECK(update.request.concurrency_limit == 0);
    REQUIRE(update.request.history_retention);
    CHECK(*update.request.history_retention == -1s);
    CHECK(update.request.runnable_wait_warning == -1ms);
}

TEST_CASE("Queue JSONC parsing retains strict validation and safe errors", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_queue(), registry);
    for (auto const* text : {"",
                             "{",
                             "{}{}",
                             "[]",
                             "null",
                             "true",
                             "/* unfinished",
                             "// note\n{\"private-draft-marker\":1,\"private-draft-marker\":2}",
                             "/* note */ {\"private-draft-marker\":18446744073709551616}",
                             "// note\n{\"private-draft-marker\":\"\xff\"}",
                             "{\"weight\":3,}"}) {
        auto rejected = prepare_queue_update(baseline, text, registry);
        check_safe_failure(rejected);
        CHECK(rejected.error().code == "jobuctl.edit.invalid_json");
        CHECK(rejected.error().message == "Queue edit draft must contain one valid JSONC object");
    }

    auto deep = std::string{"// note\n{\"defaults\":"};
    deep.append(JsonLimits{}.max_depth, '[');
    deep += '0';
    deep.append(JsonLimits{}.max_depth, ']');
    deep                 += '}';
    auto excessive_depth  = prepare_queue_update(baseline, deep, registry);
    check_safe_failure(excessive_depth);
    CHECK(excessive_depth.error().code == "jobuctl.edit.invalid_json");
}

TEST_CASE("Queue formatting and preparation bound complete drafts including comments", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_queue(), registry);
    auto                      value    = baseline.value();
    object(value).at("name")           = make_json(std::string{});
    auto small                         = format_queue_edit_draft(value);
    REQUIRE(small);
    object(value).at("name") = make_json(std::string(max_edit_draft_bytes - small->size(), 'x'));
    auto exact               = format_queue_edit_draft(value);
    REQUIRE(exact);
    CHECK(exact->size() == max_edit_draft_bytes);
    std::get<std::string>(object(value).at("name").data).push_back('x');
    auto oversized = format_queue_edit_draft(value);
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == "jobuctl.edit.too_large");

    auto original  = serialized(baseline.value());
    auto commented = std::string{"//"};
    commented.append(max_edit_draft_bytes - commented.size() - 1 - original.size(), 'x');
    commented     += '\n';
    commented     += original;
    auto accepted  = prepare_queue_update(baseline, commented, registry);
    REQUIRE(accepted);
    CHECK_FALSE(*accepted);
    CHECK(commented.size() == max_edit_draft_bytes);
    commented.push_back(' ');
    auto rejected = prepare_queue_update(baseline, commented, registry);
    check_safe_failure(rejected);
    CHECK(rejected.error().code == "jobuctl.edit.too_large");
}

TEST_CASE("Prepared queue updates own minimal params matching strict outgoing requests", "[jobuctl][edit][queue]")
{
    auto update = [] {
        StandardAttributeRegistry registry;
        auto                      baseline = baseline_for(sample_queue(), registry);
        auto                      original = serialized(baseline.value());
        auto                      draft    = baseline.value();
        object(draft).at("name")           = make_json(std::string{"nightly"});
        object(object(draft).at("defaults")).emplace("retry.jitter", make_json(std::uint64_t{1}));
        auto result = prepared_update(baseline, draft, registry);
        CHECK(serialized(baseline.value()) == original);
        object(draft).clear();
        return result;
    }();
    CHECK(update.params.as_object().size() == 4);
    CHECK(update.request.name == "nightly");
    CHECK(update.request.expected_revision == 7);
    REQUIRE(std::holds_alternative<Uuid>(update.request.queue));
    CHECK(std::get<Uuid>(update.request.queue).to_string() == "00112233-4455-6677-8899-aabbccddeeff");
    REQUIRE(update.request.defaults);
    CHECK(update.request.defaults->size() == 4);
    CHECK(update.params.as_object().at("defaults").as_object().at("retry.jitter").is_double());
    CHECK_FALSE(update.params.as_object().contains("weight"));

    StandardAttributeRegistry registry;
    auto                      compact = serialize_edit_request(EditResource::Queue, update.params);
    REQUIRE(compact);
    CHECK(compact->find('\n') == std::string::npos);
    auto strict = parse_json(*compact);
    REQUIRE(strict);
    auto decoded = decode_command_request(CommandKind::QueueUpdate, *strict, registry);
    REQUIRE(decoded);
    auto outgoing = update_queue_request_to_json(std::get<UpdateQueueRequest>(*decoded), registry);
    REQUIRE(outgoing);
    CHECK(serialized(*outgoing) == *compact);
    CHECK(serialized(update.params) == *compact);
}

TEST_CASE("Queue compact requests retain the smaller framing limit", "[jobuctl][edit][queue]")
{
    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_queue(), registry);
    auto                      draft    = baseline.value();
    object(draft).at("name")           = make_json(std::string{});
    auto       small                   = prepared_update(baseline, draft, registry);
    auto const limit                   = jb::rpc::FramingLimits{}.max_body_bytes;
    object(draft).at("name")           = make_json(std::string(limit - serialized(small.params).size(), 'x'));
    auto exact                         = prepared_update(baseline, draft, registry);
    auto compact                       = serialize_edit_request(EditResource::Queue, exact.params);
    REQUIRE(compact);
    CHECK(compact->size() == limit);
    CHECK(parse_json(*compact));

    std::get<std::string>(object(draft).at("name").data).push_back('x');
    auto oversized = prepared_update(baseline, draft, registry);
    auto human     = format_queue_edit_draft(draft);
    REQUIRE(human);
    CHECK(human->size() < max_edit_draft_bytes);
    auto rejected = serialize_edit_request(EditResource::Queue, oversized.params);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobuctl.edit.request_too_large");
}
