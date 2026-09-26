#include "cli_exit_policy_priv.hpp"

#include "byte_buffer.hpp"
#include "json.hpp"
#include "process.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations

#include <catch2/catch_test_macros.hpp>

#include <bitset>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;

namespace {

auto selector_list(std::initializer_list<std::string_view> selectors) -> AttributeValue::List
{
    auto result = AttributeValue::List{};
    result.reserve(selectors.size());
    for (auto const selector : selectors) {
        result.push_back({.data = std::string{selector}});
    }
    return result;
}

auto retry_policy(std::initializer_list<std::string_view> selectors) -> CliExitCodeSet
{
    auto decoded = decode_cli_retry_exit_codes(selector_list(selectors));
    REQUIRE(decoded);
    return std::move(*decoded);
}

auto output_channel(std::string_view bytes, std::uint64_t total_bytes) -> AttemptOutputChannel
{
    auto const view = as_bytes(bytes);
    return {
        .bytes       = ByteBuffer{view.begin(), view.end()},
        .total_bytes = total_bytes,
        .truncated   = total_bytes > bytes.size(),
    };
}

auto capture_snapshot(bool capture_lost = false) -> CliCaptureSnapshot
{
    return {
        .standard_output = output_channel("abc", 5U),
        .standard_error  = output_channel("xy", 2U),
        .capture_lost    = capture_lost,
    };
}

auto result_outcome(CliCompletionPolicy const& policy) -> std::string const&
{
    return policy.result.as_object().at("outcome").as_string();
}

} // anonymous namespace

TEST_CASE("CLI retry exit selectors normalize into one membership policy", "[jobu][cli][exit-policy]")
{
    auto decoded = decode_cli_retry_exit_codes(selector_list({"255", "7-9", "0", "2-4", "6", "5"}));
    REQUIRE(decoded);

    auto const& ranges = decoded->ranges();
    REQUIRE(ranges.size() == 3U);
    CHECK(ranges[0] == CliExitCodeRange{.first = 0, .last = 0});
    CHECK(ranges[1] == CliExitCodeRange{.first = 2, .last = 9});
    CHECK(ranges[2] == CliExitCodeRange{.first = 255, .last = 255});

    CHECK(decoded->contains(0));
    CHECK_FALSE(decoded->contains(1));
    CHECK(decoded->contains(2));
    CHECK(decoded->contains(9));
    CHECK_FALSE(decoded->contains(10));
    CHECK(decoded->contains(255));
    CHECK_FALSE(decoded->contains(-1));
    CHECK_FALSE(decoded->contains(256));
}

TEST_CASE("CLI retry exit selectors accept an empty terminal policy", "[jobu][cli][exit-policy]")
{
    auto decoded = decode_cli_retry_exit_codes({});
    REQUIRE(decoded);
    CHECK(decoded->ranges().empty());
    CHECK_FALSE(decoded->contains(0));
    CHECK_FALSE(decoded->contains(255));
}

TEST_CASE("CLI retry exit selectors require canonical bounded syntax", "[jobu][cli][exit-policy]")
{
    for (auto const* const selector : {
             "",
             "00",
             "01",
             "+1",
             "-1",
             " 1",
             "1 ",
             "256",
             "1-",
             "-",
             "1--2",
             "1-1",
             "2-1",
             "01-2",
             "1-02",
             "1-256",
         }) {
        CAPTURE(selector);
        CHECK_FALSE(decode_cli_retry_exit_codes(selector_list({selector})));
    }
}

TEST_CASE("CLI retry exit selectors reject duplicate and overlapping membership", "[jobu][cli][exit-policy]")
{
    for (auto const& selectors : {
             selector_list({"1", "1"}),
             selector_list({"1", "1-2"}),
             selector_list({"1-3", "3-5"}),
             selector_list({"10-20", "12"}),
             selector_list({"200-220", "190-210"}),
         }) {
        CHECK_FALSE(decode_cli_retry_exit_codes(selectors));
    }

    CHECK_FALSE(decode_cli_retry_exit_codes({AttributeValue{.data = std::int64_t{1}}}));
}

TEST_CASE("CLI retry exit selectors retain the input count bound before normalization", "[jobu][cli][exit-policy]")
{
    auto selectors = AttributeValue::List{};
    selectors.reserve(65U);
    for (std::uint16_t code = 0; code < 128; code += 2) {
        selectors.push_back({.data = std::to_string(code)});
    }

    auto maximum = decode_cli_retry_exit_codes(selectors);
    REQUIRE(maximum);
    CHECK(maximum->ranges().size() == 64U);

    selectors.push_back({.data = std::string{"128"}});
    CHECK_FALSE(decode_cli_retry_exit_codes(selectors));
}

