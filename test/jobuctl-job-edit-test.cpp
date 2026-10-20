#include "edit_document_priv.hpp"
#include "edit_resource_priv.hpp"
#include "job_edit_priv.hpp"

#include "attribute_registry.hpp"
#include "command_line_priv.hpp"
#include "input_priv.hpp"
#include "management_json.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobuctl::detail;

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

auto sample_job(StandardAttributeRegistry const& registry) -> JobDefinition
{
    auto id         = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    auto queue      = Uuid::parse("10112233-4455-6677-8899-aabbccddeeff");
    auto at         = parse_utc_timestamp("2026-10-16T12:00:00.123456Z");
    auto attributes = materialize_attributes(registry, {}, {}, {});
    REQUIRE(id);
    REQUIRE(queue);
    REQUIRE(at);
    REQUIRE(attributes);

    return {
        .id         = *id,
        .queue_id   = *queue,
        .revision   = 7,
        .name       = std::string{"nightly-export"},
        .state      = JobState::Suspended,
        .type       = JobType::Cli,
        .schedule   = OnceSchedule{.planned_at = *at},
        .priority   = 4,
        .attributes = std::move(attributes).value(),
        .payload    = parsed(R"({"command":"/bin/echo","arguments":["first",{"secret":"reports.token"},"last"]})"),
        .created_at = *at,
        .updated_at = *at,
    };
}

auto baseline_for(JobDefinition const& job, AttributeRegistry const& registry) -> JobEditBaseline
{
    auto baseline = JobEditBaseline::from_job(job, registry);
    REQUIRE(baseline);
    return std::move(baseline).value();
}

auto prepare(JobEditBaseline const& baseline, JsonValue const& draft, AttributeRegistry const& registry)
    -> Result<std::optional<PreparedJobUpdate>, Error>
{
    return prepare_job_update(baseline, serialized(draft), registry);
}

auto prepared_update(JobEditBaseline const& baseline, JsonValue const& draft, AttributeRegistry const& registry)
    -> PreparedJobUpdate
{
    auto result = prepare(baseline, draft, registry);
    REQUIRE(result);
    REQUIRE(*result);
    return std::move(**result);
}

void check_noop(JobEditBaseline const& baseline, JsonValue const& draft, AttributeRegistry const& registry)
{
    auto result = prepare(baseline, draft, registry);
    REQUIRE(result);
    CHECK_FALSE(*result);
}

void check_safe_failure(Result<std::optional<PreparedJobUpdate>, Error> const& result)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().message.find("private-draft-marker") == std::string::npos);
    CHECK(result.error().detail.empty());
}

auto nested_arrays(std::size_t count) -> JsonValue
{
    auto value = make_json(std::uint64_t{0});
    for (std::size_t index = 0; index < count; ++index) {
        auto items = JsonValue::Array{};
        items.push_back(std::move(value));
        value = make_json(std::move(items));
    }
    return value;
}

} // namespace

TEST_CASE("Shared documents accept domain instructions without requiring a job projection", "[jobuctl][edit]")
{
    auto value        = parsed(R"({"resource_data":[1,18446744073709551615,"// literal /* text */"]})");
    auto instructions = std::string{"// Caller-owned instructions\r\n/* another instruction */\r\n"};
    auto draft        = format_edit_draft(EditResource::Queue, value, instructions);
    REQUIRE(draft);
    CHECK(draft->starts_with(instructions));
    CHECK(draft->find("job_id") == std::string::npos);
    auto original = *draft;
    auto decoded  = parse_edit_draft(EditResource::Queue, *draft);
    REQUIRE(decoded);
    CHECK(*decoded == value);
    CHECK(*draft == original);
    CHECK_FALSE(parse_json(*draft));

    auto request = serialize_edit_request(EditResource::Queue, value);
    REQUIRE(request);
    CHECK(*request == serialized(value));
    CHECK(parse_json(*request));

    auto oversized = format_edit_draft(EditResource::Queue, value, std::string(max_edit_draft_bytes + 1, ' '));
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == "jobuctl.edit.too_large");
    CHECK(oversized.error().message == "Queue edit draft exceeds 16 MiB");
}

