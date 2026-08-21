#include "management_json.hpp"

#include "attribute_registry.hpp"
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono> // IWYU pragma: keep for std::chrono_literals
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::rpc;
using namespace std::chrono_literals;

namespace {

auto make_json(auto value) -> JsonValue
{
    JsonValue result;
    result.data = std::move(value);
    return result;
}

auto object(JsonValue& value) -> JsonValue::Object&
{
    return std::get<JsonValue::Object>(value.data);
}

auto array(JsonValue& value) -> JsonValue::Array&
{
    return std::get<JsonValue::Array>(value.data);
}

auto parse_uuid(std::string_view text) -> Uuid
{
    auto parsed = Uuid::parse(text);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto parse_time(std::string_view text) -> UtcTimePoint
{
    auto parsed = parse_utc_timestamp(text);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto job_attributes(StandardAttributeRegistry const& registry, Duration timeout = 2500ms) -> AttributeSet
{
    auto materialized = materialize_attributes(registry,
                                               {
    },
                                               {},
                                               {{"job.timeout", {.data = timeout}}});
    REQUIRE(materialized);
    return std::move(materialized).value();
}

auto cli_payload(std::string command = "/usr/bin/example") -> JsonValue
{
    return make_json(JsonValue::Object{
        {"arguments", make_json(JsonValue::Array{make_json(std::string{"--dry-run"})})},
        {"command",   make_json(std::move(command))                                   },
        {"future",    make_json(true)                                                 },
    });
}

auto sample_job(StandardAttributeRegistry const& registry, std::string_view id = "00112233-4455-6677-8899-aabbccddeeff")
    -> JobDefinition
{
    return {
        .id         = parse_uuid(id),
        .queue_id   = parse_uuid("10112233-4455-6677-8899-aabbccddeeff"),
        .revision   = 7,
        .name       = std::string{"nightly-export"},
        .state      = JobState::Suspended,
        .type       = JobType::Cli,
        .schedule   = OnceSchedule{.planned_at = parse_time("2026-07-21T21:00:00.123456Z")},
        .priority   = -4,
        .attributes = job_attributes(registry),
        .payload    = cli_payload(),
        .created_at = parse_time("2026-07-21T08:00:00.000000Z"),
        .updated_at = parse_time("2026-07-21T09:30:00.123456Z"),
        .deleted_at = parse_time("2026-07-21T10:00:00.000000Z"),
    };
}

auto valid_job_json(StandardAttributeRegistry const& registry) -> JsonValue
{
    auto encoded = job_to_json(sample_job(registry), registry);
    REQUIRE(encoded);
    return std::move(encoded).value();
}

class TrackingAttributeRegistry final : public AttributeRegistry {
public:
    [[nodiscard]] auto find(std::string_view name) const noexcept -> AttributeDefinition const* override
    {
        ++_find_calls;
        return _registry.find(name);
    }

    [[nodiscard]] auto validate(std::string_view name, AttributeValue const& value, AttributeScope scope) const
        -> Result<void, Error> override
    {
        return _registry.validate(name, value, scope);
    }

    [[nodiscard]] auto definitions() const -> std::span<AttributeDefinition const> override
    {
        return _registry.definitions();
    }

    [[nodiscard]] auto find_calls() const noexcept -> std::size_t { return _find_calls; }

private:
    StandardAttributeRegistry _registry;
    mutable std::size_t       _find_calls{0};
};

template <typename T>
auto check_invalid_request(Result<T, Error> const& result) -> void
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::InvalidArgument);
    CHECK(result.error().code == "jobu.protocol.invalid_request");
    CHECK(result.error().detail.empty());
}

template <typename T>
auto check_invalid_response(Result<T, Error> const& result) -> void
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::InvalidArgument);
    CHECK(result.error().code == "jobu.protocol.invalid_response");
    CHECK(result.error().detail.empty());
}

} // anonymous namespace

