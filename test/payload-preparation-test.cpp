#include "payload_template_priv.hpp"

#include "byte_buffer.hpp"
#include "cli_job_payload_priv.hpp"
#include "http_job_payload_priv.hpp"
#include "json.hpp"
#include "secret_provider.hpp"
#include "support/catch_utils.hpp" // IWYU pragma: keep for Catch::StringMaker specializations

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <map>
#include <optional>
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

auto bytes(std::string_view text) -> ByteBuffer
{
    auto view = as_bytes(text);
    return {view.begin(), view.end()};
}

class Provider final : public SecretProvider {
public:
    std::map<std::string, ByteBuffer, std::less<>>  values;
    std::map<std::string, std::size_t, std::less<>> calls;
    std::optional<Error>                            failure;

    auto resolve(std::string_view name) -> Result<ByteBuffer, Error> override
    {
        ++calls[std::string{name}];
        if (failure) {
            return Result<ByteBuffer, Error>::failure(*failure);
        }
        auto found = values.find(name);
        if (found == values.end()) {
            return Result<ByteBuffer, Error>::failure({.category = ErrorCategory::NotFound,
                                                       .code     = "jobu.secret.not_found",
                                                       .message  = "private-sentinel",
                                                       .detail   = "private-sentinel"});
        }
        return Result<ByteBuffer, Error>::success(found->second);
    }
};

void check_failure(Result<JsonValue, PayloadPreparationFailure> const& result,
                   PayloadPreparationFailureKind                       kind,
                   std::string_view                                    code)
{
    REQUIRE_FALSE(result);
    CHECK(result.error().kind == kind);
    CHECK(result.error().error.code == code);
    CHECK(result.error().error.message.find("private-sentinel") == std::string::npos);
    CHECK(result.error().error.detail.find("private-sentinel") == std::string::npos);
}

} // namespace

TEST_CASE("Preparation caches each name per attempt and preserves the durable CLI template")
{
    Provider provider;
    provider.values["token"] = bytes("first-private-sentinel");
    auto       original      = json(R"({"command":"/bin/tool","arguments":[{"secret":"token"},{"secret":"token"}],
        "environment":{"TOKEN":{"secret":"token"},"REMOVE":null},"future":{"secret":"ignored"}})");
    auto const snapshot      = original;
    auto       prepared      = prepare_payload_template(JobType::Cli, original, provider);
    REQUIRE(prepared);
    auto decoded = decode_cli_job_payload(*prepared);
    REQUIRE(decoded);
    CHECK(decoded->arguments == std::vector<std::string>{"first-private-sentinel", "first-private-sentinel"});
    CHECK(decoded->environment.at("TOKEN") == "first-private-sentinel");
    CHECK_FALSE(decoded->environment.at("REMOVE"));
    CHECK(provider.calls.size() == 1);
    CHECK(provider.calls.at("token") == 1);
    CHECK(prepared->as_object().at("future") == snapshot.as_object().at("future"));
    CHECK(original == snapshot);

    // A separate invocation represents a new attempt and must observe the rotated value.
    provider.values["token"] = bytes("rotated");
    prepared                 = prepare_payload_template(JobType::Cli, original, provider);
    REQUIRE(prepared);
    decoded = decode_cli_job_payload(*prepared);
    REQUIRE(decoded);
    CHECK(decoded->arguments.front() == "rotated");
    CHECK(provider.calls.at("token") == 2);
    CHECK(original == snapshot);
}

TEST_CASE("Preparation retains literal payloads and ignores additive secret-shaped objects")
{
    auto type = GENERATE(JobType::Cli, JobType::Http);
    auto original =
        type == JobType::Cli
            ? json(
                  R"({"command":"/bin/tool","arguments":["literal"],"environment":{"TOKEN":"text","REMOVE":null},"future":{"secret":"ignored"}})")
            : json(
                  R"({"url":"https://example.test/","headers":[{"name":"X-Test","value":"text","sensitive":false}],"body":{"encoding":"utf8","data":"body"},"future":{"secret":"ignored"}})");
    Provider provider;
    auto     prepared = prepare_payload_template(type, original, provider);
    REQUIRE(prepared);
    CHECK(*prepared == original);
    CHECK(provider.calls.empty());
}

TEST_CASE("HTTP preparation forces reference sensitivity and caches a name shared with the body")
{
    Provider provider;
    provider.values["token"] = bytes("private-sentinel");
    auto original            = json(R"({"url":"https://example.test/","method":"POST",
        "headers":[{"name":"X-Private","value":{"secret":"token"},"sensitive":false},
                   {"name":"X-Other","value":{"secret":"token"}}],"body":{"secret":"token"}})");
    auto snapshot            = original;
    auto prepared            = prepare_payload_template(JobType::Http, original, provider);
    REQUIRE(prepared);
    auto decoded = decode_http_job_payload(*prepared);
    REQUIRE(decoded);
    REQUIRE(decoded->headers.size() == 2);
    for (auto const& header : decoded->headers) {
        CHECK(header.value == "private-sentinel");
        CHECK(header.sensitive);
    }
    REQUIRE(decoded->body);
    CHECK(*decoded->body == provider.values.at("token"));
    CHECK(provider.calls.at("token") == 1);
    CHECK(original == snapshot);
}