TEST_CASE("Shared draft parsing retains strict JSON validation and job error text", "[jobuctl][edit]")
{
    for (auto const* text : {"// comments\n{\"value\":1,\"value\":2}",
                             "/* note */ {\"value\":18446744073709551616}",
                             "// note\n{\"value\":\"\xff\"}",
                             "/* unclosed",
                             "{\"value\":1,}"}) {
        auto rejected = parse_edit_draft(EditResource::Job, text);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == "jobuctl.edit.invalid_json");
        CHECK(rejected.error().message == "Job edit draft must contain one valid JSONC object");
    }
    auto invalid = format_edit_json(EditResource::Job, make_json(std::string{"\xff"}));
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().message == "Job edit JSON cannot be serialized");
    CHECK(edit_draft_too_large(EditResource::Job).message == "Job edit draft exceeds 16 MiB");
}

TEST_CASE("Job edit projection selects writable content for each runner and schedule", "[jobuctl][edit]")
{
    StandardAttributeRegistry registry;
    for (auto const type : {JobType::Cli, JobType::Http}) {
        for (auto const recurring : {false, true}) {
            for (auto const named : {false, true}) {
                CAPTURE(type, recurring, named);
                auto job = sample_job(registry);
                job.type = type;
                if (type == JobType::Http) {
                    job.payload = parsed(
                        R"({"url":"https://example.test","method":"GET","headers":[{"name":"Authorization","value":{"secret":"reports.token"}}],"body":{"secret":"reports.body"}})");
                }
                if (recurring) {
                    job.schedule = CronSchedule{.expression = "0 * * * *", .timezone = "Europe/Tallinn"};
                }
                if (!named) {
                    job.name.reset();
                }

                auto        baseline = baseline_for(job, registry);
                auto const& draft    = baseline.value().as_object();
                REQUIRE(draft.size() == 8);
                CHECK(draft.at("job_id").as_string() == job.id.to_string());
                CHECK(draft.at("expected_revision").as_uint() == job.revision);
                CHECK(draft.at("type").as_string() == (type == JobType::Cli ? "cli" : "http"));
                CHECK(draft.at("priority").as_int() == job.priority);
                CHECK(draft.at("payload") == job.payload);
                CHECK(draft.at("name").is_null() == !named);
                if (named) {
                    CHECK(draft.at("name").as_string() == *job.name);
                }
                auto attributes = attribute_set_to_json(job.attributes, registry, AttributeScope::Job);
                REQUIRE(attributes);
                CHECK(draft.at("attributes") == *attributes);
                auto const& schedule = draft.at("schedule").as_object();
                if (recurring) {
                    CHECK(schedule.at("kind").as_string() == "cron");
                    CHECK(schedule.at("expression").as_string() == "0 * * * *");
                    CHECK(schedule.at("timezone").as_string() == "Europe/Tallinn");
                }
                else {
                    CHECK(schedule.at("kind").as_string() == "once");
                    CHECK(schedule.at("at").as_string() == "2026-10-16T12:00:00.123456Z");
                }

                auto formatted = format_edit_json(EditResource::Job, baseline.value());
                REQUIRE(formatted);
                auto result = prepare_job_update(baseline, *formatted, registry);
                REQUIRE(result);
                CHECK_FALSE(*result);
            }
        }
    }
}

TEST_CASE("Job edit baseline owns its snapshot and rejects unencodable definitions", "[jobuctl][edit]")
{
    auto baseline = [] {
        StandardAttributeRegistry registry;
        auto                      job    = sample_job(registry);
        auto                      result = baseline_for(job, registry);
        job.name                         = "changed after projection";
        object(job.payload).clear();
        return result;
    }();
    CHECK(baseline.value().as_object().at("name").as_string() == "nightly-export");
    CHECK(baseline.value().as_object().at("payload").as_object().contains("command"));

    StandardAttributeRegistry registry;
    auto                      job = sample_job(registry);
    job.revision                  = 0;
    auto invalid                  = JobEditBaseline::from_job(job, registry);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "jobuctl.edit.invalid_baseline");
}