TEST_CASE("Job JSON uses the stable shape and round trips owning values", "[jobu][management][json][job]")
{
    StandardAttributeRegistry registry;
    auto                      encoded = valid_job_json(registry);
    auto const&               wire    = encoded.as_object();

    REQUIRE(wire.size() == 13U);
    CHECK(wire.at("id").as_string() == "00112233-4455-6677-8899-aabbccddeeff");
    CHECK(wire.at("queue_id").as_string() == "10112233-4455-6677-8899-aabbccddeeff");
    CHECK(wire.at("revision").as_uint() == 7U);
    CHECK(wire.at("name").as_string() == "nightly-export");
    CHECK(wire.at("state").as_string() == "suspended");
    CHECK(wire.at("type").as_string() == "cli");
    CHECK(wire.at("schedule").as_object().at("kind").as_string() == "once");
    CHECK(wire.at("schedule").as_object().at("at").as_string() == "2026-07-21T21:00:00.123456Z");
    CHECK(wire.at("priority").as_int() == -4);
    CHECK(wire.at("attributes").as_object().at("job.timeout").as_int() == 2500);
    CHECK(wire.at("payload").as_object().at("future").as_bool());
    CHECK(wire.at("created_at").as_string() == "2026-07-21T08:00:00.000000Z");
    CHECK(wire.at("updated_at").as_string() == "2026-07-21T09:30:00.123456Z");
    CHECK(wire.at("deleted_at").as_string() == "2026-07-21T10:00:00.000000Z");

    auto text = serialize_json(encoded);
    REQUIRE(text);
    auto parsed = parse_json(*text);
    REQUIRE(parsed);
    object(*parsed).emplace("future_job", make_json(true));
    object(object(*parsed).at("schedule")).emplace("future_schedule", make_json(true));
    auto decoded = job_from_json(*parsed, registry);
    REQUIRE(decoded);
    CHECK(decoded->id == parse_uuid("00112233-4455-6677-8899-aabbccddeeff"));
    CHECK(decoded->queue_id == parse_uuid("10112233-4455-6677-8899-aabbccddeeff"));
    CHECK(decoded->revision == 7U);
    CHECK(decoded->name == "nightly-export");
    CHECK(decoded->state == JobState::Suspended);
    CHECK(decoded->type == JobType::Cli);
    CHECK(std::get<OnceSchedule>(decoded->schedule).planned_at == parse_time("2026-07-21T21:00:00.123456Z"));
    CHECK(decoded->priority == -4);
    CHECK(decoded->attributes.size() == registry.definitions().size());
    CHECK(decoded->payload.as_object().at("future").as_bool());
    REQUIRE(decoded->deleted_at);

    object(object(*parsed).at("payload")).at("command") = make_json(std::string{"changed"});
    CHECK(decoded->payload.as_object().at("command").as_string() == "/usr/bin/example");

    object(*parsed).at("created_at") = make_json(std::string{"2026-07-21T08:00:00Z"});
    auto tolerant_time               = job_from_json(*parsed, registry);
    REQUIRE(tolerant_time);
    CHECK(tolerant_time->created_at == parse_time("2026-07-21T08:00:00Z"));
}

TEST_CASE("Job JSON covers optional values, cron, and every enum spelling", "[jobu][management][json][job][enum]")
{
    StandardAttributeRegistry registry;
    auto                      job = sample_job(registry);
    job.name.reset();
    job.deleted_at.reset();
    job.schedule = CronSchedule{.expression = "0 * * * *", .timezone = "Europe/Tallinn"};
    job.type     = JobType::Http;

    auto cron_json = job_to_json(job, registry);
    REQUIRE(cron_json);
    CHECK(cron_json->as_object().at("name").is_null());
    CHECK(cron_json->as_object().at("deleted_at").is_null());
    auto const& schedule = cron_json->as_object().at("schedule").as_object();
    CHECK(schedule.at("kind").as_string() == "cron");
    CHECK(schedule.at("expression").as_string() == "0 * * * *");
    CHECK(schedule.at("timezone").as_string() == "Europe/Tallinn");
    auto cron_decoded = job_from_json(*cron_json, registry);
    REQUIRE(cron_decoded);
    CHECK_FALSE(cron_decoded->name);
    CHECK_FALSE(cron_decoded->deleted_at);
    CHECK(std::get<CronSchedule>(cron_decoded->schedule).timezone == "Europe/Tallinn");

    for (auto const [state, text] : {
             std::pair{JobState::Active,     std::string_view{"active"}    },
             std::pair{JobState::Suspending, std::string_view{"suspending"}},
             std::pair{JobState::Suspended,  std::string_view{"suspended"} },
             std::pair{JobState::Deleted,    std::string_view{"deleted"}   },
    }) {
        job.state    = state;
        auto encoded = job_to_json(job, registry);
        REQUIRE(encoded);
        CHECK(encoded->as_object().at("state").as_string() == text);
        auto decoded = job_from_json(*encoded, registry);
        REQUIRE(decoded);
        CHECK(decoded->state == state);
    }
    for (auto const [type, text] : {
             std::pair{JobType::Cli,  std::string_view{"cli"} },
             std::pair{JobType::Http, std::string_view{"http"}},
    }) {
        job.type     = type;
        auto encoded = job_to_json(job, registry);
        REQUIRE(encoded);
        CHECK(encoded->as_object().at("type").as_string() == text);
        auto decoded = job_from_json(*encoded, registry);
        REQUIRE(decoded);
        CHECK(decoded->type == type);
    }
}

