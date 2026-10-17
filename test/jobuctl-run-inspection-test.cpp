#include "run_inspection_priv.hpp"

#include "attempt.hpp"
#include "command_helpers_priv.hpp"
#include "command_line_priv.hpp"
#include "commands/commands_priv.hpp"
#include "history.hpp"
#include "job.hpp"
#include "json.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations
#include "utc_timestamp.hpp"
#include "uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <csignal> // IWYU pragma: keep for SIGTERM and the platform NSIG macro
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobuctl::detail;

namespace {

auto json(auto value) -> JsonValue
{
    return {.data = std::move(value)};
}

auto field_result(std::string field, JsonValue value) -> JsonValue
{
    return json(JsonValue::Object{
        {std::move(field), std::move(value)}
    });
}

void check_empty(AttemptDiagnostics const& diagnostics)
{
    CHECK_FALSE(diagnostics.exit_code);
    CHECK_FALSE(diagnostics.signal_number);
    CHECK_FALSE(diagnostics.http_status);
    CHECK_FALSE(diagnostics.error_code);
    CHECK_FALSE(diagnostics.error_category);
    CHECK_FALSE(diagnostics.runner_outcome);
}

auto sample_attempt(AttemptNumber number = 1, AttemptState state = AttemptState::Completed) -> AttemptDetails
{
    auto id = Uuid::parse("00112233-4455-6677-8899-aabbccddeeff");
    auto at = parse_utc_timestamp("2026-10-17T09:00:00.123456Z");
    REQUIRE(id);
    REQUIRE(at);

    auto attempt           = AttemptDetails{};
    attempt.run_id         = *id;
    attempt.attempt_number = number;
    attempt.due_at         = *at;
    attempt.state          = state;
    if (state != AttemptState::Pending) {
        attempt.started_at = *at;
    }
    if (state == AttemptState::Completed) {
        attempt.completed_at = *at;
        attempt.outcome      = AttemptOutcome::Failed;
    }
    return attempt;
}

} // namespace

TEST_CASE("attempt diagnostics accept exact bounded integers for the trusted runner", "[jobuctl][inspection]")
{
    for (auto number : {std::uint64_t{0}, std::uint64_t{255}}) {
        for (auto const& value : {json(number), json(static_cast<std::int64_t>(number))}) {
            auto diagnostics = project_attempt_diagnostics(JobType::Cli, field_result("exit_code", value));
            CHECK(diagnostics.exit_code == number);
            CHECK_FALSE(diagnostics.http_status);
            check_empty(project_attempt_diagnostics(JobType::Http, field_result("exit_code", value)));
        }
    }
    for (auto number : {std::uint64_t{1}, static_cast<std::uint64_t>(NSIG - 1)}) {
        for (auto const& value : {json(number), json(static_cast<std::int64_t>(number))}) {
            CHECK(project_attempt_diagnostics(JobType::Cli, field_result("signal", value)).signal_number == number);
        }
    }
    for (auto number : {std::uint64_t{100}, std::uint64_t{599}}) {
        for (auto const& value : {json(number), json(static_cast<std::int64_t>(number))}) {
            CHECK(project_attempt_diagnostics(JobType::Http, field_result("status", value)).http_status == number);
            check_empty(project_attempt_diagnostics(JobType::Cli, field_result("status", value)));
        }
    }

    // A result's type token cannot redirect the projection away from the stored run type.
    auto mixed = json(JsonValue::Object{
        {"type",      json(std::string{"http"})},
        {"exit_code", json(std::uint64_t{9})   },
        {"status",    json(std::uint64_t{503}) }
    });
    auto cli   = project_attempt_diagnostics(JobType::Cli, mixed);
    CHECK(cli.exit_code == 9);
    CHECK_FALSE(cli.http_status);
    auto http = project_attempt_diagnostics(JobType::Http, mixed);
    CHECK(http.http_status == 503);
    CHECK_FALSE(http.exit_code);
    check_empty(project_attempt_diagnostics(static_cast<JobType>(255), mixed));
}