TEST_CASE("Job edit formatting preserves escapes and JSON scalar representations", "[jobuctl][edit]")
{
    auto value = parsed(
        R"({"text":"private-draft-marker: { [ , : } ] \" \\\" \\ \n\t \u0000 \u20ac","empty_object":{},"empty_array":[],"items":[true,false,null,1.0,-2,18446744073709551615,{"inner":"value"}]})");
    auto pretty = format_edit_json(EditResource::Job, value);
    REQUIRE(pretty);
    CHECK(pretty->starts_with("{\n  \"empty_array\": [],\n"));
    CHECK(pretty->ends_with("\n}\n"));
    CHECK(parsed(*pretty) == value);
    CHECK(serialized(parsed(*pretty)) == serialized(value));

    for (auto const* text : {"null", "true", "-1", "1.0", R"("[{},:]\\\"")", "{}", "[]"}) {
        auto scalar    = parsed(text);
        auto formatted = format_edit_json(EditResource::Job, scalar);
        REQUIRE(formatted);
        CHECK(parsed(*formatted) == scalar);
    }

    auto invalid = format_edit_json(EditResource::Job, make_json(std::string{"\xff"}));
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "jobuctl.edit.invalid_json");
    auto non_finite = format_edit_json(EditResource::Job, make_json(std::numeric_limits<double>::infinity()));
    CHECK_FALSE(non_finite);
}

TEST_CASE("Interactive drafts show instructions and preserve JSON values and no-op semantics", "[jobuctl][edit]")
{
    auto value = parsed(R"({"text":"jobu: literal\njobu: escaped newline","jobu: property":"value"})");
    auto draft = format_job_edit_draft(value);
    REQUIRE(draft);
    CHECK(draft->starts_with("// JSONC draft: comments are ignored; trailing commas are not allowed.\n"));
    CHECK(draft->find("// Keep job_id and expected_revision unchanged.\n") != std::string::npos);
    CHECK(draft->find("// Omitted fields leave server values unchanged.\n") != std::string::npos);
    CHECK(draft->find("// Exit without effective changes to cancel the update.\n") != std::string::npos);
    CHECK_FALSE(parse_json(*draft));
    auto decoded = parse_edit_draft(EditResource::Job, *draft);
    REQUIRE(decoded);
    CHECK(*decoded == value);

    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_job(registry), registry);
    auto                      editable = format_job_edit_draft(baseline.value());
    REQUIRE(editable);
    auto unchanged = prepare_job_update(baseline, *editable, registry);
    REQUIRE(unchanged);
    CHECK_FALSE(*unchanged);
    auto notes_only =
        prepare_job_update(baseline, "// a new operator note\n" + *editable + "/* closing note */", registry);
    REQUIRE(notes_only);
    CHECK_FALSE(*notes_only);
}

TEST_CASE("Interactive drafts accept JSONC comments without changing saved bytes or string contents", "[jobuctl][edit]")
{
    auto expected = parsed(R"({"text":"// literal\n/* escaped newline */","jobu: property":"https://example.test"})");
    for (auto const ending : {std::string_view{"\n"}, std::string_view{"\r\n"}}) {
        auto text = std::string{};
        for (auto const* line : {"// heading",
                                 "{",
                                 "  // indented note",
                                 R"("text":"// literal\n/* escaped newline */", // inline note)",
                                 "\t/* block comment",
                                 "between members */",
                                 R"("jobu: property" /* key */ : "https://example.test")",
                                 "}"}) {
            text += line;
            text += ending;
        }
        text          += "// trailing note without a final newline";
        auto original  = text;
        auto decoded   = parse_edit_draft(EditResource::Job, text);
        REQUIRE(decoded);
        CHECK(*decoded == expected);
        CHECK(text == original);
    }

    for (auto const* text : {"jobu: legacy note\n{}",
                             "// only notes\n/* no object */",
                             "// notes\n[1,2,3]",
                             "{} /* unfinished",
                             "{} // note\n{}",
                             "{\"priority\":1, // trailing comma\n}"}) {
        CAPTURE(text);
        auto rejected = parse_edit_draft(EditResource::Job, text);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == "jobuctl.edit.invalid_json");
    }
}