TEST_CASE("Job response conversion rejects invalid known fields", "[jobu][management][json][job][invalid]")
{
    StandardAttributeRegistry registry;

    check_invalid_response(job_from_json(make_json(JsonValue::Array{}), registry));

    auto missing = valid_job_json(registry);
    object(missing).erase("queue_id");
    check_invalid_response(job_from_json(missing, registry));

    auto bad_id             = valid_job_json(registry);
    object(bad_id).at("id") = make_json(std::string{"not-a-uuid"});
    check_invalid_response(job_from_json(bad_id, registry));

    auto noncanonical_queue                   = valid_job_json(registry);
    object(noncanonical_queue).at("queue_id") = make_json(std::string{"10112233-4455-6677-8899-AABBCCDDEEFF"});
    check_invalid_response(job_from_json(noncanonical_queue, registry));

    auto zero_revision                   = valid_job_json(registry);
    object(zero_revision).at("revision") = make_json(std::uint64_t{0});
    check_invalid_response(job_from_json(zero_revision, registry));

    auto bad_name               = valid_job_json(registry);
    object(bad_name).at("name") = make_json(false);
    check_invalid_response(job_from_json(bad_name, registry));

    auto bad_state                = valid_job_json(registry);
    object(bad_state).at("state") = make_json(std::string{"ACTIVE"});
    check_invalid_response(job_from_json(bad_state, registry));

    auto bad_type               = valid_job_json(registry);
    object(bad_type).at("type") = make_json(std::string{"worker"});
    check_invalid_response(job_from_json(bad_type, registry));

    auto bad_schedule                   = valid_job_json(registry);
    object(bad_schedule).at("schedule") = make_json(JsonValue::Object{
        {"kind", make_json(std::string{"once"})},
    });
    check_invalid_response(job_from_json(bad_schedule, registry));

    auto bad_cron                   = valid_job_json(registry);
    object(bad_cron).at("schedule") = make_json(JsonValue::Object{
        {"expression", make_json(std::string{"0 * * * *"})},
        {"kind",       make_json(std::string{"cron"})     },
        {"timezone",   make_json(JsonNull{})              },
    });
    check_invalid_response(job_from_json(bad_cron, registry));

    auto priority_overflow = valid_job_json(registry);
    object(priority_overflow).at("priority") =
        make_json(static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1U);
    check_invalid_response(job_from_json(priority_overflow, registry));

    auto incomplete = valid_job_json(registry);
    object(object(incomplete).at("attributes")).erase("job.timeout");
    check_invalid_response(job_from_json(incomplete, registry));

    auto bad_payload                  = valid_job_json(registry);
    object(bad_payload).at("payload") = make_json(JsonValue::Array{});
    check_invalid_response(job_from_json(bad_payload, registry));

    auto bad_time                     = valid_job_json(registry);
    object(bad_time).at("updated_at") = make_json(std::string{"2026-07-21T09:30:00+03:00"});
    check_invalid_response(job_from_json(bad_time, registry));

    auto bad_deleted                     = valid_job_json(registry);
    object(bad_deleted).at("deleted_at") = make_json(false);
    check_invalid_response(job_from_json(bad_deleted, registry));

    auto invalid_revision     = sample_job(registry);
    invalid_revision.revision = 0;
    check_invalid_response(job_to_json(invalid_revision, registry));

    auto invalid_state  = sample_job(registry);
    invalid_state.state = static_cast<JobState>(255);
    check_invalid_response(job_to_json(invalid_state, registry));

    auto invalid_type = sample_job(registry);
    invalid_type.type = static_cast<JobType>(255);
    check_invalid_response(job_to_json(invalid_type, registry));

    auto invalid_attributes                              = sample_job(registry);
    invalid_attributes.attributes.at("job.timeout").data = Duration{1};
    check_invalid_response(job_to_json(invalid_attributes, registry));

    auto invalid_payload    = sample_job(registry);
    invalid_payload.payload = make_json(JsonValue::Array{});
    check_invalid_response(job_to_json(invalid_payload, registry));
}