TEST_CASE("attempt diagnostics omit malformed numeric observations", "[jobuctl][inspection]")
{
    auto malformed = std::vector<JsonValue>{JsonValue{},
                                            json(true),
                                            json(1.0),
                                            json(std::string{"1"}),
                                            json(JsonValue::Array{}),
                                            json(JsonValue::Object{}),
                                            json(std::int64_t{-1}),
                                            json(std::numeric_limits<std::int64_t>::min()),
                                            json(std::numeric_limits<std::uint64_t>::max())};
    for (auto const& value : malformed) {
        check_empty(project_attempt_diagnostics(JobType::Cli, field_result("exit_code", value)));
        check_empty(project_attempt_diagnostics(JobType::Cli, field_result("signal", value)));
        check_empty(project_attempt_diagnostics(JobType::Http, field_result("status", value)));
    }
    for (auto number : {std::uint64_t{256}, std::uint64_t{65536}}) {
        CHECK_FALSE(project_attempt_diagnostics(JobType::Cli, field_result("exit_code", json(number))).exit_code);
    }
    for (auto number : {std::uint64_t{0}, static_cast<std::uint64_t>(NSIG)}) {
        CHECK_FALSE(project_attempt_diagnostics(JobType::Cli, field_result("signal", json(number))).signal_number);
    }
    for (auto number : {std::uint64_t{0}, std::uint64_t{99}, std::uint64_t{600}}) {
        CHECK_FALSE(project_attempt_diagnostics(JobType::Http, field_result("status", json(number))).http_status);
    }
    for (auto const& value : malformed) {
        check_empty(project_attempt_diagnostics(JobType::Cli, value));
        check_empty(project_attempt_diagnostics(JobType::Http, value));
    }
}

TEST_CASE("attempt diagnostics recognize current runner tokens without accepting arbitrary text",
          "[jobuctl][inspection]")
{
    for (auto const* token :
         {"success", "unexpected_exit", "signal", "timeout", "cancelled", "start_failure", "internal_error"}) {
        CHECK(project_attempt_diagnostics(JobType::Cli, field_result("outcome", json(std::string{token})))
                  .runner_outcome == token);
    }
    for (auto const* token :
         {"expected_status", "unexpected_status", "transport_error", "cancelled", "internal_error"}) {
        CHECK(project_attempt_diagnostics(JobType::Http, field_result("outcome", json(std::string{token})))
                  .runner_outcome == token);
    }
    for (auto const* token : {"invalid_request",
                              "resolve",
                              "connect",
                              "tls_verification",
                              "tls_handshake",
                              "timeout",
                              "send",
                              "receive",
                              "redirect",
                              "protocol",
                              "cancelled",
                              "internal"}) {
        CHECK(project_attempt_diagnostics(JobType::Http, field_result("error_category", json(std::string{token})))
                  .error_category == token);
    }
    check_empty(project_attempt_diagnostics(JobType::Http, field_result("outcome", json(std::string{"success"}))));
    check_empty(
        project_attempt_diagnostics(JobType::Cli, field_result("outcome", json(std::string{"expected_status"}))));
    check_empty(
        project_attempt_diagnostics(JobType::Cli, field_result("error_category", json(std::string{"connect"}))));

    auto result      = json(JsonValue::Object{
        {"outcome",        json(std::string{"transport_error"})         },
        {"error_category", json(std::string{"connect"})                 },
        {"error_code",     json(std::string{"core.http.connect_failed"})}
    });
    auto diagnostics = project_attempt_diagnostics(JobType::Http, result);
    result           = JsonValue{};
    CHECK(diagnostics.runner_outcome == "transport_error");
    CHECK(diagnostics.error_category == "connect");
    CHECK(diagnostics.error_code == "core.http.connect_failed");
}

TEST_CASE("diagnostic strings are bounded identifiers or exact known tokens", "[jobuctl][inspection]")
{
    for (auto type : {JobType::Cli, JobType::Http}) {
        for (auto const& code :
             {std::string{"core.process.exec_failed"}, std::string{"Driver.Error-2"}, std::string(128, 'a')}) {
            CHECK(project_attempt_diagnostics(type, field_result("error_code", json(code))).error_code == code);
        }

        auto unsafe_strings = std::vector<std::string>{
            "",
            "1error",
            ".error",
            "core error",
            "core.error:secret",
            "https://user:password@example.test",
            "core.error\nsecret",
            "core.error\rsecret",
            "core.error\tsecret",
            "core.error\x1b[31m",
            "core.error\x7f",
            "core.error\xc2\x9b",
            "core.error\xc3\xa4",
            std::string{"core.error\0secret", 17},
            std::string(129, 'a'),
            std::string{"timeout"}
            + std::string(4096, 'x')
        };
        for (auto const& text : unsafe_strings) {
            check_empty(project_attempt_diagnostics(type, field_result("error_code", json(text))));
            check_empty(project_attempt_diagnostics(type, field_result("error_category", json(text))));
            check_empty(project_attempt_diagnostics(type, field_result("outcome", json(text))));
        }
        for (auto const* field : {"error_code", "error_category", "outcome"}) {
            for (auto const& value : {JsonValue{},
                                      json(false),
                                      json(std::uint64_t{7}),
                                      json(JsonValue::Array{}),
                                      json(JsonValue::Object{})}) {
                check_empty(project_attempt_diagnostics(type, field_result(field, value)));
            }
        }
        for (auto const* field : {"error_category", "outcome"}) {
            check_empty(
                project_attempt_diagnostics(type, field_result(field, json(std::string{"unknown_identifier"}))));
            check_empty(project_attempt_diagnostics(type, field_result(field, json(std::string{"TIMEOUT"}))));
        }
    }
}