TEST_CASE("Interactive instructions count toward the existing size and nesting limits", "[jobuctl][edit]")
{
    auto value = parsed(R"({"text":""})");
    auto small = format_job_edit_draft(value);
    REQUIRE(small);
    // Fill exactly the remaining complete-draft budget; even the instruction bytes count toward 16 MiB.
    object(value).at("text") = make_json(std::string(max_edit_draft_bytes - small->size(), 'x'));
    auto exact               = format_job_edit_draft(value);
    REQUIRE(exact);
    CHECK(exact->size() == max_edit_draft_bytes);
    REQUIRE(parse_edit_draft(EditResource::Job, *exact));
    std::get<std::string>(object(value).at("text").data).push_back('x');
    REQUIRE(format_edit_json(EditResource::Job, value));
    auto oversized = format_job_edit_draft(value);
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == "jobuctl.edit.too_large");

    auto notes = std::string{"//"};
    notes.append(max_edit_draft_bytes - notes.size() - 3, 'x');
    notes += "\n{}";
    REQUIRE(parse_edit_draft(EditResource::Job, notes));
    notes.push_back(' ');
    auto rejected = parse_edit_draft(EditResource::Job, notes);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobuctl.edit.too_large");

    auto too_deep = "// notes\n{\"nested\":" + serialized(nested_arrays(JsonLimits{}.max_depth)) + "}";
    auto depth    = parse_edit_draft(EditResource::Job, too_deep);
    REQUIRE_FALSE(depth);
    CHECK(depth.error().code == "jobuctl.edit.invalid_json");
}

TEST_CASE("Job edit formatting and parsing enforce byte and nesting limits", "[jobuctl][edit]")
{
    // Quotes and the final newline consume three bytes. The next byte must fail even though compact JSON fits.
    auto value = make_json(std::string(max_edit_draft_bytes - 3, 'x'));
    auto exact = format_edit_json(EditResource::Job, value);
    REQUIRE(exact);
    CHECK(exact->size() == max_edit_draft_bytes);
    std::get<std::string>(value.data).push_back('x');
    auto expanded = format_edit_json(EditResource::Job, value);
    REQUIRE_FALSE(expanded);
    CHECK(expanded.error().code == "jobuctl.edit.too_large");
    std::get<std::string>(value.data).push_back('x');
    auto oversized = format_edit_json(EditResource::Job, value);
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error().code == "jobuctl.edit.too_large");

    auto maximum_depth = format_edit_json(EditResource::Job, nested_arrays(JsonLimits{}.max_depth));
    REQUIRE(maximum_depth);
    CHECK(parse_json(*maximum_depth));
    auto excessive_depth = format_edit_json(EditResource::Job, nested_arrays(JsonLimits{}.max_depth + 1));
    REQUIRE_FALSE(excessive_depth);
    CHECK(excessive_depth.error().code == "jobuctl.edit.invalid_json");

    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_job(registry), registry);
    auto                      text     = serialized(baseline.value());
    text.resize(max_edit_draft_bytes, ' ');
    auto accepted = prepare_job_update(baseline, text, registry);
    REQUIRE(accepted);
    CHECK_FALSE(*accepted);
    text.push_back(' ');
    auto rejected = prepare_job_update(baseline, text, registry);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "jobuctl.edit.too_large");

    auto draft                                  = baseline.value();
    object(object(draft).at("payload"))["deep"] = nested_arrays(JsonLimits{}.max_depth);
    check_safe_failure(prepare(baseline, draft, registry));
}

TEST_CASE("Job edit no-op tolerates omissions and reordered object members", "[jobuctl][edit]")
{
    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_job(registry), registry);
    check_noop(baseline, baseline.value(), registry);

    auto draft = baseline.value();
    for (auto const* name : {"name", "type", "schedule", "priority", "payload"}) {
        object(draft).erase(name);
        check_noop(baseline, draft, registry);
    }
    object(draft).at("attributes") = make_json(JsonValue::Object{});
    check_noop(baseline, draft, registry);
    object(draft).erase("attributes");
    check_noop(baseline, draft, registry);

    auto        reversed = std::string{"{"};
    auto const& original = baseline.value().as_object();
    for (auto member = original.rbegin(); member != original.rend(); ++member) {
        if (member != original.rbegin()) {
            reversed += ',';
        }
        reversed += serialized(make_json(member->first)) + ":" + serialized(member->second);
    }
    reversed       += '}';
    auto reordered  = prepare_job_update(baseline, reversed, registry);
    REQUIRE(reordered);
    CHECK_FALSE(*reordered);
}

