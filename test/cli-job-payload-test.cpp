#include "cli_job_payload_priv.hpp"

#include "attempt.hpp"
#include "job_validation_priv.hpp"
#include "json.hpp"
#include "process.hpp"
#include "process_request_priv.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::core::priv;
using namespace jb::jobu;
using namespace jb::jobu::detail;

namespace {

auto json_null() -> JsonValue
{
    return {};
}

auto json_bool(bool value) -> JsonValue
{
    auto json = JsonValue{};
    json.data = value;
    return json;
}

auto json_int(std::int64_t value) -> JsonValue
{
    auto json = JsonValue{};
    json.data = value;
    return json;
}

auto json_uint(std::uint64_t value) -> JsonValue
{
    auto json = JsonValue{};
    json.data = value;
    return json;
}

auto json_double(double value) -> JsonValue
{
    auto json = JsonValue{};
    json.data = value;
    return json;
}

auto json_string(std::string value) -> JsonValue
{
    auto json = JsonValue{};
    json.data = std::move(value);
    return json;
}

auto json_array(JsonValue::Array value) -> JsonValue
{
    auto json = JsonValue{};
    json.data = std::move(value);
    return json;
}

auto json_object(JsonValue::Object value) -> JsonValue
{
    auto json = JsonValue{};
    json.data = std::move(value);
    return json;
}

auto& object(JsonValue& value)
{
    return std::get<JsonValue::Object>(value.data);
}

auto payload(std::string command = "/fixture/tool") -> JsonValue
{
    return json_object({
        {"command", json_string(std::move(command))}
    });
}

void set_member(JsonValue& payload_value, std::string name, JsonValue value)
{
    object(payload_value).insert_or_assign(std::move(name), std::move(value));
}

void check_issue(JsonValue const& payload_value, JobPayloadIssue expected)
{
    auto decoded = decode_cli_job_payload(payload_value);
    REQUIRE_FALSE(decoded);
    CHECK(decoded.error() == expected);
    CHECK(job_payload_structure_issue(JobType::Cli, payload_value) == expected);

    auto validated = validate_and_serialize_job_payload(JobType::Cli, payload_value);
    REQUIRE_FALSE(validated);
    CHECK(validated.error() == expected);
}

auto joined_path(std::size_t count, std::string_view directory) -> std::string
{
    std::string path;
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0) {
            path += ':';
        }
        path += directory;
    }
    return path;
}

auto with_path(std::string command, JsonValue path) -> JsonValue
{
    return json_object({
        {"command",     json_string(std::move(command))         },
        {"environment", json_object({{"PATH", std::move(path)}})},
    });
}

auto process_request(CliJobPayload const& decoded) -> ProcessStartInfo
{
    auto request              = ProcessStartInfo{};
    request.executable        = decoded.command;
    request.arguments         = decoded.arguments;
    request.working_directory = decoded.working_directory;
    for (auto const& [name, value] : decoded.environment) {
        if (value) {
            request.environment.emplace(name, *value);
        }
    }

    // Use maximum metadata values so this request matches management-time aggregate reservation exactly.
    request.environment.emplace("JOBU_JOB_ID", "00000000-0000-0000-0000-000000000000");
    request.environment.emplace("JOBU_RUN_ID", "00000000-0000-0000-0000-000000000000");
    request.environment.emplace("JOBU_ATTEMPT", std::to_string(std::numeric_limits<AttemptNumber>::max()));
    return request;
}

auto prepare(CliJobPayload const& decoded)
{
    return prepare_process_request(process_request(decoded), TimePoint{}, 2L * 1024 * 1024);
}

} // namespace

TEST_CASE("CLI payload applies execution defaults and preserves additive members", "[jobu][cli][payload]")
{
    auto input = payload();
    set_member(input, "future", json_bool(true));

    auto decoded = decode_cli_job_payload(input);
    REQUIRE(decoded);
    CHECK(decoded->command == "/fixture/tool");
    CHECK(decoded->arguments.empty());
    CHECK(decoded->working_directory == "/");
    CHECK(decoded->environment.empty());
    CHECK(decoded->expected_exit_codes.count() == 1);
    CHECK(decoded->expected_exit_codes.test(0));

    auto original = serialize_json(input);
    REQUIRE(original);
    auto validated = validate_and_serialize_job_payload(JobType::Cli, input);
    REQUIRE(validated);
    CHECK(validated->serialized() == *original);
    CHECK(validated->serialized().find("future") != std::string_view::npos);
    CHECK(validated->serialized().find("arguments") == std::string_view::npos);
}

