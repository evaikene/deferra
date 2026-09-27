#include "attribute_registry.hpp"
#include "control_json.hpp"
#include "history_json.hpp"
#include "json.hpp"
#include "management_json.hpp"
#include "secret_json.hpp"
#include "statistics_json.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <variant>

using namespace jb::core;
using namespace jb::jobu;

namespace {

// Read the checked examples from docs so a documentation edit must still satisfy
// the same decoders used by the daemon and typed client.
auto example(std::string_view filename) -> JsonValue
{
    auto const path  = std::filesystem::path{JOBU_DOC_EXAMPLE_DIR} / std::string{filename};
    auto       input = std::ifstream{path};
    REQUIRE(input.is_open());

    auto const source = std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto       parsed = parse_json(source);
    REQUIRE(parsed);
    return *parsed;
}

} // namespace

TEST_CASE("Published protocol requests use production decoders", "[jobu][protocol][docs]")
{
    auto const attributes = StandardAttributeRegistry{};

    REQUIRE(create_queue_request_from_json(example("queue-create.params.json"), attributes));
    REQUIRE(create_job_request_from_json(example("job-create.params.json"), attributes));
    REQUIRE(create_job_request_from_json(example("job-create-secret.params.json"), attributes));
    REQUIRE(job_list_request_from_json(example("job-list.params.json")));
    REQUIRE(job_list_request_from_json(example("job-list-all.params.json")));
    REQUIRE(run_list_request_from_json(example("run-list.params.json")));
    REQUIRE(attempt_get_request_from_json(example("attempt-get-max.params.json")));
    REQUIRE(attempt_output_request_from_json(example("attempt-output.params.json")));
    REQUIRE(set_secret_request_from_json(example("secret-set.params.json")));
    REQUIRE(schedule_next_request_from_json(example("schedule-next.params.json")));
    REQUIRE(system_statistics_request_from_json(example("system-stats.params.json")));
}

TEST_CASE("Published protocol results use production decoders", "[jobu][protocol][docs]")
{
    auto const attributes = StandardAttributeRegistry{};

    REQUIRE(queue_page_from_json(example("queue-list.result.json"), attributes));
    REQUIRE(job_page_from_json(example("job-list.result.json"), attributes));
    REQUIRE(run_page_from_json(example("run-list.result.json")));
    REQUIRE(attempt_page_from_json(example("attempt-list.result.json")));
    REQUIRE(secret_metadata_from_json(example("secret-set.result.json")));
    REQUIRE(schedule_next_result_from_json(example("schedule-next.result.json")));
    REQUIRE(statistics_page_from_json(example("system-stats.result.json")));
}

TEST_CASE("Published closure examples preserve terminal and raw request semantics", "[jobu][protocol][docs]")
{
    auto const attributes = StandardAttributeRegistry{};

    auto const filtered = job_list_request_from_json(example("job-list.params.json"));
    REQUIRE(filtered);
    CHECK(filtered->state == JobState::Succeeded);

    auto const unfiltered = job_list_request_from_json(example("job-list-all.params.json"));
    REQUIRE(unfiltered);
    CHECK_FALSE(unfiltered->state);

    auto const finished = job_page_from_json(example("job-list.result.json"), attributes);
    REQUIRE(finished);
    REQUIRE(finished->items.size() == 1U);
    CHECK(finished->items.front().state == JobState::Succeeded);
    CHECK(std::holds_alternative<OnceSchedule>(finished->items.front().schedule));
    CHECK_FALSE(finished->items.front().deleted_at);

    auto const boundary = attempt_get_request_from_json(example("attempt-get-max.params.json"));
    REQUIRE(boundary);
    CHECK(boundary->attempt_number == maximum_attempt_number);

    auto const secret_job = create_job_request_from_json(example("job-create-secret.params.json"), attributes);
    REQUIRE(secret_job);
    auto const& payload     = secret_job->payload.as_object();
    auto const& arguments   = payload.at("arguments").as_array();
    auto const& environment = payload.at("environment").as_object();
    CHECK(arguments.at(1).as_object().at("secret").as_string() == "reports.token");
    CHECK(environment.at("REPORT_PASSWORD").as_object().at("secret").as_string() == "reports.password");
}

TEST_CASE("Published statistics request and result describe one empty cohort", "[jobu][protocol][docs]")
{
    auto const request = system_statistics_request_from_json(example("system-stats.params.json"));
    REQUIRE(request);

    auto const* query = std::get_if<StatisticsRequest>(&*request);
    REQUIRE(query != nullptr);

    auto const result = statistics_page_from_json(example("system-stats.result.json"));
    REQUIRE(result);

    CHECK(result->group_by == query->group_by);
    CHECK(result->window.from == query->planned.from);
    CHECK(result->window.to == query->planned.to);
    CHECK(result->groups.size() <= query->limit);

    REQUIRE(result->group_by == StatisticsGroupBy::None);
    REQUIRE(result->groups.size() == 1);
    CHECK(std::holds_alternative<std::monostate>(result->groups.front().key));
    CHECK(result->groups.front().runs.total == 0);
    CHECK(result->groups.front().attempts.total == 0);
    CHECK_FALSE(result->next_cursor);
}