TEST_CASE("Job edit comparison checks integer values without losing range or array order", "[jobuctl][edit]")
{
    StandardAttributeRegistry registry;
    auto                      job  = sample_job(registry);
    object(job.payload)["numbers"] = make_json(JsonValue::Object{
        {"positive", make_json(std::numeric_limits<std::int64_t>::max()) },
        {"negative", make_json(std::numeric_limits<std::int64_t>::min()) },
        {"unsigned", make_json(std::numeric_limits<std::uint64_t>::max())},
        {"zero",     make_json(std::uint64_t{0})                         },
    });
    auto baseline                  = baseline_for(job, registry);
    check_noop(baseline, baseline.value(), registry);

    auto       text = serialized(baseline.value());
    auto const zero = text.find("\"zero\":0");
    REQUIRE(zero != std::string::npos);
    text.insert(zero + std::string_view{"\"zero\":"}.size(), "-");
    auto negative_zero = prepare_job_update(baseline, text, registry);
    REQUIRE(negative_zero);
    CHECK_FALSE(*negative_zero);

    auto  draft                    = baseline.value();
    auto& numbers                  = object(object(draft).at("payload")).at("numbers");
    object(numbers).at("unsigned") = make_json(std::int64_t{-1});
    auto changed                   = prepared_update(baseline, draft, registry);
    CHECK(changed.params.as_object().size() == 3);
    REQUIRE(changed.request.payload);
    CHECK(changed.request.payload->as_object().at("numbers").as_object().at("unsigned").as_int() == -1);

    draft           = baseline.value();
    auto& arguments = std::get<JsonValue::Array>(object(object(draft).at("payload")).at("arguments").data);
    std::swap(arguments.front(), arguments.back());
    auto reordered = prepared_update(baseline, draft, registry);
    REQUIRE(reordered.request.payload);
    CHECK(reordered.request.payload->as_object().at("arguments").as_array().front().as_string() == "last");

    draft                        = baseline.value();
    object(draft).at("priority") = make_json(4.0);
    check_safe_failure(prepare(baseline, draft, registry));
    draft                                                           = baseline.value();
    object(object(draft).at("attributes")).at("retry.max_attempts") = make_json(1.0);
    check_safe_failure(prepare(baseline, draft, registry));
}

TEST_CASE("Job edit identity and revision cannot be removed or changed", "[jobuctl][edit]")
{
    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_job(registry), registry);
    for (auto const* name : {"job_id", "expected_revision"}) {
        auto draft = baseline.value();
        object(draft).erase(name);
        auto missing = prepare(baseline, draft, registry);
        check_safe_failure(missing);
        CHECK(missing.error().code == "jobuctl.edit.identity_changed");
    }
    for (auto const* replacement : {"0", "8", "-1", "7.0", "null", "true", "\"7\"", "18446744073709551615"}) {
        auto draft                            = baseline.value();
        object(draft).at("expected_revision") = parsed(replacement);
        auto result                           = prepare(baseline, draft, registry);
        check_safe_failure(result);
        CHECK(result.error().code == "jobuctl.edit.identity_changed");
    }
    for (auto const* replacement :
         {"\"10112233-4455-6677-8899-aabbccddeeff\"", "\"private-draft-marker\"", "null", "7"}) {
        auto draft                 = baseline.value();
        object(draft).at("job_id") = parsed(replacement);
        check_safe_failure(prepare(baseline, draft, registry));
    }
}

TEST_CASE("Job edit validates all retained content before classifying a no-op", "[jobuctl][edit]")
{
    StandardAttributeRegistry registry;
    auto                      baseline = baseline_for(sample_job(registry), registry);
    for (auto const* property :
         {"id", "revision", "state", "queue_id", "created_at", "updated_at", "deleted_at", "private-draft-marker"}) {
        auto draft = baseline.value();
        object(draft).emplace(property, make_json(std::string{"private-draft-marker"}));
        check_safe_failure(prepare(baseline, draft, registry));
    }
    auto const invalid_members = std::array{
        std::pair{"name",       "false"                                                                     },
        std::pair{"type",       "null"                                                                      },
        std::pair{"type",       "\"private-draft-marker\""                                                  },
        std::pair{"schedule",   "null"                                                                      },
        std::pair{"schedule",   R"({"kind":"once","at":"now"})"                                             },
        std::pair{"schedule",   R"({"kind":"once","at":"2026-10-16T12:00:00Z","private-draft-marker":true})"},
        std::pair{"schedule",   R"({"kind":"cron","timezone":"UTC"})"                                       },
        std::pair{"priority",   "null"                                                                      },
        std::pair{"priority",   "2147483648"                                                                },
        std::pair{"priority",   "-2147483649"                                                               },
        std::pair{"attributes", "null"                                                                      },
        std::pair{"attributes", R"({"job.timeout":null})"                                                   },
        std::pair{"attributes", R"({"private-draft-marker":1})"                                             },
        std::pair{"attributes", R"({"retry.initial_delay":10,"retry.max_delay":9})"                         },
        std::pair{"payload",    "null"                                                                      },
        std::pair{"payload",    "[]"                                                                        },
    };
    for (auto const& [name, text] : invalid_members) {
        CAPTURE(name, text);
        auto draft             = baseline.value();
        object(draft).at(name) = parsed(text);
        check_safe_failure(prepare(baseline, draft, registry));
    }
    for (auto const* text :
         {"", "{", "{}{}", "[]", "null", "true", R"({"private-draft-marker":true,"private-draft-marker":false})"}) {
        check_safe_failure(prepare_job_update(baseline, text, registry));
    }
}