TEST_CASE("CLI payload decodes complete optional execution fields", "[jobu][cli][payload]")
{
    auto input = json_object({
        {"arguments",           json_array({json_string(""), json_string("-x"), json_string("two words")})},
        {"command",             json_string("/fixture/tool")                                              },
        {"environment",
         json_object({
             {"A", json_string("")},
             {"PATH", json_string("relative::ignored")},
             {"REMOVE", json_null()},
         })                                                                                               },
        {"expected_exit_codes", json_array({json_uint(0), json_uint(7), json_uint(255)})                  },
        {"working_directory",   json_string("/work/../literal")                                           },
    });

    auto decoded = decode_cli_job_payload(input);
    REQUIRE(decoded);
    CHECK(decoded->arguments == std::vector<std::string>{"", "-x", "two words"});
    CHECK(decoded->working_directory == "/work/../literal");
    CHECK(decoded->environment.at("A") == std::optional<std::string>{""});
    CHECK(decoded->environment.at("PATH") == std::optional<std::string>{"relative::ignored"});
    CHECK_FALSE(decoded->environment.at("REMOVE"));
    CHECK(decoded->expected_exit_codes.count() == 3);
    CHECK(decoded->expected_exit_codes.test(0));
    CHECK(decoded->expected_exit_codes.test(7));
    CHECK(decoded->expected_exit_codes.test(255));
    CHECK(prepare(*decoded));
}

TEST_CASE("CLI payload keeps the original document size boundary", "[jobu][cli][payload]")
{
    auto input = payload();
    set_member(input, "future", json_string(""));
    auto serialized = serialize_json(input);
    REQUIRE(serialized);
    REQUIRE(serialized->size() < maximum_job_document_bytes);

    auto& filler = std::get<std::string>(object(input).at("future").data);
    filler.resize(maximum_job_document_bytes - serialized->size(), 'x');
    auto validated = validate_and_serialize_job_payload(JobType::Cli, input);
    REQUIRE(validated);
    CHECK(validated->serialized().size() == maximum_job_document_bytes);

    filler    += 'x';
    validated  = validate_and_serialize_job_payload(JobType::Cli, input);
    REQUIRE_FALSE(validated);
    CHECK(validated.error() == JobPayloadIssue::TooLarge);
}

TEST_CASE("CLI payload validates command arguments and working directory", "[jobu][cli][payload]")
{
    check_issue(json_object({}), JobPayloadIssue::MissingCommand);
    check_issue(json_object({
                    {"command", json_bool(true)}
    }),
                JobPayloadIssue::MissingCommand);
    check_issue(payload(""), JobPayloadIssue::MissingCommand);

    for (auto const& command : std::vector<std::string>{
             "relative/path",
             "./tool",
             "../tool",
             std::string{"tool\0suffix", 11},
             "/" + std::string(maximum_cli_path_bytes, 'x'),
    }) {
        check_issue(payload(command), JobPayloadIssue::InvalidCommand);
    }
    REQUIRE(decode_cli_job_payload(payload("/" + std::string(maximum_cli_path_bytes - 1U, 'x'))));

    auto invalid_arguments = payload();
    set_member(invalid_arguments, "arguments", json_string("not-an-array"));
    check_issue(invalid_arguments, JobPayloadIssue::InvalidArguments);
    set_member(invalid_arguments, "arguments", json_array({json_bool(true)}));
    check_issue(invalid_arguments, JobPayloadIssue::InvalidArguments);
    set_member(invalid_arguments, "arguments", json_array({json_string(std::string{"a\0b", 3})}));
    check_issue(invalid_arguments, JobPayloadIssue::InvalidArguments);

    auto maximum_arguments = JsonValue::Array(maximum_cli_arguments, json_string(""));
    set_member(invalid_arguments, "arguments", json_array(maximum_arguments));
    REQUIRE(decode_cli_job_payload(invalid_arguments));
    maximum_arguments.push_back(json_string(""));
    set_member(invalid_arguments, "arguments", json_array(std::move(maximum_arguments)));
    check_issue(invalid_arguments, JobPayloadIssue::InvalidArguments);

    auto invalid_directory = payload();
    for (auto directory : std::vector<JsonValue>{
             json_null(),
             json_string(""),
             json_string("relative"),
             json_string(std::string{"/a\0b", 4}),
             json_string("/" + std::string(maximum_cli_path_bytes, 'x')),
         }) {
        set_member(invalid_directory, "working_directory", std::move(directory));
        check_issue(invalid_directory, JobPayloadIssue::InvalidWorkingDirectory);
    }
    set_member(invalid_directory,
               "working_directory",
               json_string("/" + std::string(maximum_cli_path_bytes - 1U, 'x')));
    REQUIRE(decode_cli_job_payload(invalid_directory));
}