TEST_CASE("HTTP secret bodies preserve arbitrary bytes and every base64 tail length")
{
    auto     size = GENERATE(0U, 1U, 2U, 3U, 4U, 256U, 65536U);
    Provider provider;
    auto&    value = provider.values["token"];
    for (unsigned index = 0; index < size; ++index) {
        value.push_back(static_cast<std::byte>(index % 256U));
    }
    auto original = json(R"({"url":"https://example.test/","method":"POST","body":{"secret":"token"}})");
    auto prepared = prepare_payload_template(JobType::Http, original, provider);
    REQUIRE(prepared);
    auto decoded = decode_http_job_payload(*prepared);
    REQUIRE(decoded);
    REQUIRE(decoded->body);
    CHECK(*decoded->body == value);
}

TEST_CASE("Text destinations reject invalid UTF8 and NUL but preserve empty and Unicode values")
{
    auto destination = GENERATE(0, 1, 2);
    auto original    = json(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})");
    if (destination == 1) {
        original = json(R"({"command":"/bin/tool","environment":{"TOKEN":{"secret":"token"}}})");
    }
    else if (destination == 2) {
        original = json(R"({"url":"https://example.test/","headers":[{"name":"X-Test","value":{"secret":"token"}}]})");
    }
    auto     type = destination == 2 ? JobType::Http : JobType::Cli;
    Provider provider;
    for (auto const& text : {std::string{}, std::string{"Unicode-\xc3\xb5"}}) {
        provider.values["token"] = bytes(text);
        REQUIRE(prepare_payload_template(type, original, provider));
    }
    for (auto const& text : {
             std::string{"\xff"},
             std::string{"a\0b", 3},
             std::string{"\xc0\xaf"}
    }) {
        provider.values["token"] = bytes(text);
        check_failure(prepare_payload_template(type, original, provider),
                      PayloadPreparationFailureKind::Ordinary,
                      "jobu.secret.invalid_value");
    }
}

TEST_CASE("HTTP secret headers reject CRLF while CLI text retains these valid bytes")
{
    auto const* text = GENERATE("a\rb", "a\nb", "a\r\nb");
    Provider    provider;
    provider.values["token"] = bytes(text);
    auto http = json(R"({"url":"https://example.test/","headers":[{"name":"X-Test","value":{"secret":"token"}}]})");
    check_failure(prepare_payload_template(JobType::Http, http, provider),
                  PayloadPreparationFailureKind::Ordinary,
                  "jobu.secret.invalid_value");
    auto cli = json(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})");
    REQUIRE(prepare_payload_template(JobType::Cli, cli, provider));
}

TEST_CASE("Preparation classifies provider failures without exposing provider diagnostics")
{
    Provider provider;
    auto     original = json(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})");
    check_failure(prepare_payload_template(JobType::Cli, original, provider),
                  PayloadPreparationFailureKind::Ordinary,
                  "jobu.secret.not_found");
    for (auto const* code : {"db.busy",
                             "db.io",
                             "db.corrupt",
                             "jobu.storage.invariant",
                             "jobu.storage.invalid_blob",
                             "jobu.storage.invalid_limit",
                             "unknown.private-sentinel",
                             "jobu.secret.invalid_value"}) {
        provider.failure = Error{.category = ErrorCategory::InvalidArgument,
                                 .code     = code,
                                 .message  = "private-sentinel",
                                 .detail   = "private-sentinel"};
        auto storage     = std::string_view{code}.starts_with("db.");
        auto persisted   = std::string_view{code}.starts_with("jobu.storage.") &&
                           std::string_view{code} != "jobu.storage.invalid_limit";
        auto kind        = PayloadPreparationFailureKind::Provider;
        if (storage) {
            kind = PayloadPreparationFailureKind::Storage;
        }
        else if (persisted) {
            kind = PayloadPreparationFailureKind::PersistedData;
        }
        check_failure(prepare_payload_template(JobType::Cli, original, provider),
                      kind,
                      storage || persisted ? code : "jobu.secret.provider_failed");
    }
    provider.failure.reset();
    provider.values["token"] = ByteBuffer(65537U);
    check_failure(prepare_payload_template(JobType::Cli, original, provider),
                  PayloadPreparationFailureKind::Provider,
                  "jobu.secret.provider_failed");
}