TEST_CASE("Job edit name changes distinguish clearing from omission", "[jobuctl][edit]")
{
    StandardAttributeRegistry registry;
    auto                      job      = sample_job(registry);
    auto                      baseline = baseline_for(job, registry);
    auto                      draft    = baseline.value();
    object(draft).at("name")           = make_json(JsonNull{});
    auto cleared                       = prepared_update(baseline, draft, registry);
    REQUIRE(cleared.request.name);
    CHECK_FALSE(*cleared.request.name);
    CHECK(cleared.params.as_object().size() == 3);

    job.name.reset();
    baseline = baseline_for(job, registry);
    check_noop(baseline, baseline.value(), registry);
    draft                    = baseline.value();
    object(draft).at("name") = make_json(std::string{"replacement"});
    auto named               = prepared_update(baseline, draft, registry);
    REQUIRE(named.request.name);
    REQUIRE(*named.request.name);
    CHECK(**named.request.name == "replacement");
}

TEST_CASE("Job edit attributes contain only changed and newly supplied entries", "[jobuctl][edit]")
{
    StandardAttributeRegistry registry;
    auto                      job = sample_job(registry);
    // Model an older definition lacking an attribute now known to the supplied registry.
    job.attributes.erase("job.timeout");
    auto  baseline   = baseline_for(job, registry);
    auto  draft      = baseline.value();
    auto& attributes = object(object(draft).at("attributes"));
    attributes.erase("output.capture");
    attributes.at("retry.max_attempts") = make_json(std::uint64_t{3});
    attributes.emplace("job.timeout", make_json(std::uint64_t{2500}));

    auto        update = prepared_update(baseline, draft, registry);
    auto const& patch  = update.params.as_object().at("attributes").as_object();
    REQUIRE(patch.size() == 2);
    CHECK(patch.contains("retry.max_attempts"));
    CHECK(patch.contains("job.timeout"));
    CHECK(update.request.attribute_changes.size() == 2);
    CHECK(std::get<std::int64_t>(update.request.attribute_changes.at("retry.max_attempts").data) == 3);
    CHECK(std::get<Duration>(update.request.attribute_changes.at("job.timeout").data) ==
          std::chrono::milliseconds{2500});
    CHECK_FALSE(update.request.payload);
}

