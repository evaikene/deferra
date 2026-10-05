#include "statistics_json.hpp"

#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
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

TEST_CASE("Statistics request codecs distinguish system filters from queue selectors", "[jobu][statistics][json]")
{
    auto const queue = id("00112233-4455-6677-8899-aabbccddeeff");
    auto const job   = id("10112233-4455-6677-8899-aabbccddeeff");
    auto       query = StatisticsRequest{
        .queue_id = queue,
        .job_id   = job,
        .type     = JobType::Http,
        .origin   = RunOrigin::Manual,
        .planned  = {.from = at("2026-01-01T00:00:00Z"), .to = at("2026-01-02T00:00:00Z")},
        .group_by = StatisticsGroupBy::Queue,
        .limit    = 17
    };
    auto system = system_statistics_request_to_json(StatisticsListRequest{query});
    REQUIRE(system);
    auto decoded = system_statistics_request_from_json(*system);
    REQUIRE(decoded);
    auto const& restored = std::get<StatisticsRequest>(*decoded);
    CHECK(restored.queue_id == queue);
    CHECK(restored.job_id == job);
    CHECK(restored.type == JobType::Http);
    CHECK(restored.origin == RunOrigin::Manual);
    CHECK(restored.group_by == StatisticsGroupBy::Queue);
    CHECK(restored.limit == 17);

    query.queue_id.reset();
    auto queue_request = queue_statistics_request_to_json(QueueStatisticsListRequest{
        QueueStatisticsQuery{.selector = std::string{"reports"}, .statistics = query}
    });
    REQUIRE(queue_request);
    CHECK(queue_request->as_object().at("queue_name").as_string() == "reports");
    auto queue_decoded = queue_statistics_request_from_json(*queue_request);
    REQUIRE(queue_decoded);
    CHECK(std::get<std::string>(std::get<QueueStatisticsQuery>(*queue_decoded).selector) == "reports");

    auto by_id = json(JsonValue::Object{
        {"queue_id", json(queue.to_string())}
    });
    REQUIRE(queue_statistics_request_from_json(by_id));
    std::get<JsonValue::Object>(by_id.data).emplace("queue_name", json(std::string{"reports"}));
    invalid_request(queue_statistics_request_from_json(by_id));

    auto continuation = json(JsonValue::Object{
        {"cursor", json(std::string{"opaque"})}
    });
    CHECK(std::get<CursorRequest>(*system_statistics_request_from_json(continuation)).cursor == "opaque");
    CHECK(std::get<CursorRequest>(*queue_statistics_request_from_json(continuation)).cursor == "opaque");
    std::get<JsonValue::Object>(continuation.data).emplace("queue_id", json(queue.to_string()));
    invalid_request(queue_statistics_request_from_json(continuation));
    invalid_request(system_statistics_request_from_json(continuation));

    invalid_request(system_statistics_request_from_json(json(JsonValue::Object{
        {"origin", json(std::string{"submitted"})}
    })));
    invalid_request(system_statistics_request_from_json(json(JsonValue::Object{
        {"limit", json(std::uint64_t{201})}
    })));
    invalid_request(queue_statistics_request_from_json(json(JsonValue::Object{
        {"queue_name", json(std::string{"reports"})},
        {"unexpected", json(true)                  }
    })));
}

TEST_CASE("Statistics page codec preserves counts, duration nulls, and provenance", "[jobu][statistics][json]")
{
    auto const queue             = id("00112233-4455-6677-8899-aabbccddeeff");
    auto       group             = StatisticsGroup{};
    group.key                    = queue;
    group.runs.total             = 2;
    group.runs.running           = 1;
    group.runs.succeeded         = 1;
    group.runs.cli               = 2;
    group.runs.scheduled_origin  = 2;
    group.runnable_wait_coverage = {.unmeasured = 1, .unfinished = 1};
    group.attempts.total         = 3;
    group.attempts.running       = 1;
    group.attempts.completed     = 2;
    group.attempts.retries       = 1;
    group.capture.lost_attempts  = 1;
    group.schedule_lateness_ms   = {.samples = 2, .average = 1.5, .maximum = 2.0};

    auto page = StatisticsPage{
        .window      = {.from = at("2026-01-01T00:00:00Z"), .to = at("2026-01-02T00:00:00Z")},
        .group_by    = StatisticsGroupBy::Queue,
        .groups      = {group},
        .next_cursor = "next"
    };
    auto encoded = statistics_page_to_json(page);
    REQUIRE(encoded);
    auto decoded = statistics_page_from_json(*encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->groups.size() == 1);
    CHECK(std::get<Uuid>(decoded->groups.front().key) == queue);
    CHECK(decoded->groups.front().runs.total == 2);
    CHECK(decoded->groups.front().attempts.retries == 1);
    CHECK(decoded->groups.front().schedule_lateness_ms.average == 1.5);
    CHECK_FALSE(decoded->groups.front().execution_wall_duration_ms.average);
    CHECK_FALSE(decoded->groups.front().runnable_wait_ms);
    CHECK(decoded->measurement.runnable_wait == "unavailable");

    auto future = *encoded;
    std::get<JsonValue::Object>(future.data).emplace("future", json(true));
    REQUIRE(statistics_page_from_json(future));
    auto& future_group = std::get<JsonValue::Object>(
        std::get<JsonValue::Array>(std::get<JsonValue::Object>(future.data).at("groups").data).front().data);
    future_group.emplace("future", json(true));
    REQUIRE(statistics_page_from_json(future));
    future_group["runnable_wait_ms"] = json(JsonValue::Object{});
    invalid_response(statistics_page_from_json(future));

    auto  invalid       = *encoded;
    auto& invalid_group = std::get<JsonValue::Object>(
        std::get<JsonValue::Array>(std::get<JsonValue::Object>(invalid.data).at("groups").data).front().data);
    invalid_group["key"] = json(std::string{"not-a-uuid"});
    invalid_response(statistics_page_from_json(invalid));
}