TEST_CASE("Job page JSON shares bounded ordering and cursor invariants", "[jobu][management][json][job][page]")
{
    StandardAttributeRegistry registry;
    auto                      first  = sample_job(registry, "00112233-4455-6677-8899-aabbccddeeff");
    auto                      second = sample_job(registry, "20112233-4455-6677-8899-aabbccddeeff");
    auto                      page   = JobPage{
        .items         = {first, second},
        .next_after_id = second.id,
    };
    auto encoded = job_page_to_json(page, registry);
    REQUIRE(encoded);
    object(*encoded).emplace("future_page", make_json(true));
    object(array(object(*encoded).at("items"))[0]).emplace("future_job", make_json(true));
    auto decoded = job_page_from_json(*encoded, registry);
    REQUIRE(decoded);
    REQUIRE(decoded->items.size() == 2U);
    CHECK(decoded->items[0].id == first.id);
    CHECK(decoded->items[1].id == second.id);
    CHECK(decoded->next_after_id == second.id);

    auto missing = *encoded;
    object(missing).erase("next_after_id");
    check_invalid_response(job_page_from_json(missing, registry));

    auto bad_items                = *encoded;
    object(bad_items).at("items") = make_json(JsonValue::Object{});
    check_invalid_response(job_page_from_json(bad_items, registry));

    auto bad_next                        = *encoded;
    object(bad_next).at("next_after_id") = make_json(std::string{"bad"});
    check_invalid_response(job_page_from_json(bad_next, registry));

    auto  decoded_out_of_order = *encoded;
    auto& decoded_items        = array(object(decoded_out_of_order).at("items"));
    std::swap(decoded_items[0], decoded_items[1]);
    check_invalid_response(job_page_from_json(decoded_out_of_order, registry));

    auto decoded_cursor_mismatch                        = *encoded;
    object(decoded_cursor_mismatch).at("next_after_id") = make_json(first.id.to_string());
    check_invalid_response(job_page_from_json(decoded_cursor_mismatch, registry));

    auto mismatched          = page;
    mismatched.next_after_id = first.id;
    check_invalid_response(job_page_to_json(mismatched, registry));

    auto out_of_order = JobPage{
        .items = {second, first}
    };
    check_invalid_response(job_page_to_json(out_of_order, registry));

    auto oversized = JobPage{};
    oversized.items.resize(201U, first);
    check_invalid_response(job_page_to_json(oversized, registry));

    auto oversized_items   = JsonValue::Array(201U, valid_job_json(registry));
    auto tracking_registry = TrackingAttributeRegistry{};
    check_invalid_response(job_page_from_json(make_json(JsonValue::Object{
                                                  {"items",         make_json(std::move(oversized_items))},
                                                  {"next_after_id", make_json(JsonNull{})                },
    }),
                                              tracking_registry));
    CHECK(tracking_registry.find_calls() == 0U);
}

TEST_CASE("Job ID parameters use one strict canonical shape", "[jobu][management][json][job][id]")
{
    auto const id      = parse_uuid("00112233-4455-6677-8899-aabbccddeeff");
    auto       encoded = job_id_to_json(id);
    REQUIRE(encoded);
    REQUIRE(encoded->as_object().size() == 1U);
    CHECK(encoded->as_object().at("job_id").as_string() == id.to_string());
    auto decoded = job_id_from_json(*encoded);
    REQUIRE(decoded);
    CHECK(*decoded == id);

    check_invalid_request(job_id_from_json(make_json(JsonNull{})));
    check_invalid_request(job_id_from_json(make_json(JsonValue::Object{})));
    check_invalid_request(job_id_from_json(make_json(JsonValue::Object{
        {"job_id", make_json(std::string{"bad"})},
    })));
    check_invalid_request(job_id_from_json(make_json(JsonValue::Object{
        {"job_id", make_json(std::string{"00112233-4455-6677-8899-AABBCCDDEEFF"})},
    })));
    check_invalid_request(job_id_from_json(make_json(JsonValue::Object{
        {"future", make_json(true)          },
        {"job_id", make_json(id.to_string())},
    })));
}

