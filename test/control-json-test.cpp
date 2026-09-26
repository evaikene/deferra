#include "control_json.hpp"

#include "attribute_registry.hpp"
#include "history_json.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

using namespace jb::core;
using namespace jb::jobu;

namespace {

auto json(auto value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto id(std::string_view text) -> Uuid
{
    auto parsed = Uuid::parse(text);
    REQUIRE(parsed);
    return *parsed;
}

auto at(std::string_view text) -> UtcTimePoint
{
    auto parsed = parse_utc_timestamp(text);
    REQUIRE(parsed);
    return *parsed;
}

template <typename T>
void invalid_request(Result<T, Error> const& result)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.protocol.invalid_request");
}

template <typename T>
void invalid_response(Result<T, Error> const& result)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "jobu.protocol.invalid_response");
}

} // namespace

TEST_CASE("Run control requests require canonical IDs and strict fields", "[jobu][control][json]")
{
    auto const job     = id("00112233-4455-6677-8899-aabbccddeeff");
    auto       request = RunNowRequest{.job_id = job, .idempotency_key = "retry-one"};
    auto       encoded = run_now_request_to_json(request);
    REQUIRE(encoded);
    auto decoded = run_now_request_from_json(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->job_id == job);
    CHECK(decoded->idempotency_key == "retry-one");

    auto extra = *encoded;
    std::get<JsonValue::Object>(extra.data).emplace("future", json(true));
    invalid_request(run_now_request_from_json(extra));

    auto null_key                                                 = *encoded;
    std::get<JsonValue::Object>(null_key.data)["idempotency_key"] = json(JsonNull{});
    invalid_request(run_now_request_from_json(null_key));

    auto uppercase                                        = *encoded;
    std::get<JsonValue::Object>(uppercase.data)["job_id"] = json(std::string{"00112233-4455-6677-8899-AABBCCDDEEFF"});
    invalid_request(run_now_request_from_json(uppercase));

    auto cancel = cancel_run_request_to_json(job);
    REQUIRE(cancel);
    CHECK(cancel_run_request_from_json(*cancel).value() == job);
    invalid_request(cancel_run_request_from_json(json(JsonValue::Object{})));
    invalid_request(cancel_run_request_from_json(*encoded));
}

TEST_CASE("Cron preview codecs keep UTC and count defaults while rejecting non-cron shapes", "[jobu][control][json]")
{
    auto schedule = CronSchedule{.expression = "@daily", .timezone = "Europe/Tallinn"};
    auto preview  = ScheduleNextRequest{.schedule = schedule, .after = at("2026-01-01T00:00:00Z"), .count = 7};
    auto encoded  = schedule_next_request_to_json(preview);
    REQUIRE(encoded);
    auto decoded = schedule_next_request_from_json(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->schedule.expression == schedule.expression);
    CHECK(decoded->schedule.timezone == schedule.timezone);
    CHECK(decoded->after == preview.after);
    CHECK(decoded->count == 7);

    auto defaults = *encoded;
    std::get<JsonValue::Object>(defaults.data).erase("count");
    REQUIRE(schedule_next_request_from_json(defaults));
    CHECK(schedule_next_request_from_json(defaults)->count == 5);

    auto malformed                                       = defaults;
    std::get<JsonValue::Object>(malformed.data)["after"] = json(std::string{"2026-01-01T02:00:00+02:00"});
    invalid_request(schedule_next_request_from_json(malformed));

    malformed                                               = defaults;
    std::get<JsonValue::Object>(malformed.data)["schedule"] = json(JsonValue::Object{
        {"kind", json(std::string{"once"})                },
        {"at",   json(std::string{"2026-01-01T00:00:00Z"})},
    });
    invalid_request(schedule_next_request_from_json(malformed));
    invalid_request(schedule_validate_request_from_json(malformed));

    auto count                                       = defaults;
    std::get<JsonValue::Object>(count.data)["count"] = json(std::int64_t{-1});
    invalid_request(schedule_next_request_from_json(count));
    std::get<JsonValue::Object>(count.data)["count"] = json(std::uint64_t{201});
    REQUIRE(schedule_next_request_from_json(count)); // Domain helper reports jobu.schedule.invalid_count.

    auto validation = schedule_validate_request_to_json(schedule);
    REQUIRE(validation);
    CHECK(schedule_validate_request_from_json(*validation)->expression == schedule.expression);
    REQUIRE(schedule_validate_result_from_json(schedule_validate_result_to_json()));
    invalid_response(schedule_validate_result_from_json(json(JsonValue::Object{
        {"valid", json(false)}
    })));

    auto occurrences = std::vector<UtcTimePoint>{at("2026-01-02T00:00:00Z"), at("2026-01-03T00:00:00Z")};
    auto result      = schedule_next_result_to_json(occurrences);
    REQUIRE(result);
    auto decoded_result = schedule_next_result_from_json(*result);
    REQUIRE(decoded_result);
    CHECK(*decoded_result == occurrences);
    std::get<JsonValue::Object>(result->data).emplace("future", json(true));
    REQUIRE(schedule_next_result_from_json(*result));
}

TEST_CASE("Run controls reuse the full durable template view", "[jobu][control][json]")
{
    StandardAttributeRegistry registry;
    auto                      attributes = materialize_attributes(registry, {}, {}, {});
    REQUIRE(attributes);

    auto run           = JobRun{};
    run.id             = id("00112233-4455-6677-8899-aabbccddeeff");
    run.job_id         = id("10112233-4455-6677-8899-aabbccddeeff");
    run.queue_id       = id("20112233-4455-6677-8899-aabbccddeeff");
    run.origin         = RunOrigin::Manual;
    run.schedule_owned = false;
    run.planned_at     = at("2026-01-01T00:00:00Z");
    run.runnable_at    = run.planned_at;
    run.attributes     = std::move(*attributes);
    run.payload        = json(JsonValue::Object{
        {"command", json(std::string{"/bin/true"})                                                          },
        {"args",    json(JsonValue::Array{json(JsonValue::Object{{"$secret", json(std::string{"token"})}})})}
    });

    auto encoded = run_details_to_json(run, registry);
    REQUIRE(encoded);
    CHECK(encoded->as_object().at("payload") == run.payload);
    CHECK(encoded->as_object().at("result").is_null());
    CHECK_FALSE(encoded->as_object().contains("output"));
    auto decoded = run_details_from_json(*encoded, registry);
    REQUIRE(decoded);
    CHECK(decoded->id == run.id);
    CHECK(decoded->attributes.size() == run.attributes.size());
    CHECK(decoded->payload == run.payload);

    auto cancel = cancel_run_result_to_json({.run = run, .disposition = CancelDisposition::Requested}, registry);
    REQUIRE(cancel);
    auto decoded_cancel = cancel_run_result_from_json(*cancel, registry);
    REQUIRE(decoded_cancel);
    CHECK(decoded_cancel->disposition == CancelDisposition::Requested);
    CHECK(decoded_cancel->run.id == run.id);

    std::get<JsonValue::Object>(encoded->data).erase("attributes");
    invalid_response(run_details_from_json(*encoded, registry));
}
