#include "history_json.hpp"

#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

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

TEST_CASE("Run history requests preserve filters and require cursor-only continuation", "[jobu][history][json]")
{
    auto const queue   = id("00112233-4455-6677-8899-aabbccddeeff");
    auto       request = RunListRequest{
        RunQuery{.filters = {.queue_id = queue,
                             .state    = RunState::RetryWait,
                             .origin   = RunOrigin::Manual,
                             .type     = JobType::Http,
                             .planned  = {.from = at("2026-01-01T00:00:00Z"), .to = at("2026-02-01T00:00:00Z")},
                             .started  = {.from = at("2026-01-03T00:00:00Z")}},
                 .limit   = 200}
    };
    auto encoded = run_list_request_to_json(request);
    REQUIRE(encoded);
    auto decoded = run_list_request_from_json(*encoded);
    REQUIRE(decoded);
    REQUIRE(std::holds_alternative<RunQuery>(*decoded));
    auto const& query = std::get<RunQuery>(*decoded);
    CHECK(query.filters.queue_id == queue);
    CHECK(query.filters.state == RunState::RetryWait);
    CHECK(query.filters.origin == RunOrigin::Manual);
    CHECK(query.filters.type == JobType::Http);
    CHECK(query.filters.planned.from == at("2026-01-01T00:00:00Z"));
    CHECK(query.filters.planned.to == at("2026-02-01T00:00:00Z"));
    CHECK(query.filters.started.from == at("2026-01-03T00:00:00Z"));
    CHECK(query.limit == 200);

    auto continuation = json(JsonValue::Object{
        {"cursor", json(std::string{"opaque-token"})}
    });
    auto resumed      = run_list_request_from_json(continuation);
    REQUIRE(resumed);
    CHECK(std::get<CursorRequest>(*resumed).cursor == "opaque-token");
    auto round_trip = run_list_request_to_json(*resumed);
    REQUIRE(round_trip);
    CHECK(*round_trip == continuation);

    std::get<JsonValue::Object>(continuation.data).emplace("limit", json(std::uint64_t{5}));
    invalid_request(run_list_request_from_json(continuation));
    std::get<JsonValue::Object>(continuation.data).erase("limit");
    std::get<JsonValue::Object>(continuation.data).emplace("state", json(std::string{"running"}));
    invalid_request(run_list_request_from_json(continuation));
}

TEST_CASE("Run history request validation rejects invalid ranges, enums, and limits", "[jobu][history][json]")
{
    auto const lower = std::string{"2026-01-01T00:00:00Z"};
    auto const upper = std::string{"2026-02-01T00:00:00Z"};
    auto       bad   = json(JsonValue::Object{
        {"planned", json(JsonValue::Object{{"from", json(upper)}, {"to", json(lower)}})}
    });
    invalid_request(run_list_request_from_json(bad));
    invalid_request(run_list_request_to_json(RunQuery{.filters = {.planned = {.from = at(lower), .to = at(lower)}}}));

    for (auto const* name : {"state", "origin", "type"}) {
        invalid_request(run_list_request_from_json(json(JsonValue::Object{
            {name, json(std::string{"unknown"})}
        })));
    }
    invalid_request(run_list_request_from_json(json(JsonValue::Object{
        {"origin", json(std::string{"submitted"})}
    })));
    invalid_request(run_list_request_to_json(RunQuery{.filters = {.origin = RunOrigin::Submitted}}));
    invalid_request(run_list_request_from_json(json(JsonValue::Object{
        {"limit", json(std::uint64_t{0})}
    })));
    invalid_request(run_list_request_from_json(json(JsonValue::Object{
        {"limit", json(std::uint64_t{201})}
    })));
    invalid_request(run_list_request_from_json(json(JsonValue::Object{
        {"completed", json(JsonNull{})}
    })));
    invalid_request(run_list_request_from_json(json(JsonValue::Object{
        {"unexpected", json(true)}
    })));
}