TEST_CASE("CLI payload validates the literal environment patch", "[jobu][cli][payload]")
{
    auto input = payload();
    set_member(input, "environment", json_array({}));
    check_issue(input, JobPayloadIssue::InvalidEnvironment);

    for (auto const& name : std::vector<std::string>{"", "1A", "A=B", "A-B", " A", "A B", "A\n", "\xc3\xa4"}) {
        set_member(input,
                   "environment",
                   json_object({
                       {name, json_string("private-marker")}
        }));
        check_issue(input, JobPayloadIssue::InvalidEnvironment);
    }
    for (char const* const name : {"JOBU_JOB_ID", "JOBU_RUN_ID", "JOBU_ATTEMPT"}) {
        set_member(input,
                   "environment",
                   json_object({
                       {name, json_null()}
        }));
        check_issue(input, JobPayloadIssue::InvalidEnvironment);
    }
    set_member(input,
               "environment",
               json_object({
                   {"A", json_bool(true)}
    }));
    check_issue(input, JobPayloadIssue::InvalidEnvironment);
    set_member(input,
               "environment",
               json_object({
                   {"A", json_string(std::string{"x\0y", 3})}
    }));
    check_issue(input, JobPayloadIssue::InvalidEnvironment);

    auto maximum_environment = JsonValue::Object{};
    for (std::size_t index = 0; index < maximum_cli_environment_entries; ++index) {
        maximum_environment.emplace("V" + std::to_string(index), json_null());
    }
    set_member(input, "environment", json_object(maximum_environment));
    REQUIRE(decode_cli_job_payload(input));
    maximum_environment.emplace("OVER", json_null());
    set_member(input, "environment", json_object(std::move(maximum_environment)));
    check_issue(input, JobPayloadIssue::InvalidEnvironment);
}

TEST_CASE("CLI bare-command PATH limits match Process candidate preparation", "[jobu][cli][payload]")
{
    STATIC_REQUIRE(maximum_cli_path_bytes == kMaxProcessPathBytes);
    STATIC_REQUIRE(maximum_cli_arguments == kMaxProcessArguments);
    STATIC_REQUIRE(maximum_cli_path_entries == kMaxPathEntries);
    STATIC_REQUIRE(maximum_cli_path_candidate_bytes == kMaxPathCandidateBytes);
    STATIC_REQUIRE(maximum_cli_prepared_request_bytes == kMaxProcessArgumentBytes);

    check_issue(payload("tool"), JobPayloadIssue::InvalidPath);
    check_issue(with_path("tool", json_null()), JobPayloadIssue::InvalidPath);
    for (char const* const path : {"", ":/bin", "/bin:", "/bin::/usr/bin", "bin", "/bin:relative", "/bin:./local"}) {
        check_issue(with_path("tool", json_string(path)), JobPayloadIssue::InvalidPath);
    }

    auto valid   = with_path("tool", json_string("/first:/second/:/:/first"));
    auto decoded = decode_cli_job_payload(valid);
    REQUIRE(decoded);
    REQUIRE(validate_and_serialize_job_payload(JobType::Cli, valid));
    CHECK(prepare(*decoded));

    auto exact_entries = with_path("x", json_string(joined_path(maximum_cli_path_entries, "/d")));
    decoded            = decode_cli_job_payload(exact_entries);
    REQUIRE(decoded);
    REQUIRE(validate_and_serialize_job_payload(JobType::Cli, exact_entries));
    auto prepared = prepare(*decoded);
    REQUIRE(prepared);
    CHECK(prepared.value()->candidates().size() == maximum_cli_path_entries);

    auto too_many = exact_entries;
    object(object(too_many).at("environment")).at("PATH") =
        json_string(joined_path(maximum_cli_path_entries + 1U, "/d"));
    check_issue(too_many, JobPayloadIssue::InvalidPath);
    auto process_too_many                 = process_request(*decoded);
    process_too_many.environment["PATH"] += ":/d";
    auto process_result = prepare_process_request(std::move(process_too_many), TimePoint{}, 2L * 1024 * 1024);
    REQUIRE_FALSE(process_result);
    CHECK(process_result.error().detail == "path.too_many_entries");

    auto exact_bytes = with_path(std::string(1020, 'x'), json_string(joined_path(maximum_cli_path_entries, "/d")));
    decoded          = decode_cli_job_payload(exact_bytes);
    REQUIRE(decoded);
    REQUIRE(validate_and_serialize_job_payload(JobType::Cli, exact_bytes));
    prepared = prepare(*decoded);
    REQUIRE(prepared);
    std::size_t candidate_bytes{0};
    for (auto const& candidate : prepared.value()->candidates()) {
        candidate_bytes += candidate.size() + 1U;
    }
    CHECK(candidate_bytes == maximum_cli_path_candidate_bytes);

    auto  too_large_path = exact_bytes;
    auto& path           = std::get<std::string>(object(object(too_large_path).at("environment")).at("PATH").data);
    path.insert(1, "d");
    check_issue(too_large_path, JobPayloadIssue::InvalidPath);
    auto process_too_large = process_request(*decoded);
    process_too_large.environment["PATH"].insert(1, "d");
    process_result = prepare_process_request(std::move(process_too_large), TimePoint{}, 2L * 1024 * 1024);
    REQUIRE_FALSE(process_result);
    CHECK(process_result.error().detail == "path.candidates_too_large");
}