TEST_CASE("Create job request JSON preserves selectors, schedules, attributes, and payload ownership",
          "[jobu][management][json][job][create]")
{
    StandardAttributeRegistry registry;
    auto                      request = CreateJobRequest{
        .queue           = std::string{"default"},
        .name            = std::string{"nightly-export"},
        .type            = JobType::Http,
        .schedule        = CronSchedule{.expression = "0 * * * *", .timezone = "UTC"},
        .priority        = 9,
        .attributes      = {{"job.timeout", {.data = Duration{3s}}}},
        .payload         = make_json(JsonValue::Object{
                                       {"future", make_json(true)},
                                       {"url", make_json(std::string{"https://example"})},
                                       }
                    ),
        .idempotency_key = std::string{"create-export-1"},
    };
    auto encoded = create_job_request_to_json(request, registry);
    REQUIRE(encoded);
    auto const& wire = encoded->as_object();
    CHECK(wire.at("queue_name").as_string() == "default");
    CHECK(wire.at("name").as_string() == "nightly-export");
    CHECK(wire.at("type").as_string() == "http");
    CHECK(wire.at("schedule").as_object().at("kind").as_string() == "cron");
    CHECK(wire.at("priority").as_int() == 9);
    CHECK(wire.at("attributes").as_object().at("job.timeout").as_int() == 3000);
    CHECK(wire.at("payload").as_object().at("future").as_bool());
    CHECK(wire.at("idempotency_key").as_string() == "create-export-1");

    auto decoded = create_job_request_from_json(*encoded, registry);
    REQUIRE(decoded);
    CHECK(std::get<std::string>(decoded->queue) == "default");
    CHECK(decoded->name == "nightly-export");
    CHECK(decoded->type == JobType::Http);
    CHECK(std::get<CronSchedule>(decoded->schedule).expression == "0 * * * *");
    CHECK(decoded->priority == 9);
    REQUIRE(decoded->attributes.size() == 1U);
    CHECK(decoded->payload.as_object().at("future").as_bool());
    CHECK(decoded->idempotency_key == "create-export-1");

    object(object(*encoded).at("payload")).at("url") = make_json(std::string{"changed"});
    CHECK(decoded->payload.as_object().at("url").as_string() == "https://example");

    auto const queue_id = parse_uuid("10112233-4455-6677-8899-aabbccddeeff");
    auto       minimal  = create_job_request_from_json(make_json(JsonValue::Object{
                                                           {"payload",  make_json(JsonValue::Object{}) },
                                                           {"queue_id", make_json(queue_id.to_string())},
                                                           {"schedule",
                                                            make_json(JsonValue::Object{
                                                                {"at", make_json(std::string{"2026-07-21T21:00:00Z"})},
                                                                {"kind", make_json(std::string{"once"})},
                                                            })                                         },
    }),
                                                       registry);
    REQUIRE(minimal);
    CHECK(std::get<Uuid>(minimal->queue) == queue_id);
    CHECK_FALSE(minimal->name);
    CHECK(minimal->type == JobType::Cli);
    CHECK(minimal->priority == 0);
    CHECK(minimal->attributes.empty());
    CHECK_FALSE(minimal->idempotency_key);
    CHECK(std::get<OnceSchedule>(minimal->schedule).planned_at == parse_time("2026-07-21T21:00:00Z"));
}

TEST_CASE("Create job request decoding is strict without taking service policy",
          "[jobu][management][json][job][create][invalid]")
{
    StandardAttributeRegistry registry;
    auto const                id    = parse_uuid("10112233-4455-6677-8899-aabbccddeeff");
    auto                      valid = make_json(JsonValue::Object{
        {"payload",  make_json(JsonValue::Object{})},
        {"queue_id", make_json(id.to_string())     },
        {"schedule",
         make_json(JsonValue::Object{
             {"at", make_json(std::string{"2026-07-21T21:00:00Z"})},
             {"kind", make_json(std::string{"once"})},
         })                                        },
    });
    REQUIRE(create_job_request_from_json(valid, registry));

    check_invalid_request(create_job_request_from_json(make_json(JsonNull{}), registry));
    auto missing_selector = valid;
    object(missing_selector).erase("queue_id");
    check_invalid_request(create_job_request_from_json(missing_selector, registry));

    auto both_selectors = valid;
    object(both_selectors).emplace("queue_name", make_json(std::string{"default"}));
    check_invalid_request(create_job_request_from_json(both_selectors, registry));

    auto missing_schedule = valid;
    object(missing_schedule).erase("schedule");
    check_invalid_request(create_job_request_from_json(missing_schedule, registry));

    auto missing_payload = valid;
    object(missing_payload).erase("payload");
    check_invalid_request(create_job_request_from_json(missing_payload, registry));

    auto unknown = valid;
    object(unknown).emplace("future", make_json(true));
    check_invalid_request(create_job_request_from_json(unknown, registry));

    auto null_name = valid;
    object(null_name).emplace("name", make_json(JsonNull{}));
    check_invalid_request(create_job_request_from_json(null_name, registry));

    auto bad_type = valid;
    object(bad_type).emplace("type", make_json(std::string{"worker"}));
    check_invalid_request(create_job_request_from_json(bad_type, registry));

    auto nested_unknown = valid;
    object(object(nested_unknown).at("schedule")).emplace("future", make_json(true));
    check_invalid_request(create_job_request_from_json(nested_unknown, registry));

    auto priority_overflow = valid;
    object(priority_overflow)
        .emplace("priority", make_json(static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1U));
    check_invalid_request(create_job_request_from_json(priority_overflow, registry));

    auto bad_attributes = valid;
    object(bad_attributes)
        .emplace("attributes",
                 make_json(JsonValue::Object{
                     {"missing", make_json(true)},
    }));
    check_invalid_request(create_job_request_from_json(bad_attributes, registry));

    auto bad_payload                  = valid;
    object(bad_payload).at("payload") = make_json(JsonValue::Array{});
    check_invalid_request(create_job_request_from_json(bad_payload, registry));

    auto null_key = valid;
    object(null_key).emplace("idempotency_key", make_json(JsonNull{}));
    check_invalid_request(create_job_request_from_json(null_key, registry));

    auto invalid_enum = CreateJobRequest{
        .queue    = QueueSelector{id},
        .type     = static_cast<JobType>(255),
        .schedule = OnceSchedule{.planned_at = parse_time("2026-07-21T21:00:00Z")},
        .payload  = make_json(JsonValue::Object{}),
    };
    check_invalid_request(create_job_request_to_json(invalid_enum, registry));

    auto invalid_payload    = invalid_enum;
    invalid_payload.type    = JobType::Cli;
    invalid_payload.payload = make_json(JsonValue::Array{});
    check_invalid_request(create_job_request_to_json(invalid_payload, registry));
}

