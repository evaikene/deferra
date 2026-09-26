#include "payload_template_priv.hpp"

#include "cli_job_payload_priv.hpp"
#include "http_job_payload_priv.hpp"
#include "job_validation_priv.hpp"
#include "json.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::jobu::detail;

namespace {

auto json(std::string_view text) -> JsonValue
{
    auto parsed = parse_json(text);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto reference(std::string name = "service.token") -> JsonValue
{
    return JsonValue{.data = JsonValue::Object{{"secret", JsonValue{.data = std::move(name)}}}};
}

auto& object(JsonValue& value)
{
    return std::get<JsonValue::Object>(value.data);
}

void require_issue(JobType type, JsonValue const& payload, JobPayloadIssue expected)
{
    auto validated = validate_and_serialize_job_payload(type, payload);
    REQUIRE_FALSE(validated);
    CHECK(validated.error() == expected);
}

} // namespace

TEST_CASE("Templates extract only recognized whole-value references and retain original JSON", "[jobu][template]")
{
    auto const type      = GENERATE(JobType::Cli, JobType::Http);
    auto       payload   = type == JobType::Cli
                             ? json(R"({"command":"/bin/tool","arguments":["${literal}",{"secret":"shared.token"}],
                   "environment":{"TOKEN":{"secret":"shared.token"},"REMOVE":null},
                   "future":{"secret":"not a valid binding","nested":{"secret":"ignored"}}})")
                             : json(R"({"url":"https://example.test/","method":"POST",
                   "headers":[{"name":"X-Private","value":{"secret":"shared.token"},"sensitive":false}],
                   "body":{"secret":"body.bytes"},"future":{"secret":"ignored"}})");
    auto const original  = payload;
    auto       extracted = validate_payload_template(type, payload);
    REQUIRE(extracted);
    auto expected = type == JobType::Cli
        ? std::vector<SecretReference>{{.secret_name = "shared.token", .field_path = "/arguments/1"}, {.secret_name = "shared.token", .field_path = "/environment/TOKEN"}}
        : std::vector<SecretReference>{{.secret_name = "shared.token", .field_path = "/headers/0/value"}, {.secret_name = "body.bytes", .field_path = "/body"}};
    CHECK(*extracted == expected);
    auto stored = validate_and_serialize_job_payload(type, payload);
    REQUIRE(stored);
    CHECK(json(stored->serialized()) == original);
    CHECK(payload == original);

    if (type == JobType::Cli) {
        CHECK_FALSE(decode_cli_job_payload(payload));
    }
    else {
        CHECK_FALSE(decode_http_job_payload(payload));
    }
}

TEST_CASE("Reference grammar is closed and canonical", "[jobu][template]")
{
    auto payload = json(R"({"command":"/bin/tool","arguments":[]})");
    for (auto const* malformed :
         {"{}", R"({"secret":null})", R"({"secret":7})", R"({"secret":"service.token","default":"x"})"}) {
        object(payload)["arguments"] = JsonValue{.data = JsonValue::Array{json(malformed)}};
        require_issue(JobType::Cli, payload, JobPayloadIssue::InvalidSecretReference);
    }
    for (auto const& name :
         std::vector<std::string>{"", "Upper", "bad name", "a..b", "a.", "1a", "a/b", "a~b", std::string(129U, 'a')}) {
        object(payload)["arguments"] = JsonValue{.data = JsonValue::Array{reference(name)}};
        require_issue(JobType::Cli, payload, JobPayloadIssue::InvalidSecretName);
    }
    object(payload)["arguments"] = JsonValue{.data = JsonValue::Array{reference(std::string(128U, 'a'))}};
    REQUIRE(validate_and_serialize_job_payload(JobType::Cli, payload));
    CHECK(job_payload_issue_text(JobPayloadIssue::InvalidSecretReference) == "invalid_secret_reference");
    CHECK(job_payload_issue_text(JobPayloadIssue::InvalidSecretName) == "invalid_secret_name");
    CHECK(job_payload_issue_text(JobPayloadIssue::TooManySecretReferences) == "too_many_secret_references");
}

