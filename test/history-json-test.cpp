#include "history_json.hpp"

#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
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

TEST_CASE("Output request codec enforces channel, limit, offset, and strict fields", "[jobu][history][json]")
{
    auto const run     = id("10112233-4455-6677-8899-aabbccddeeff");
    auto       request = AttemptOutputRequest{
        .attempt = {.run_id = run, .attempt_number = 2},
        .channel = OutputChannel::Headers,
        .offset  = 7,
        .limit   = 65'536
    };
    auto encoded = attempt_output_request_to_json(request);
    REQUIRE(encoded);
    auto decoded = attempt_output_request_from_json(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->attempt.run_id == run);
    CHECK(decoded->attempt.attempt_number == 2);
    CHECK(decoded->channel == OutputChannel::Headers);
    CHECK(decoded->offset == 7);
    CHECK(decoded->limit == 65'536);

    auto defaults        = json(JsonValue::Object{
        {"run_id",         json(run.to_string())      },
        {"attempt_number", json(std::uint64_t{1})     },
        {"channel",        json(std::string{"stdout"})},
    });
    auto parsed_defaults = attempt_output_request_from_json(defaults);
    REQUIRE(parsed_defaults);
    CHECK(parsed_defaults->offset == 0);
    CHECK(parsed_defaults->limit == 16'384);

    auto changed                                         = defaults;
    std::get<JsonValue::Object>(changed.data)["channel"] = json(std::string{"bodyx"});
    invalid_request(attempt_output_request_from_json(changed));

    changed                                            = defaults;
    std::get<JsonValue::Object>(changed.data)["limit"] = json(std::uint64_t{65'537});
    invalid_request(attempt_output_request_from_json(changed));

    changed                                             = defaults;
    std::get<JsonValue::Object>(changed.data)["offset"] = json(std::numeric_limits<std::uint64_t>::max());
    invalid_request(attempt_output_request_from_json(changed));

    changed                                             = defaults;
    std::get<JsonValue::Object>(changed.data)["future"] = json(true);
    invalid_request(attempt_output_request_from_json(changed));
    invalid_request(attempt_output_request_to_json(AttemptOutputRequest{
        .attempt = {.run_id = run, .attempt_number = 0}
    }));
}

TEST_CASE("Output chunk codec preserves raw bytes and availability metadata", "[jobu][history][json]")
{
    auto const run   = id("10112233-4455-6677-8899-aabbccddeeff");
    auto       chunk = AttemptOutputChunk{
        .attempt        = {.run_id = run,   .attempt_number = 2},
        .channel        = OutputChannel::Body,
        .status         = OutputStatus::Lost,
        .offset         = 1,
        .bytes_returned = 2,
        .next_offset    = 3,
        .retained_bytes = 4,
        .total_bytes    = 9,
        .omitted_bytes  = 5,
        .truncated      = true,
        .capture_lost   = true,
        .encoding       = OutputEncoding::Base64,
        .data           = {std::byte{0xff}, std::byte{0x00}    },
    };
    auto encoded = attempt_output_chunk_to_json(chunk);
    REQUIRE(encoded);
    CHECK(encoded->as_object().at("encoding").as_string() == "base64");
    CHECK(encoded->as_object().at("data").as_string() == "/wA=");
    auto decoded = attempt_output_chunk_from_json(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->data == chunk.data);
    CHECK(decoded->status == OutputStatus::Lost);
    CHECK(decoded->total_bytes == 9);
    CHECK(decoded->omitted_bytes == 5);
    CHECK(decoded->next_offset == 3);

    auto future = *encoded;
    std::get<JsonValue::Object>(future.data).emplace("future", json(true));
    CHECK(attempt_output_chunk_from_json(future));
    std::get<JsonValue::Object>(future.data)["data"] = json(std::string{"/wB="});
    invalid_response(attempt_output_chunk_from_json(future));
    future                                                     = *encoded;
    std::get<JsonValue::Object>(future.data)["bytes_returned"] = json(std::uint64_t{3});
    invalid_response(attempt_output_chunk_from_json(future));
    future                                                  = *encoded;
    std::get<JsonValue::Object>(future.data)["next_offset"] = json(std::uint64_t{4});
    invalid_response(attempt_output_chunk_from_json(future));

    chunk.status         = OutputStatus::Available;
    chunk.capture_lost   = false;
    chunk.encoding       = OutputEncoding::Utf8;
    chunk.data           = {std::byte{'A'}, std::byte{0xc3}, std::byte{0xa9}};
    chunk.bytes_returned = 3;
    chunk.offset         = 0;
    chunk.next_offset    = 3;
    auto utf8            = attempt_output_chunk_to_json(chunk);
    REQUIRE(utf8);
    CHECK(utf8->as_object().at("data").as_string() == "A\xc3\xa9");
    auto decoded_utf8 = attempt_output_chunk_from_json(*utf8);
    REQUIRE(decoded_utf8);
    CHECK(decoded_utf8->data == chunk.data);
}

TEST_CASE("History get and page codecs preserve detail boundaries", "[jobu][history][json]")
{
    auto const run_id   = id("00112233-4455-6677-8899-aabbccddeeff");
    auto const job_id   = id("10112233-4455-6677-8899-aabbccddeeff");
    auto const queue_id = id("20112233-4455-6677-8899-aabbccddeeff");
    auto const key      = AttemptKey{.run_id = run_id, .attempt_number = 3};

    auto run_request     = run_get_request_to_json(run_id);
    auto attempt_request = attempt_get_request_to_json(key);
    REQUIRE(run_request);
    REQUIRE(attempt_request);
    CHECK(run_get_request_from_json(*run_request).value() == run_id);
    CHECK(attempt_get_request_from_json(*attempt_request)->attempt_number == 3);
    std::get<JsonValue::Object>(run_request->data).emplace("extra", json(true));
    invalid_request(run_get_request_from_json(*run_request));
    std::get<JsonValue::Object>(attempt_request->data)["attempt_number"] = json(std::uint64_t{0});
    invalid_request(attempt_get_request_from_json(*attempt_request));

    auto run          = RunSummary{.id          = run_id,
                                   .job_id      = job_id,
                                   .queue_id    = queue_id,
                                   .planned_at  = at("2026-01-01T00:00:00Z"),
                                   .runnable_at = at("2026-01-01T00:00:00Z")};
    auto page         = RunPage{.items = {run}, .next_cursor = "cursor"};
    auto encoded_page = run_page_to_json(page);
    REQUIRE(encoded_page);
    auto decoded_page = run_page_from_json(*encoded_page);
    REQUIRE(decoded_page);
    CHECK(decoded_page->items.front().id == run_id);
    CHECK(decoded_page->next_cursor == "cursor");
    auto future_page = *encoded_page;
    std::get<JsonValue::Object>(future_page.data).emplace("future", json(true));
    REQUIRE(run_page_from_json(future_page));
    std::get<JsonValue::Object>(future_page.data).erase("items");
    invalid_response(run_page_from_json(future_page));

    auto attempt           = AttemptDetails{};
    attempt.run_id         = run_id;
    attempt.attempt_number = 3;
    attempt.due_at         = run.planned_at;
    attempt.state          = AttemptState::Completed;
    attempt.outcome        = AttemptOutcome::Succeeded;
    attempt.result         = json(JsonValue::Object{
        {"exit_code", json(std::uint64_t{0})}
    });
    auto encoded_detail    = attempt_details_to_json(attempt);
    REQUIRE(encoded_detail);
    CHECK(attempt_details_from_json(*encoded_detail)->result == attempt.result);
    auto attempt_page = attempt_page_to_json(AttemptPage{.items = {attempt}});
    REQUIRE(attempt_page);
    CHECK_FALSE(attempt_page->as_object().at("items").as_array().front().as_object().contains("result"));
    CHECK(attempt_page_from_json(*attempt_page)->items.front().attempt_number == 3);
    std::get<JsonValue::Object>(encoded_detail->data).erase("result");
    invalid_response(attempt_details_from_json(*encoded_detail));
    invalid_response(run_page_from_json(json(JsonValue::Object{
        {"items",       json(JsonValue::Array{})      },
        {"next_cursor", json(std::string{"repeating"})}
    })));
}

TEST_CASE("History attempt identities stay within the durable number domain", "[jobu][history][json]")
{
    auto const run     = id("10112233-4455-6677-8899-aabbccddeeff");
    auto const due     = at("2026-01-01T00:00:00Z");
    auto       summary = AttemptSummary{.run_id = run, .attempt_number = 1, .due_at = due};
    auto       output  = AttemptOutputChunk{
        .attempt = {.run_id = run, .attempt_number = 1}
    };
    auto const valid_numbers = {AttemptNumber{1}, maximum_attempt_number};

    for (auto number : valid_numbers) {
        CHECK(is_valid_attempt_number(number));
        auto key = AttemptKey{.run_id = run, .attempt_number = number};
        auto get = attempt_get_request_to_json(key);
        REQUIRE(get);
        CHECK(attempt_get_request_from_json(*get)->attempt_number == number);

        auto request         = AttemptOutputRequest{.attempt = key};
        auto encoded_request = attempt_output_request_to_json(request);
        REQUIRE(encoded_request);
        CHECK(attempt_output_request_from_json(*encoded_request)->attempt.attempt_number == number);

        summary.attempt_number = number;
        auto encoded_summary   = attempt_summary_to_json(summary);
        REQUIRE(encoded_summary);
        CHECK(attempt_summary_from_json(*encoded_summary)->attempt_number == number);
        auto detail                          = AttemptDetails{};
        static_cast<AttemptSummary&>(detail) = summary;
        auto encoded_detail                  = attempt_details_to_json(detail);
        REQUIRE(encoded_detail);
        CHECK(attempt_details_from_json(*encoded_detail)->attempt_number == number);
        auto page = attempt_page_to_json(AttemptPage{.items = {summary}});
        REQUIRE(page);
        CHECK(attempt_page_from_json(*page)->items.front().attempt_number == number);

        output.attempt.attempt_number = number;
        auto encoded_output           = attempt_output_chunk_to_json(output);
        REQUIRE(encoded_output);
        CHECK(attempt_output_chunk_from_json(*encoded_output)->attempt.attempt_number == number);
    }

    for (auto number : {AttemptNumber{0}, maximum_attempt_number + 1, std::numeric_limits<AttemptNumber>::max()}) {
        CHECK_FALSE(is_valid_attempt_number(number));
        auto key = AttemptKey{.run_id = run, .attempt_number = number};
        invalid_request(attempt_get_request_to_json(key));
        invalid_request(attempt_output_request_to_json(AttemptOutputRequest{.attempt = key}));

        summary.attempt_number = number;
        invalid_response(attempt_summary_to_json(summary));
        auto detail                          = AttemptDetails{};
        static_cast<AttemptSummary&>(detail) = summary;
        invalid_response(attempt_details_to_json(detail));
        invalid_response(attempt_page_to_json(AttemptPage{.items = {summary}}));

        output.attempt.attempt_number = number;
        invalid_response(attempt_output_chunk_to_json(output));
    }

    // Mutate valid wire objects so each decoder's boundary is exercised independently.
    summary.attempt_number               = 1;
    output.attempt.attempt_number        = 1;
    auto encoded_summary                 = attempt_summary_to_json(summary).value();
    auto detail                          = AttemptDetails{};
    static_cast<AttemptSummary&>(detail) = summary;
    auto encoded_detail                  = attempt_details_to_json(detail).value();
    auto encoded_page                    = attempt_page_to_json(AttemptPage{.items = {summary}}).value();
    auto encoded_output                  = attempt_output_chunk_to_json(output).value();

    for (auto value : {json(std::uint64_t{0}),
                       json(maximum_attempt_number + 1),
                       json(std::numeric_limits<AttemptNumber>::max()),
                       json(std::int64_t{-1}),
                       json(1.5),
                       json(true),
                       json(JsonNull{}),
                       json(std::string{"1"})}) {
        auto get = json(JsonValue::Object{
            {"run_id",         json(run.to_string())},
            {"attempt_number", value                }
        });
        invalid_request(attempt_get_request_from_json(get));

        auto request = json(JsonValue::Object{
            {"run_id",         json(run.to_string())      },
            {"attempt_number", value                      },
            {"channel",        json(std::string{"stdout"})}
        });
        invalid_request(attempt_output_request_from_json(request));

        auto changed_summary                                                = encoded_summary;
        std::get<JsonValue::Object>(changed_summary.data)["attempt_number"] = value;
        invalid_response(attempt_summary_from_json(changed_summary));
        auto changed_detail                                                = encoded_detail;
        std::get<JsonValue::Object>(changed_detail.data)["attempt_number"] = value;
        invalid_response(attempt_details_from_json(changed_detail));
        auto  changed_page = encoded_page;
        auto& page_items   = std::get<JsonValue::Array>(std::get<JsonValue::Object>(changed_page.data)["items"].data);
        std::get<JsonValue::Object>(page_items.front().data)["attempt_number"] = value;
        invalid_response(attempt_page_from_json(changed_page));
        auto changed_output                                                = encoded_output;
        std::get<JsonValue::Object>(changed_output.data)["attempt_number"] = value;
        invalid_response(attempt_output_chunk_from_json(changed_output));
    }
}