TEST_CASE("Job list request JSON round trips filters and defaults", "[jobu][management][json][job][list]")
{
    auto const queue_id = parse_uuid("10112233-4455-6677-8899-aabbccddeeff");
    auto const after_id = parse_uuid("20112233-4455-6677-8899-aabbccddeeff");
    auto       request  = JobListRequest{
        .queue           = QueueSelector{queue_id},
        .include_deleted = true,
        .state           = JobState::Suspending,
        .type            = JobType::Http,
        .page            = {.limit = 42, .after_id = after_id},
    };
    auto encoded = job_list_request_to_json(request);
    REQUIRE(encoded);
    CHECK(encoded->as_object().at("queue_id").as_string() == queue_id.to_string());
    CHECK(encoded->as_object().at("state").as_string() == "suspending");
    CHECK(encoded->as_object().at("type").as_string() == "http");
    auto decoded = job_list_request_from_json(*encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->queue);
    CHECK(std::get<Uuid>(*decoded->queue) == queue_id);
    CHECK(decoded->include_deleted);
    CHECK(decoded->state == JobState::Suspending);
    CHECK(decoded->type == JobType::Http);
    CHECK(decoded->page.limit == 42U);
    CHECK(decoded->page.after_id == after_id);

    auto defaults = job_list_request_from_json(make_json(JsonValue::Object{}));
    REQUIRE(defaults);
    CHECK_FALSE(defaults->queue);
    CHECK_FALSE(defaults->include_deleted);
    CHECK_FALSE(defaults->state);
    CHECK_FALSE(defaults->type);
    CHECK(defaults->page.limit == 100U);
    CHECK_FALSE(defaults->page.after_id);
}

TEST_CASE("Job list request decoding rejects ambiguous and malformed filters",
          "[jobu][management][json][job][list][invalid]")
{
    auto const id = parse_uuid("10112233-4455-6677-8899-aabbccddeeff");
    check_invalid_request(job_list_request_from_json(make_json(JsonNull{})));
    check_invalid_request(job_list_request_from_json(make_json(JsonValue::Object{
        {"future", make_json(true)},
    })));
    check_invalid_request(job_list_request_from_json(make_json(JsonValue::Object{
        {"queue_id",   make_json(id.to_string())        },
        {"queue_name", make_json(std::string{"default"})},
    })));
    check_invalid_request(job_list_request_from_json(make_json(JsonValue::Object{
        {"include_deleted", make_json(std::uint64_t{1})},
    })));
    check_invalid_request(job_list_request_from_json(make_json(JsonValue::Object{
        {"state", make_json(std::string{"ACTIVE"})},
    })));
    check_invalid_request(job_list_request_from_json(make_json(JsonValue::Object{
        {"type", make_json(std::string{"worker"})},
    })));
    check_invalid_request(job_list_request_from_json(make_json(JsonValue::Object{
        {"limit", make_json(std::uint64_t{0})},
    })));
    check_invalid_request(job_list_request_from_json(make_json(JsonValue::Object{
        {"limit", make_json(std::uint64_t{201})},
    })));
    check_invalid_request(job_list_request_from_json(make_json(JsonValue::Object{
        {"after_id", make_json(std::string{"bad"})},
    })));

    auto invalid_state  = JobListRequest{};
    invalid_state.state = static_cast<JobState>(255);
    check_invalid_request(job_list_request_to_json(invalid_state));

    auto invalid_type = JobListRequest{};
    invalid_type.type = static_cast<JobType>(255);
    check_invalid_request(job_list_request_to_json(invalid_type));

    auto invalid_limit       = JobListRequest{};
    invalid_limit.page.limit = 201;
    check_invalid_request(job_list_request_to_json(invalid_limit));
}