TEST_CASE("Job edit payload and schedule changes are complete replacements", "[jobuctl][edit]")
{
    StandardAttributeRegistry registry;
    auto                      job = sample_job(registry);
    job.schedule                  = CronSchedule{.expression = "0 * * * *", .timezone = "Europe/Tallinn"};
    auto baseline                 = baseline_for(job, registry);
    auto draft                    = baseline.value();
    object(object(draft).at("payload")).erase("arguments");
    object(object(draft).at("schedule")).erase("timezone");
    auto update = prepared_update(baseline, draft, registry);
    REQUIRE(update.request.payload);
    CHECK(update.request.payload->as_object().size() == 1);
    CHECK(update.request.payload->as_object().contains("command"));
    REQUIRE(update.request.schedule);
    auto const& cron = std::get<CronScheduleInput>(*update.request.schedule);
    CHECK(cron.expression == "0 * * * *");
    CHECK_FALSE(cron.timezone);
    CHECK(update.params.as_object().at("schedule").as_object().size() == 2);

    draft                        = baseline.value();
    object(draft).at("type")     = make_json(std::string{"http"});
    object(draft).at("payload")  = parsed(R"({"url":"https://example.test","method":"OPTIONS"})");
    object(draft).at("schedule") = parsed(R"({"kind":"once","at":"2026-10-17T12:00:00Z"})");
    auto conversion              = prepared_update(baseline, draft, registry);
    CHECK(conversion.request.type == JobType::Http);
    REQUIRE(conversion.request.payload);
    // The pure helper validates the request schema; HTTP method admission remains the service's decision.
    CHECK(conversion.request.payload->as_object().at("method").as_string() == "OPTIONS");
    REQUIRE(conversion.request.schedule);
    CHECK(std::holds_alternative<OnceSchedule>(*conversion.request.schedule));
}

TEST_CASE("Job edit preserves legacy HTTP payloads and returns owning minimal shared-codec data", "[jobuctl][edit]")
{
    for (auto const* method : {"get", "CUSTOM"}) {
        auto update = [method] {
            StandardAttributeRegistry registry;
            auto                      job = sample_job(registry);
            job.type                      = JobType::Http;
            job.payload = parsed(R"({"url":"https://example.test","method":"get","body":{"secret":"reports.body"}})");
            object(job.payload).at("method") = make_json(std::string{method});
            auto baseline                    = baseline_for(job, registry);
            auto original                    = serialized(baseline.value());
            auto draft                       = baseline.value();
            object(draft).at("priority")     = make_json(std::int64_t{50});
            object(draft).at("name")         = make_json(std::string{"edited"});
            auto result                      = prepared_update(baseline, draft, registry);
            CHECK(serialized(baseline.value()) == original);
            CHECK(baseline.value().as_object().at("payload").as_object().at("method").as_string() == method);
            object(draft).clear();
            return result;
        }();

        REQUIRE(update.params.as_object().size() == 4);
        CHECK_FALSE(update.params.as_object().contains("payload"));
        CHECK_FALSE(update.params.as_object().contains("attributes"));
        CHECK(update.request.priority == 50);
        REQUIRE(update.request.name);
        REQUIRE(*update.request.name);
        CHECK(**update.request.name == "edited");
        CHECK(update.request.expected_revision == 7);
        CHECK(update.request.job_id.to_string() == "00112233-4455-6677-8899-aabbccddeeff");
        CHECK_FALSE(update.request.payload);

        StandardAttributeRegistry registry;
        auto                      decoded = decode_command_request(CommandKind::JobUpdate, update.params, registry);
        REQUIRE(decoded);
        auto encoded = update_job_request_to_json(std::get<UpdateJobRequest>(*decoded), registry);
        REQUIRE(encoded);
        CHECK(serialized(*encoded) == serialized(update.params));
    }
}

TEST_CASE("Job edit recovery params match the typed client's outgoing representation", "[jobuctl][edit]")
{
    StandardAttributeRegistry registry;
    auto                      job = sample_job(registry);
    job.type                      = JobType::Http;
    job.payload   = parsed(R"({"url":"https://example.test","method":"get","body":{"secret":"reports.body"}})");
    auto baseline = baseline_for(job, registry);
    auto draft    = baseline.value();
    object(object(draft).at("payload")).at("url") = make_json(std::string{"https://example.test/edited"});
    object(draft).at("schedule")                  = parsed(R"({"kind":"once","at":"2026-10-17T12:00:00Z"})");
    object(object(draft).at("attributes")).at("retry.jitter") = make_json(std::uint64_t{1});

    auto update = prepared_update(baseline, draft, registry);
    REQUIRE(update.request.payload);
    CHECK(update.request.payload->as_object().at("method").as_string() == "get");
    CHECK(update.request.payload->as_object().at("body").as_object().at("secret").as_string() == "reports.body");
    CHECK(update.params.as_object().at("attributes").as_object().at("retry.jitter").is_double());
    CHECK(update.params.as_object().at("schedule").as_object().at("at").as_string() == "2026-10-17T12:00:00.000000Z");
    auto outgoing = update_job_request_to_json(update.request, registry);
    REQUIRE(outgoing);
    CHECK(serialized(*outgoing) == serialized(update.params));
    CHECK(update.params.as_object().size() == 5);
}