TEST_CASE("CLI numeric exit mapping gives expected codes precedence over retry membership",
          "[jobu][cli][exit-policy][result]")
{
    auto expected = CliExpectedExitCodes{};
    expected.set(7U);
    auto retryable = retry_policy({"7", "9"});

    auto success = map_cli_completion(
        ProcessExit{
            .kind      = ProcessExitKind::Exited,
            .exit_code = 7,
        },
        expected,
        retryable,
        CliCaptureMode::Always,
        capture_snapshot());
    REQUIRE(success);
    CHECK(success->outcome == AttemptOutcome::Succeeded);
    CHECK_FALSE(success->failure_disposition);
    CHECK(result_outcome(*success) == "success");

    auto retry = map_cli_completion(
        ProcessExit{
            .kind      = ProcessExitKind::Exited,
            .exit_code = 9,
        },
        expected,
        retryable,
        CliCaptureMode::Always,
        capture_snapshot());
    REQUIRE(retry);
    CHECK(retry->outcome == AttemptOutcome::Failed);
    CHECK(retry->failure_disposition == FailureDisposition::Retryable);
    CHECK(result_outcome(*retry) == "unexpected_exit");

    auto terminal = map_cli_completion(
        ProcessExit{
            .kind      = ProcessExitKind::Exited,
            .exit_code = 8,
        },
        expected,
        retryable,
        CliCaptureMode::Always,
        capture_snapshot());
    REQUIRE(terminal);
    CHECK(terminal->outcome == AttemptOutcome::Failed);
    CHECK(terminal->failure_disposition == FailureDisposition::Terminal);
    CHECK(result_outcome(*terminal) == "unexpected_exit");
}

TEST_CASE("CLI non-numeric process observations map to fixed attempt policy", "[jobu][cli][exit-policy][result]")
{
    auto const expected  = CliExpectedExitCodes{};
    auto const retryable = retry_policy({});

    SECTION("unrequested signal")
    {
        auto mapped = map_cli_completion(
            ProcessExit{
                .kind          = ProcessExitKind::Signaled,
                .signal_number = 15,
            },
            expected,
            retryable,
            CliCaptureMode::Always,
            capture_snapshot());
        REQUIRE(mapped);
        CHECK(mapped->outcome == AttemptOutcome::Failed);
        CHECK(mapped->failure_disposition == FailureDisposition::Retryable);
        CHECK(result_outcome(*mapped) == "signal");
        CHECK(mapped->result.as_object().at("signal").as_uint() == 15U);
    }

    SECTION("timeout preserving a normal exit")
    {
        auto mapped = map_cli_completion(
            ProcessExit{
                .kind      = ProcessExitKind::TimedOut,
                .exit_code = 124,
            },
            expected,
            retryable,
            CliCaptureMode::Always,
            capture_snapshot());
        REQUIRE(mapped);
        CHECK(mapped->outcome == AttemptOutcome::Failed);
        CHECK(mapped->failure_disposition == FailureDisposition::Retryable);
        CHECK(result_outcome(*mapped) == "timeout");
        CHECK(mapped->result.as_object().at("exit_code").as_uint() == 124U);
    }

    SECTION("explicit cancellation")
    {
        auto mapped = map_cli_completion(
            ProcessExit{
                .kind          = ProcessExitKind::Cancelled,
                .signal_number = 15,
            },
            expected,
            retryable,
            CliCaptureMode::Always,
            capture_snapshot());
        REQUIRE(mapped);
        CHECK(mapped->outcome == AttemptOutcome::Cancelled);
        CHECK_FALSE(mapped->failure_disposition);
        CHECK(result_outcome(*mapped) == "cancelled");
        CHECK(mapped->result.as_object().at("signal").as_uint() == 15U);
    }

    SECTION("asynchronous start failure")
    {
        auto mapped = map_cli_completion(
            ProcessExit{
                .kind = ProcessExitKind::StartFailed,
                .start_error =
                    Error{
                          .category = ErrorCategory::NotFound,
                          .code     = "core.process.exec_failed",
                          .message  = "sensitive-message-marker",
                          .detail   = "sensitive-detail-marker",
                          },
        },
            expected,
            retryable,
            CliCaptureMode::Always,
            capture_snapshot());
        REQUIRE(mapped);
        CHECK(mapped->outcome == AttemptOutcome::Failed);
        CHECK(mapped->failure_disposition == FailureDisposition::Terminal);
        CHECK(result_outcome(*mapped) == "start_failure");
        CHECK(mapped->result.as_object().at("error_code").as_string() == "core.process.exec_failed");

        auto serialized = serialize_json(mapped->result);
        REQUIRE(serialized);
        CHECK(serialized->find("sensitive-message-marker") == std::string::npos);
        CHECK(serialized->find("sensitive-detail-marker") == std::string::npos);
    }
}