TEST_CASE("last attempt presentation distinguishes durable state from runner diagnostics", "[jobuctl][inspection]")
{
    for (auto type : {JobType::Cli, JobType::Http}) {
        for (auto state : {AttemptState::Pending, AttemptState::Running}) {
            auto attempt   = sample_attempt(3, state);
            // A contradictory optional result must not make an incomplete attempt appear terminal.
            attempt.result = json(JsonValue::Object{
                {"outcome",   json(std::string{"success"})},
                {"exit_code", json(std::uint64_t{0})      },
                {"status",    json(std::uint64_t{200})    }
            });
            auto text      = format_last_attempt(type, attempt);
            REQUIRE(text);
            CHECK(text->find(state == AttemptState::Pending ? "state=pending" : "state=running") != std::string::npos);
            CHECK(text->find("outcome=none") != std::string::npos);
            CHECK(text->find("completed_at=none") != std::string::npos);
            CHECK(text->find("runner_outcome") == std::string::npos);
            CHECK(text->find("exit_code") == std::string::npos);
            CHECK(text->find("status=") == std::string::npos);
            if (state == AttemptState::Pending) {
                CHECK(text->find("started_at=none") != std::string::npos);
            }
            else {
                CHECK(text->find("started_at=2026-10-17T09:00:00.123456Z") != std::string::npos);
            }
        }
    }

    auto attempt   = sample_attempt(7);
    attempt.result = json(JsonValue::Object{
        {"outcome",   json(std::string{"success"})},
        {"exit_code", json(std::uint64_t{0})      }
    });
    auto text      = format_last_attempt(JobType::Cli, attempt);
    REQUIRE(text);
    CHECK(text->find("Last attempt: 7: state=completed, outcome=failed") == 0);
    CHECK(text->find("completed_at=2026-10-17T09:00:00.123456Z") != std::string::npos);
    CHECK(text->find("exit_code=0") != std::string::npos);
    CHECK(text->find("runner_outcome=success") != std::string::npos);

    attempt.outcome.reset();
    auto no_outcome = format_last_attempt(static_cast<AttemptSummary const&>(attempt));
    REQUIRE(no_outcome);
    CHECK(no_outcome->find("state=completed, outcome=none") != std::string::npos);
    CHECK(no_outcome->find("runner_outcome") == std::string::npos);
    attempt.result.reset();
    CHECK(format_last_attempt(JobType::Cli, attempt) == no_outcome);
}

TEST_CASE("last attempt presentation shows failures without execution content", "[jobuctl][inspection]")
{
    auto content = json(JsonValue::Object{
        {"message", json(std::string{"sensitive-content\x1b[31m"})},
        {"exit_code", json(std::uint64_t{99})},
        {"data", json(std::string(4096, 'x'))}
    });
    for (auto type : {JobType::Cli, JobType::Http}) {
        auto attempt   = sample_attempt();
        attempt.result = json(JsonValue::Object{
            {"exit_code",      json(std::uint64_t{17})                                },
            {"signal",         json(std::uint64_t{SIGTERM})                           },
            {"status",         json(std::uint64_t{503})                               },
            {"error_code",     json(std::string{"core.http.connect_failed"})          },
            {"error_category", json(std::string{"connect"})                           },
            {"outcome",        json(std::string{"transport_error"})                   },
            {"message",        content                                                },
            {"detail",         content                                                },
            {"stdout",         content                                                },
            {"stderr",         content                                                },
            {"body",           content                                                },
            {"headers",        content                                                },
            {"payload",        content                                                },
            {"environment",    content                                                },
            {"url",            json(std::string{"https://user:password@example.test"})}
        });
        auto text      = format_last_attempt(type, attempt);
        REQUIRE(text);
        CHECK(text->size() < 1024);
        CHECK(text->find("sensitive-content") == std::string::npos);
        CHECK(text->find(std::string(128, 'x')) == std::string::npos);
        CHECK(text->find("password") == std::string::npos);
        CHECK(text->find("exit_code=99") == std::string::npos);
        CHECK(text->find('\x1b') == std::string::npos);
        if (type == JobType::Cli) {
            CHECK(text->find("exit_code=17") != std::string::npos);
            CHECK(text->find("signal=" + std::to_string(SIGTERM)) != std::string::npos);
            CHECK(text->find("status=") == std::string::npos);
        }
        else {
            CHECK(text->find("status=503") != std::string::npos);
            CHECK(text->find("error_category=connect") != std::string::npos);
            CHECK(text->find("runner_outcome=transport_error") != std::string::npos);
            CHECK(text->find("exit_code=") == std::string::npos);
            CHECK(text->find("signal=") == std::string::npos);
        }

        auto summary = format_last_attempt(static_cast<AttemptSummary const&>(attempt));
        REQUIRE(summary);
        CHECK(summary->find("state=completed, outcome=failed") != std::string::npos);
        CHECK(summary->find("error_code") == std::string::npos);

        attempt.result = json(std::string{"sensitive-content"});
        CHECK(format_last_attempt(type, attempt) == summary);
    }
}