TEST_CASE("Update job request JSON preserves nested optionals and partial changes",
          "[jobu][management][json][job][update]")
{
    StandardAttributeRegistry registry;
    auto const                id      = parse_uuid("00112233-4455-6677-8899-aabbccddeeff");
    auto                      request = UpdateJobRequest{
        .job_id            = id,
        .expected_revision = 7,
        .type              = JobType::Http,
        .schedule          = CronSchedule{.expression = "0 * * * *", .timezone = "UTC"},
        .priority          = -2,
        .attribute_changes = {{"job.timeout", {.data = Duration{4s}}}},
        .payload           = make_json(JsonValue::Object{
                                          {"url", make_json(std::string{"https://example"})},
                                          }
                   ),
    };
    request.name.emplace(std::nullopt);
    auto encoded = update_job_request_to_json(request, registry);
    REQUIRE(encoded);
    CHECK(encoded->as_object().at("job_id").as_string() == id.to_string());
    CHECK(encoded->as_object().at("expected_revision").as_uint() == 7U);
    CHECK(encoded->as_object().at("name").is_null());
    CHECK(encoded->as_object().at("type").as_string() == "http");
    CHECK(encoded->as_object().at("schedule").as_object().at("kind").as_string() == "cron");
    CHECK(encoded->as_object().at("attributes").as_object().size() == 1U);
    auto decoded = update_job_request_from_json(*encoded, registry);
    REQUIRE(decoded);
    REQUIRE(decoded->name);
    CHECK_FALSE(*decoded->name);
    CHECK(decoded->type == JobType::Http);
    CHECK(std::get<CronSchedule>(*decoded->schedule).expression == "0 * * * *");
    CHECK(decoded->priority == -2);
    REQUIRE(decoded->attribute_changes.size() == 1U);
    REQUIRE(decoded->payload);
    CHECK(decoded->payload->as_object().at("url").as_string() == "https://example");

    object(object(*encoded).at("payload")).at("url") = make_json(std::string{"changed"});
    CHECK(decoded->payload->as_object().at("url").as_string() == "https://example");

    auto set_name = update_job_request_from_json(make_json(JsonValue::Object{
                                                     {"expected_revision", make_json(std::uint64_t{0})  },
                                                     {"job_id",            make_json(id.to_string())    },
                                                     {"name",              make_json(std::string{"new"})},
    }),
                                                 registry);
    REQUIRE(set_name);
    CHECK(set_name->expected_revision == 0U);
    REQUIRE(set_name->name);
    REQUIRE(*set_name->name);
    CHECK(**set_name->name == "new");

    auto omitted_name = update_job_request_from_json(make_json(JsonValue::Object{
                                                         {"expected_revision", make_json(std::uint64_t{7})},
                                                         {"job_id",            make_json(id.to_string())  },
                                                         {"priority",          make_json(std::int64_t{2}) },
    }),
                                                     registry);
    REQUIRE(omitted_name);
    CHECK_FALSE(omitted_name->name);
}