TEST_CASE("CLI prepared aggregate reserves worst-case JobU metadata", "[jobu][cli][payload]")
{
    auto input = payload("/x");
    set_member(input, "arguments", json_array(JsonValue::Array(maximum_cli_arguments, json_string(""))));
    set_member(input,
               "environment",
               json_object({
                   {"FILL", json_string("")}
    }));

    auto decoded = decode_cli_job_payload(input);
    REQUIRE(decoded);
    auto prepared = prepare(*decoded);
    REQUIRE(prepared);
    REQUIRE(prepared.value()->argument_bytes() < maximum_cli_prepared_request_bytes);

    auto const remaining = maximum_cli_prepared_request_bytes - prepared.value()->argument_bytes();
    object(object(input).at("environment")).at("FILL") = json_string(std::string(remaining, 'x'));

    auto validated = validate_and_serialize_job_payload(JobType::Cli, input);
    REQUIRE(validated);
    CHECK(validated->serialized().size() <= maximum_job_document_bytes);
    decoded = decode_cli_job_payload(input);
    REQUIRE(decoded);
    prepared = prepare(*decoded);
    REQUIRE(prepared);
    CHECK(prepared.value()->argument_bytes() == maximum_cli_prepared_request_bytes);

    auto& fill  = std::get<std::string>(object(object(input).at("environment")).at("FILL").data);
    fill       += 'x';
    check_issue(input, JobPayloadIssue::PreparedRequestTooLarge);

    auto oversized_request                 = process_request(*decoded);
    oversized_request.environment["FILL"] += 'x';
    auto process_result = prepare_process_request(std::move(oversized_request), TimePoint{}, 2L * 1024 * 1024);
    REQUIRE_FALSE(process_result);
    CHECK(process_result.error().detail == "aggregate.too_large");
}

TEST_CASE("CLI expected exit codes are unique unsigned bytes", "[jobu][cli][payload]")
{
    auto input = payload();
    set_member(input, "expected_exit_codes", json_array({}));
    auto decoded = decode_cli_job_payload(input);
    REQUIRE(decoded);
    CHECK(decoded->expected_exit_codes.none());

    auto all_codes = JsonValue::Array{};
    for (std::uint64_t code = 0; code <= 255U; ++code) {
        all_codes.push_back(json_uint(code));
    }
    set_member(input, "expected_exit_codes", json_array(all_codes));
    decoded = decode_cli_job_payload(input);
    REQUIRE(decoded);
    CHECK(decoded->expected_exit_codes.all());

    all_codes.push_back(json_uint(0));
    set_member(input, "expected_exit_codes", json_array(std::move(all_codes)));
    check_issue(input, JobPayloadIssue::InvalidExpectedExitCodes);
    for (auto value : std::vector<JsonValue>{json_uint(256), json_int(0), json_double(1.0), json_string("0")}) {
        set_member(input, "expected_exit_codes", json_array({std::move(value)}));
        check_issue(input, JobPayloadIssue::InvalidExpectedExitCodes);
    }
    set_member(input, "expected_exit_codes", json_array({json_uint(7), json_uint(7)}));
    check_issue(input, JobPayloadIssue::InvalidExpectedExitCodes);
}

TEST_CASE("CLI payload issue text exposes only fixed safe reasons", "[jobu][cli][payload]")
{
    CHECK(job_payload_issue_text(JobPayloadIssue::InvalidCommand) == "invalid_command");
    CHECK(job_payload_issue_text(JobPayloadIssue::InvalidArguments) == "invalid_arguments");
    CHECK(job_payload_issue_text(JobPayloadIssue::InvalidWorkingDirectory) == "invalid_working_directory");
    CHECK(job_payload_issue_text(JobPayloadIssue::InvalidEnvironment) == "invalid_environment");
    CHECK(job_payload_issue_text(JobPayloadIssue::InvalidPath) == "invalid_path");
    CHECK(job_payload_issue_text(JobPayloadIssue::PreparedRequestTooLarge) == "prepared_request_too_large");
    CHECK(job_payload_issue_text(JobPayloadIssue::InvalidExpectedExitCodes) == "invalid_expected_exit_codes");
}