TEST_CASE("last attempt presentation rejects invalid typed summaries and escapes controls", "[jobuctl][inspection]")
{
    auto attempt  = sample_attempt();
    attempt.state = static_cast<AttemptState>(255);
    CHECK_FALSE(format_last_attempt(attempt));
    attempt         = sample_attempt();
    attempt.outcome = static_cast<AttemptOutcome>(255);
    CHECK_FALSE(format_last_attempt(attempt));
    attempt = sample_attempt(0);
    CHECK_FALSE(format_last_attempt(attempt));

    CHECK(escape_human("\x1b\n\r\t\x7f\xc2\x9b") == "\\x1B\\n\\r\\t\\x7F\\u009B");
    auto unsafe   = sample_attempt();
    unsafe.result = json(JsonValue::Object{
        {"error_code", json(std::string{"core.error\x1b[31m"})     },
        {"outcome",    json(std::string{"success\nforged outcome"})}
    });
    CHECK(format_last_attempt(JobType::Cli, unsafe) == format_last_attempt(static_cast<AttemptSummary const&>(unsafe)));
}

TEST_CASE("latest completed selection follows durable state and newest-first page order", "[jobuctl][inspection]")
{
    auto page = AttemptPage{};
    CHECK_FALSE(latest_completed_attempt(page.items));
    page.items       = {sample_attempt(8, AttemptState::Pending), sample_attempt(7, AttemptState::Running)};
    page.next_cursor = "another-page";
    CHECK_FALSE(latest_completed_attempt(page.items));

    // Several incomplete attempts may precede retained completed attempts. Neither outcome nor clocks sort them.
    auto completed = sample_attempt(6);
    completed.outcome.reset();
    page.items.push_back(completed);
    page.items.push_back(sample_attempt(4));
    auto selected = latest_completed_attempt(page.items);
    REQUIRE(selected);
    CHECK(selected->attempt_number == 6);
    CHECK(selected->state == AttemptState::Completed);
    CHECK_FALSE(selected->outcome);
    auto const id = selected->run_id;
    page.items.clear();
    CHECK(selected->run_id == id);
    CHECK(selected->attempt_number == 6);

    page.items = {sample_attempt(9), sample_attempt(8)};
    CHECK(latest_completed_attempt(page.items)->attempt_number == 9);
}

TEST_CASE("run output hints and predicates identify the existing output contracts", "[jobuctl][inspection]")
{
    auto const id = sample_attempt().run_id;
    for (auto type : {JobType::Cli, JobType::Http}) {
        auto text = run_output_hint(id, type);
        CHECK(text.find("latest completed attempt") != std::string::npos);
        CHECK(text.find("older than a running retry") != std::string::npos);
        CHECK(text.find("jobuctl run output " + id.to_string()) != std::string::npos);
        CHECK(text.find(type == JobType::Cli ? "--channel stderr" : "--channel body") != std::string::npos);
    }
    CHECK(run_output_hint(id, static_cast<JobType>(255)).empty());
    CHECK(is_output_command(CommandKind::AttemptOutput));
    for (auto kind : {CommandKind::AttemptGet,
                      CommandKind::AttemptList,
                      CommandKind::RunGet,
                      CommandKind::RunList,
                      CommandKind::RunCancel,
                      CommandKind::JobUpdate}) {
        CHECK_FALSE(is_output_command(kind));
    }
    for (auto channel : {OutputChannel::Stdout, OutputChannel::Stderr, OutputChannel::Body, OutputChannel::Headers}) {
        CHECK(output_channel_matches(JobType::Cli, channel) ==
              (channel == OutputChannel::Stdout || channel == OutputChannel::Stderr));
        CHECK(output_channel_matches(JobType::Http, channel) ==
              (channel == OutputChannel::Body || channel == OutputChannel::Headers));
        CHECK_FALSE(output_channel_matches(static_cast<JobType>(255), channel));
    }
    CHECK_FALSE(output_channel_matches(JobType::Cli, static_cast<OutputChannel>(255)));
    CHECK_FALSE(output_channel_matches(JobType::Http, static_cast<OutputChannel>(255)));
}