TEST_CASE("Attempt history requests require one run and retain their page size", "[jobu][history][json]")
{
    auto const run     = id("10112233-4455-6677-8899-aabbccddeeff");
    auto       encoded = attempt_list_request_to_json(AttemptQuery{.run_id = run, .limit = 17});
    REQUIRE(encoded);
    auto decoded = attempt_list_request_from_json(*encoded);
    REQUIRE(decoded);
    REQUIRE(std::holds_alternative<AttemptQuery>(*decoded));
    CHECK(std::get<AttemptQuery>(*decoded).run_id == run);
    CHECK(std::get<AttemptQuery>(*decoded).limit == 17);

    invalid_request(attempt_list_request_from_json(json(JsonValue::Object{})));
    invalid_request(attempt_list_request_from_json(json(JsonValue::Object{
        {"run_id", json(run.to_string())   },
        {"limit",  json(std::uint64_t{201})}
    })));
    invalid_request(attempt_list_request_from_json(json(JsonValue::Object{
        {"cursor", json(std::string{"token"})},
        {"run_id", json(run.to_string())     }
    })));
}

TEST_CASE("History summaries encode only lightweight fields", "[jobu][history][json]")
{
    auto run        = RunDetails{};
    run.id          = id("00112233-4455-6677-8899-aabbccddeeff");
    run.job_id      = id("10112233-4455-6677-8899-aabbccddeeff");
    run.queue_id    = id("20112233-4455-6677-8899-aabbccddeeff");
    run.planned_at  = at("2026-01-01T00:00:00Z");
    run.runnable_at = run.planned_at;
    run.payload     = json(JsonValue::Object{
        {"secret", json(std::string{"a.token"})}
    });
    run.result      = json(JsonValue::Object{
        {"private", json(std::string{"heavy"})}
    });

    auto encoded = run_summary_to_json(run);
    REQUIRE(encoded);
    auto const& fields = encoded->as_object();
    CHECK_FALSE(fields.contains("attributes"));
    CHECK_FALSE(fields.contains("payload"));
    CHECK_FALSE(fields.contains("result"));
    CHECK_FALSE(fields.contains("output"));
    CHECK(fields.at("started_at").is_null());
    auto decoded = run_summary_from_json(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->id == run.id);
    CHECK(decoded->planned_at == run.planned_at);

    // An additive future response field is tolerated; a known required field remains mandatory.
    auto future_run = *encoded;
    std::get<JsonValue::Object>(future_run.data).emplace("future", json(true));
    REQUIRE(run_summary_from_json(future_run));
    std::get<JsonValue::Object>(future_run.data).erase("planned_at");
    invalid_response(run_summary_from_json(future_run));

    auto attempt           = AttemptDetails{};
    attempt.run_id         = run.id;
    attempt.attempt_number = 2;
    attempt.due_at         = run.planned_at;
    attempt.state          = AttemptState::Completed;
    attempt.outcome        = AttemptOutcome::Succeeded;
    attempt.result         = json(JsonValue::Object{
        {"private", json(std::string{"heavy"})}
    });
    auto encoded_attempt   = attempt_summary_to_json(attempt);
    REQUIRE(encoded_attempt);
    CHECK_FALSE(encoded_attempt->as_object().contains("result"));
    CHECK_FALSE(encoded_attempt->as_object().contains("output"));
    auto decoded_attempt = attempt_summary_from_json(*encoded_attempt);
    REQUIRE(decoded_attempt);
    CHECK(decoded_attempt->attempt_number == 2);
    CHECK(decoded_attempt->outcome == AttemptOutcome::Succeeded);

    auto invalid_attempt                                         = *encoded_attempt;
    std::get<JsonValue::Object>(invalid_attempt.data)["outcome"] = json(std::string{"unknown"});
    invalid_response(attempt_summary_from_json(invalid_attempt));
}