TEST_CASE("CLI result metadata survives capture policy output omission", "[jobu][cli][exit-policy][capture]")
{
    auto expected = CliExpectedExitCodes{};
    expected.set(0U);
    auto const retryable = retry_policy({"0-255"});
    auto const exit      = ProcessExit{
        .kind        = ProcessExitKind::Exited,
        .exit_code   = 0,
        .stdout_lost = true,
    };

    SECTION("none reports observation without retained bytes")
    {
        auto mapped = map_cli_completion(exit, expected, retryable, CliCaptureMode::None, capture_snapshot());
        REQUIRE(mapped);
        CHECK_FALSE(mapped->output);

        auto const& result = mapped->result.as_object();
        CHECK(result.at("capture_lost").as_bool());
        auto const& stdout_result = result.at("stdout").as_object();
        CHECK(stdout_result.at("captured_bytes").as_uint() == 0U);
        CHECK(stdout_result.at("total_bytes").as_uint() == 5U);
        CHECK(stdout_result.at("truncated").as_bool());
        auto const& stderr_result = result.at("stderr").as_object();
        CHECK(stderr_result.at("captured_bytes").as_uint() == 0U);
        CHECK(stderr_result.at("total_bytes").as_uint() == 2U);
        CHECK(stderr_result.at("truncated").as_bool());
    }

    SECTION("successful on-error capture retains metadata but omits output")
    {
        auto mapped = map_cli_completion(exit, expected, retryable, CliCaptureMode::OnError, capture_snapshot());
        REQUIRE(mapped);
        CHECK_FALSE(mapped->output);

        auto const& stdout_result = mapped->result.as_object().at("stdout").as_object();
        CHECK(stdout_result.at("captured_bytes").as_uint() == 3U);
        CHECK(stdout_result.at("total_bytes").as_uint() == 5U);
        CHECK(stdout_result.at("truncated").as_bool());
    }

    SECTION("always capture persists both channels and loss metadata")
    {
        auto mapped = map_cli_completion(exit, expected, retryable, CliCaptureMode::Always, capture_snapshot(true));
        REQUIRE(mapped);
        REQUIRE(mapped->output);
        REQUIRE(mapped->output->primary);
        REQUIRE(mapped->output->diagnostic);
        CHECK(as_string_view(mapped->output->primary->bytes) == "abc");
        CHECK(as_string_view(mapped->output->diagnostic->bytes) == "xy");
        CHECK(mapped->output->capture_lost);
    }
}

TEST_CASE("CLI on-error capture persists both stream channels for non-success outcomes",
          "[jobu][cli][exit-policy][capture]")
{
    auto const expected  = CliExpectedExitCodes{};
    auto const retryable = retry_policy({});

    for (auto const& process_exit : {
             ProcessExit{
                         .kind      = ProcessExitKind::Exited,
                         .exit_code = 2,
                         },
             ProcessExit{
                         .kind          = ProcessExitKind::Cancelled,
                         .signal_number = 15,
                         },
    }) {
        auto mapped =
            map_cli_completion(process_exit, expected, retryable, CliCaptureMode::OnError, capture_snapshot());
        REQUIRE(mapped);
        REQUIRE(mapped->output);
        REQUIRE(mapped->output->primary);
        REQUIRE(mapped->output->diagnostic);
        CHECK(mapped->output->primary->total_bytes == 5U);
        CHECK(mapped->output->diagnostic->total_bytes == 2U);
    }
}

TEST_CASE("CLI result JSON has deterministic bounded shape and excludes captured bytes",
          "[jobu][cli][exit-policy][result]")
{
    auto expected = CliExpectedExitCodes{};
    expected.set(0U);
    auto const retryable = retry_policy({"0-255"});
    auto       mapped    = map_cli_completion(
        ProcessExit{
            .kind      = ProcessExitKind::Exited,
            .exit_code = 0,
        },
        expected,
        retryable,
        CliCaptureMode::Always,
        CliCaptureSnapshot{
            .standard_output = output_channel("sensitive-output-marker", 23U),
            .standard_error  = output_channel("", 0U),
        });
    REQUIRE(mapped);

    auto serialized = serialize_json(mapped->result);
    REQUIRE(serialized);
    CHECK(
        *serialized ==
        R"({"capture_lost":false,"exit_code":0,"outcome":"success","stderr":{"captured_bytes":0,"total_bytes":0,"truncated":false},"stdout":{"captured_bytes":23,"total_bytes":23,"truncated":false},"type":"cli"})");
    CHECK(serialized->find("sensitive-output-marker") == std::string::npos);
    CHECK(serialized->size() < std::size_t{256} * 1024U);
}

TEST_CASE("CLI result mapping fails closed for reserved or malformed Process observations",
          "[jobu][cli][exit-policy][result]")
{
    auto const expected  = CliExpectedExitCodes{};
    auto const retryable = retry_policy({});

    auto interrupted = map_cli_completion(
        ProcessExit{
            .kind          = ProcessExitKind::Interrupted,
            .signal_number = 9,
        },
        expected,
        retryable,
        CliCaptureMode::Always,
        capture_snapshot());
    REQUIRE_FALSE(interrupted);
    CHECK(interrupted.error().code == "jobu.cli.invalid_result");
    CHECK(interrupted.error().detail == "interrupted_reserved");

    auto malformed = map_cli_completion(
        ProcessExit{
            .kind          = ProcessExitKind::Exited,
            .exit_code     = 0,
            .signal_number = 9,
        },
        expected,
        retryable,
        CliCaptureMode::Always,
        capture_snapshot());
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().code == "jobu.cli.invalid_result");
    CHECK(malformed.error().detail == "invalid_exit_observation");
}