TEST_CASE("Statistics codec preserves measured wait and rejects inconsistent coverage", "[jobu][statistics][json]")
{
    auto group                   = StatisticsGroup{};
    group.runs.total             = 5;
    group.runs.scheduled         = 1;
    group.runs.succeeded         = 4;
    group.runnable_wait_ms       = StatisticsDuration{.samples = 2, .average = 0.75, .maximum = 1.5};
    group.runnable_wait_coverage = {.complete = 2, .partial = 1, .unmeasured = 1, .unfinished = 1};
    auto page                    = StatisticsPage{
        .window      = {.from = at("2026-01-01T00:00:00Z"), .to = at("2026-01-02T00:00:00Z")},
        .groups      = {group},
        .measurement = {.runnable_wait = "monotonic_observed"},
    };
    auto encoded = statistics_page_to_json(page);
    REQUIRE(encoded);
    auto decoded = statistics_page_from_json(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->measurement.runnable_wait == "monotonic_observed");
    REQUIRE(decoded->groups.front().runnable_wait_ms);
    CHECK(decoded->groups.front().runnable_wait_ms->average == 0.75);
    CHECK(decoded->groups.front().runnable_wait_coverage.complete == 2);
    CHECK(decoded->groups.front().runnable_wait_coverage.partial == 1);
    CHECK(decoded->groups.front().runnable_wait_coverage.unmeasured == 1);
    CHECK(decoded->groups.front().runnable_wait_coverage.unfinished == 1);

    auto with_group = [&](auto&& mutate) {
        auto  value  = *encoded;
        auto& fields = std::get<JsonValue::Object>(
            std::get<JsonValue::Array>(std::get<JsonValue::Object>(value.data).at("groups").data).front().data);
        mutate(fields);
        return statistics_page_from_json(value);
    };
    invalid_response(with_group([](auto& fields) { fields.erase("runnable_wait_coverage"); }));
    invalid_response(with_group([](auto& fields) { fields["runnable_wait_coverage"] = json(JsonNull{}); }));
    invalid_response(with_group(
        [](auto& fields) { std::get<JsonValue::Object>(fields.at("runnable_wait_coverage").data).erase("partial"); }));
    for (auto invalid :
         {json(std::int64_t{-1}), json(1.5), json(true), json(std::numeric_limits<std::uint64_t>::max())}) {
        invalid_response(with_group([&](auto& fields) {
            std::get<JsonValue::Object>(fields.at("runnable_wait_coverage").data)["partial"] = invalid;
        }));
    }
    invalid_response(with_group([](auto& fields) {
        auto& coverage         = std::get<JsonValue::Object>(fields.at("runnable_wait_coverage").data);
        coverage["unfinished"] = json(std::uint64_t{0});
        coverage["unmeasured"] = json(std::uint64_t{2});
    }));
    invalid_response(with_group([](auto& fields) {
        std::get<JsonValue::Object>(fields.at("runnable_wait_ms").data)["samples"] = json(std::uint64_t{3});
    }));
    REQUIRE(with_group([](auto& fields) {
        std::get<JsonValue::Object>(fields.at("runnable_wait_coverage").data)["future"] = json(true);
    }));

    page.groups.front().runnable_wait_coverage.partial = 2;
    invalid_response(statistics_page_to_json(page));
    page.groups.front().runnable_wait_coverage.partial = 1;
    page.groups.front().runnable_wait_ms->samples      = 3;
    invalid_response(statistics_page_to_json(page));
}