TEST_CASE("Malformed or oversized durable templates fail before provider lookup")
{
    Provider provider;
    for (auto const* text :
         {R"({"command":"/bin/tool","arguments":[{"secret":"token","extra":0}]})",
          R"({"command":"/bin/tool","arguments":[{"secret":"token"}],"environment":{"PATH":{"secret":"token"}}})",
          R"({"command":"/bin/tool","arguments":[{"secret":"token"}],"working_directory":7})"}) {
        check_failure(prepare_payload_template(JobType::Cli, json(text), provider),
                      PayloadPreparationFailureKind::PersistedData,
                      "jobu.storage.invalid_json");
    }
    auto oversized = json(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})");
    std::get<JsonValue::Object>(oversized.data)["future"].data = std::string(maximum_job_document_bytes, 'x');
    check_failure(prepare_payload_template(JobType::Cli, oversized, provider),
                  PayloadPreparationFailureKind::PersistedData,
                  "jobu.storage.invalid_json");
    CHECK(provider.calls.empty());
}

TEST_CASE("Expanded payload size includes JSON escaping and preserved additive data")
{
    Provider provider;
    provider.values["token"] = bytes(std::string(65536U, '\x01'));
    auto payload             = json(R"({"command":"/bin/tool","arguments":[{"secret":"token"}]})");
    check_failure(prepare_payload_template(JobType::Cli, payload, provider),
                  PayloadPreparationFailureKind::Ordinary,
                  "jobu.secret.resolved_payload_too_large");

    // Fill an additive field so the concrete serialized document is exactly at its limit.
    provider.values["token"] = bytes(std::string(1000U, 'x'));
    auto concrete            = prepare_payload_template(JobType::Cli, payload, provider);
    REQUIRE(concrete);
    auto& object                                               = std::get<JsonValue::Object>(payload.data);
    object["future"].data                                      = std::string{};
    std::get<JsonValue::Object>(concrete->data)["future"].data = std::string{};
    auto serialized                                            = serialize_json(*concrete);
    REQUIRE(serialized);
    object["future"].data = std::string(maximum_job_document_bytes - serialized->size(), 'x');
    REQUIRE(prepare_payload_template(JobType::Cli, payload, provider));
    std::get<std::string>(object["future"].data).push_back('x');
    check_failure(prepare_payload_template(JobType::Cli, payload, provider),
                  PayloadPreparationFailureKind::Ordinary,
                  "jobu.secret.resolved_payload_too_large");
}

TEST_CASE("Prepared CLI and HTTP size checks include injected metadata")
{
    Provider provider;
    provider.values["token"] = bytes(std::string(65536U, 'x'));
    auto http = json(R"({"url":"https://example.test/","headers":[{"name":"X","value":{"secret":"token"}}]})");
    check_failure(prepare_payload_template(JobType::Http, http, provider),
                  PayloadPreparationFailureKind::Ordinary,
                  "jobu.secret.resolved_payload_too_large");
    // 64 KiB minus the user header alone would fit, but leaves no room for JobU headers.
    provider.values["token"] = bytes(std::string(65535U, 'x'));
    check_failure(prepare_payload_template(JobType::Http, http, provider),
                  PayloadPreparationFailureKind::Ordinary,
                  "jobu.secret.resolved_payload_too_large");
    provider.values["token"] = bytes(std::string(65000U, 'x'));
    REQUIRE(prepare_payload_template(JobType::Http, http, provider));

    auto cli = json(
        R"({"command":"/bin/tool","arguments":[{"secret":"token"},{"secret":"token"},{"secret":"token"},{"secret":"token"}]})");
    // Serialized JSON fits, but argv/envp pointers, terminators and JobU metadata exceed the prepared bound.
    provider.values["token"] = bytes(std::string(65510U, 'x'));
    check_failure(prepare_payload_template(JobType::Cli, cli, provider),
                  PayloadPreparationFailureKind::Ordinary,
                  "jobu.secret.resolved_payload_too_large");
    provider.values["token"] = bytes(std::string(65000U, 'x'));
    REQUIRE(prepare_payload_template(JobType::Cli, cli, provider));
}