TEST_CASE("Update job request decoding enforces strict structure and an effective change",
          "[jobu][management][json][job][update][invalid]")
{
    StandardAttributeRegistry registry;
    auto const                id   = parse_uuid("00112233-4455-6677-8899-aabbccddeeff");
    auto                      base = make_json(JsonValue::Object{
        {"expected_revision", make_json(std::uint64_t{7})},
        {"job_id",            make_json(id.to_string())  },
    });
    check_invalid_request(update_job_request_from_json(base, registry));

    auto missing_revision = base;
    object(missing_revision).erase("expected_revision");
    object(missing_revision).emplace("priority", make_json(std::int64_t{1}));
    check_invalid_request(update_job_request_from_json(missing_revision, registry));

    auto unknown = base;
    object(unknown).emplace("future", make_json(true));
    object(unknown).emplace("priority", make_json(std::int64_t{1}));
    check_invalid_request(update_job_request_from_json(unknown, registry));

    auto empty_attributes = base;
    object(empty_attributes).emplace("attributes", make_json(JsonValue::Object{}));
    check_invalid_request(update_job_request_from_json(empty_attributes, registry));

    auto bad_name = base;
    object(bad_name).emplace("name", make_json(false));
    check_invalid_request(update_job_request_from_json(bad_name, registry));

    auto bad_schedule = base;
    object(bad_schedule)
        .emplace("schedule",
                 make_json(JsonValue::Object{
                     {"at",     make_json(std::string{"2026-07-21T21:00:00Z"})},
                     {"future", make_json(true)                               },
                     {"kind",   make_json(std::string{"once"})                },
    }));
    check_invalid_request(update_job_request_from_json(bad_schedule, registry));

    auto bad_priority = base;
    object(bad_priority)
        .emplace("priority", make_json(static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1U));
    check_invalid_request(update_job_request_from_json(bad_priority, registry));

    auto bad_payload = base;
    object(bad_payload).emplace("payload", make_json(JsonValue::Array{}));
    check_invalid_request(update_job_request_from_json(bad_payload, registry));

    auto invalid_revision                            = base;
    object(invalid_revision).at("expected_revision") = make_json(std::int64_t{-1});
    object(invalid_revision).emplace("priority", make_json(std::int64_t{1}));
    check_invalid_request(update_job_request_from_json(invalid_revision, registry));

    auto empty = UpdateJobRequest{.job_id = id, .expected_revision = 7};
    check_invalid_request(update_job_request_to_json(empty, registry));

    auto invalid_enum = empty;
    invalid_enum.type = static_cast<JobType>(255);
    check_invalid_request(update_job_request_to_json(invalid_enum, registry));

    auto invalid_payload    = empty;
    invalid_payload.payload = make_json(JsonValue::Array{});
    check_invalid_request(update_job_request_to_json(invalid_payload, registry));
}

TEST_CASE("Move and delete job parameters share revision and selector rules",
          "[jobu][management][json][job][move][delete]")
{
    auto const job_id   = parse_uuid("00112233-4455-6677-8899-aabbccddeeff");
    auto const queue_id = parse_uuid("10112233-4455-6677-8899-aabbccddeeff");
    auto       move     = MoveJobRequest{
        .job_id            = job_id,
        .expected_revision = 0,
        .target_queue      = QueueSelector{queue_id},
    };
    auto encoded_move = move_job_request_to_json(move);
    REQUIRE(encoded_move);
    CHECK(encoded_move->as_object().at("target_queue_id").as_string() == queue_id.to_string());
    auto decoded_move = move_job_request_from_json(*encoded_move);
    REQUIRE(decoded_move);
    CHECK(decoded_move->job_id == job_id);
    CHECK(decoded_move->expected_revision == 0U);
    CHECK(std::get<Uuid>(decoded_move->target_queue) == queue_id);

    move.target_queue = std::string{"archive"};
    encoded_move      = move_job_request_to_json(move);
    REQUIRE(encoded_move);
    CHECK(encoded_move->as_object().at("target_queue_name").as_string() == "archive");

    auto both_targets = *encoded_move;
    object(both_targets).emplace("target_queue_id", make_json(queue_id.to_string()));
    check_invalid_request(move_job_request_from_json(both_targets));

    auto missing_target = *encoded_move;
    object(missing_target).erase("target_queue_name");
    check_invalid_request(move_job_request_from_json(missing_target));

    auto unknown = *encoded_move;
    object(unknown).emplace("future", make_json(true));
    check_invalid_request(move_job_request_from_json(unknown));

    auto negative_revision                            = *encoded_move;
    object(negative_revision).at("expected_revision") = make_json(std::int64_t{-1});
    check_invalid_request(move_job_request_from_json(negative_revision));

    auto deletion       = DeleteJobRequest{.job_id = job_id, .expected_revision = 0};
    auto encoded_delete = delete_job_request_to_json(deletion);
    REQUIRE(encoded_delete);
    auto decoded_delete = delete_job_request_from_json(*encoded_delete);
    REQUIRE(decoded_delete);
    CHECK(decoded_delete->job_id == job_id);
    CHECK(decoded_delete->expected_revision == 0U);

    auto missing_delete_revision = *encoded_delete;
    object(missing_delete_revision).erase("expected_revision");
    check_invalid_request(delete_job_request_from_json(missing_delete_revision));

    auto invalid_delete_id                 = *encoded_delete;
    object(invalid_delete_id).at("job_id") = make_json(std::string{"bad"});
    check_invalid_request(delete_job_request_from_json(invalid_delete_id));

    auto unknown_delete = *encoded_delete;
    object(unknown_delete).emplace("future", make_json(true));
    check_invalid_request(delete_job_request_from_json(unknown_delete));
}