TEST_CASE("References are forbidden in routing and literal-only fields", "[jobu][template]")
{
    for (auto const* field : {"command", "working_directory", "expected_exit_codes"}) {
        auto payload           = json(R"({"command":"/bin/tool"})");
        object(payload)[field] = reference();
        CHECK_FALSE(validate_payload_template(JobType::Cli, payload));
    }
    for (auto const* name : {"PATH", "JOBU_JOB_ID", "JOBU_RUN_ID", "JOBU_ATTEMPT", "JOBU_CUSTOM", "A/B", "A~B"}) {
        auto payload                   = json(R"({"command":"/bin/tool"})");
        object(payload)["environment"] = JsonValue{.data = JsonValue::Object{{name, reference()}}};
        require_issue(JobType::Cli, payload, JobPayloadIssue::InvalidEnvironment);
    }
    for (auto const* field : {"url", "method", "expected_statuses"}) {
        auto payload           = json(R"({"url":"https://example.test/"})");
        object(payload)[field] = reference();
        CHECK_FALSE(validate_payload_template(JobType::Http, payload));
    }
    auto header_name = json(R"({"url":"https://example.test/","headers":[{"name":{"secret":"x"},"value":"x"}]})");
    require_issue(JobType::Http, header_name, JobPayloadIssue::InvalidHeaders);
}

TEST_CASE("Reference paths escape components and occurrence bounds precede deduplication", "[jobu][template]")
{
    CHECK(escape_payload_pointer_component("a~/b~1") == "a~0~1b~01");
    CHECK(escape_payload_pointer_component("").empty());

    // Environment identifiers currently forbid these characters; test the path primitive without relaxing that rule.
    PayloadTemplateReferences collected;
    REQUIRE(collected.add(reference(), "/environment/TOKEN") == JobPayloadIssue::None);
    REQUIRE(collected.add(reference(), "/environment/TOKEN") == JobPayloadIssue::None);
    auto unique = std::move(collected).take();
    CHECK(unique == std::vector<SecretReference>{
                        {.secret_name = "service.token", .field_path = "/environment/TOKEN"}
    });

    auto payload                 = json(R"({"command":"/bin/tool"})");
    object(payload)["arguments"] = JsonValue{.data = JsonValue::Array(256U, reference())};
    auto extracted               = validate_payload_template(JobType::Cli, payload);
    REQUIRE(extracted);
    REQUIRE(extracted->size() == 256U);
    CHECK(extracted->front().field_path == "/arguments/0");
    CHECK(extracted->back().field_path == "/arguments/255");
    object(payload)["environment"] = JsonValue{.data = JsonValue::Object{{"TOKEN", reference()}}};
    require_issue(JobType::Cli, payload, JobPayloadIssue::TooManySecretReferences);
}

TEST_CASE("Templates retain literal CLI checks and known prepared-size accounting", "[jobu][template]")
{
    auto payload = json(R"({"command":"tool","environment":{"TOKEN":{"secret":"service.token"}}})");
    require_issue(JobType::Cli, payload, JobPayloadIssue::InvalidPath);
    object(object(payload)["environment"])["PATH"] = JsonValue{.data = std::string{"/bin:/usr/bin"}};
    REQUIRE(validate_payload_template(JobType::Cli, payload));

    for (auto const* invalid :
         {R"({"command":"relative/tool","arguments":[{"secret":"a"}]})",
          R"({"command":"/tool","arguments":[{"secret":"a"},"a\u0000b"]})",
          R"({"command":"/tool","arguments":[{"secret":"a"}],"working_directory":"relative"})",
          R"({"command":"/tool","arguments":[{"secret":"a"}],"expected_exit_codes":[256]})",
          R"({"command":"/tool","arguments":[{"secret":"a"}],"environment":{"TOKEN":"a\u0000b"}})"}) {
        CHECK_FALSE(validate_payload_template(JobType::Cli, json(invalid)));
    }

    object(payload)["arguments"] = JsonValue{.data = JsonValue::Array(1025U, JsonValue{.data = std::string{}})};
    require_issue(JobType::Cli, payload, JobPayloadIssue::InvalidArguments);

    object(payload)["arguments"] = JsonValue{
        .data = JsonValue::Array{reference(), JsonValue{.data = std::string(maximum_cli_prepared_request_bytes, 'x')}}
    };
    auto oversized = validate_payload_template(JobType::Cli, payload);
    REQUIRE_FALSE(oversized);
    CHECK(oversized.error() == JobPayloadIssue::PreparedRequestTooLarge);
}

TEST_CASE("HTTP templates retain literal and structural header and body checks", "[jobu][template]")
{
    for (
        auto const* invalid :
        {R"({"url":"ftp://host/","body":{"secret":"a"}})",
         R"({"url":"https://host/","method":"HAS SPACE","body":{"secret":"a"}})",
         R"({"url":"https://host/","method":"HEAD","body":{"secret":"a"}})",
         R"({"url":"https://host/","headers":[{"name":"Host","value":{"secret":"a"}}]})",
         R"({"url":"https://host/","headers":[{"name":"X-JobU-Test","value":{"secret":"a"}}]})",
         R"({"url":"https://host/","headers":[{"name":"Bad Name","value":{"secret":"a"}}]})",
         R"({"url":"https://host/","headers":[{"name":"X","value":{"secret":"a"},"sensitive":"no"}]})",
         R"({"url":"https://host/","headers":[{"name":"X","value":{"secret":"a"},"extra":1}]})",
         R"({"url":"https://host/","headers":[{"name":"X","value":{"secret":"a"}},{"name":"x","value":"b"}]})",
         R"({"url":"https://host/","headers":[{"name":"X","value":{"secret":"a"}},{"name":"Y","value":"bad\n"}]})",
         R"({"url":"https://host/","body":{"secret":"a"},"expected_statuses":["600"]})",
         R"({"url":"https://host/","headers":[{"name":"X","value":{"secret":"a"}}],"body":{"encoding":"base64","data":"TR=="}})",
         R"({"url":"https://host/","body":{"secret":"a","encoding":"utf8","data":"literal"}})"}) {
        CAPTURE(invalid);
        CHECK_FALSE(validate_payload_template(JobType::Http, json(invalid)));
    }
    auto payload = json(R"({"url":"https://host/","body":{"secret":"a"}})");
    auto headers = JsonValue::Array{};
    for (std::size_t i = 0; i < 124U; ++i) {
        headers.push_back(JsonValue{
            .data = JsonValue::Object{{"name", JsonValue{.data = "X-" + std::to_string(i)}}, {"value", reference()}}
        });
    }
    object(payload)["headers"] = JsonValue{.data = headers};
    REQUIRE(validate_payload_template(JobType::Http, payload));
    headers.push_back(json(R"({"name":"Y","value":{"secret":"a"}})"));
    object(payload)["headers"] = JsonValue{.data = std::move(headers)};
    require_issue(JobType::Http, payload, JobPayloadIssue::InvalidHeaders);

    object(payload)["headers"] = JsonValue{
        .data = JsonValue::Array{JsonValue{
            .data = JsonValue::Object{{"name", JsonValue{.data = std::string(65536U, 'X')}}, {"value", reference()}}}}};
    require_issue(JobType::Http, payload, JobPayloadIssue::InvalidHeaders);
}

TEST_CASE("Original template documents keep the exact byte bound", "[jobu][template]")
{
    auto payload    = json(R"({"command":"/bin/tool","arguments":[{"secret":"a"}],"future":""})");
    auto serialized = serialize_json(payload);
    REQUIRE(serialized);
    auto& filler = std::get<std::string>(object(payload)["future"].data);
    filler.assign(maximum_job_document_bytes - serialized->size(), 'x');
    REQUIRE(validate_and_serialize_job_payload(JobType::Cli, payload));
    filler.push_back('x');
    require_issue(JobType::Cli, payload, JobPayloadIssue::TooLarge);
}
