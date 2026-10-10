#include "configuration_priv.hpp"

#include "attribute.hpp"
#include "attribute_registry.hpp"
#include "json.hpp"
#include "utils.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>

namespace {

auto read_sample() -> std::string
{
    std::ifstream input{JOBUD_CONFIG_EXAMPLE};
    REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

struct SampleDirectives {
    std::map<std::string, std::size_t, std::less<>> occurrences;
    std::string                                     attribute_examples;
};

auto sample_directives(std::string_view text) -> SampleDirectives
{
    // Collect actual and commented directive lines. Their values are subsequently
    // decoded by the production INI/configuration parser, not by this inventory scan.
    SampleDirectives result;
    while (!text.empty()) {
        auto const newline = text.find('\n');
        auto       line    = jb::core::trim_ascii_whitespace(text.substr(0, newline));
        text               = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1U);

        if (line.starts_with('#')) {
            line = jb::core::trim_ascii_whitespace(line.substr(1U));
        }
        auto const separator = line.find('=');
        if (separator == std::string_view::npos) {
            continue;
        }

        auto const key = jb::core::trim_ascii_whitespace(line.substr(0, separator));
        if (key.find(' ') != std::string_view::npos) {
            continue;
        }
        ++result.occurrences[std::string{key}];
        if (key.starts_with("defaults.")) {
            result.attribute_examples.append(line);
            result.attribute_examples.push_back('\n');
        }
    }
    return result;
}

} // namespace

TEST_CASE("installed configuration sample covers the accepted directive inventory", "[jobud][configuration][install]")
{
    auto const sample     = read_sample();
    auto const directives = sample_directives(sample);
    auto const keys       = jb::jobud::detail::fixed_configuration_keys();
    auto       expected   = std::size_t{0};

    for (auto const key : keys) {
        CAPTURE(key);
        REQUIRE(directives.occurrences.contains(key));
        CHECK(directives.occurrences.at(std::string{key}) == 1U);
        ++expected;
    }

    jb::jobu::StandardAttributeRegistry registry;
    for (auto const& definition : registry.definitions()) {
        if (!definition.scopes.test(jb::jobu::AttributeScope::DaemonDefault)) {
            continue;
        }
        auto const key = "defaults." + definition.name;
        CAPTURE(key);
        REQUIRE(directives.occurrences.contains(key));
        CHECK(directives.occurrences.at(key) == 1U);
        ++expected;
    }
    CHECK(directives.occurrences.size() == expected);
    CHECK(sample.size() <= 65'536U);
}

TEST_CASE("configuration sample preserves safe omissions and validates attribute examples",
          "[jobud][configuration][install]")
{
    auto const sample = read_sample();
    auto       active = jb::jobud::detail::parse_configuration_text(sample);
    REQUIRE(active);
    CHECK(active->allow_root_daemon == false);
    CHECK(active->allow_root_cli == false);
    CHECK_FALSE(active->run_as_user);
    CHECK_FALSE(active->run_as_group);
    CHECK_FALSE(active->socket_owner);
    CHECK_FALSE(active->socket_group);
    CHECK_FALSE(active->http_proxy);
    CHECK_FALSE(active->http_ca_bundle);
    CHECK(active->daemon_defaults.empty());

    // Enable only the documented attribute examples together. Cross-field validation
    // and the INI-to-JSON quote boundary must succeed through the existing parser.
    auto const directives = sample_directives(sample);
    auto       attributes = jb::jobud::detail::parse_configuration_text(directives.attribute_examples);
    REQUIRE(attributes);

    jb::jobu::StandardAttributeRegistry registry;
    jb::jobu::AttributeSet              built_ins;
    for (auto const& definition : registry.definitions()) {
        if (definition.scopes.test(jb::jobu::AttributeScope::DaemonDefault)) {
            built_ins.emplace(definition.name, definition.built_in_default);
        }
    }
    auto actual =
        jb::jobu::attribute_set_to_json(attributes->daemon_defaults, registry, jb::jobu::AttributeScope::DaemonDefault);
    auto expected = jb::jobu::attribute_set_to_json(built_ins, registry, jb::jobu::AttributeScope::DaemonDefault);
    REQUIRE(actual);
    REQUIRE(expected);
    auto actual_text   = jb::core::serialize_json(*actual);
    auto expected_text = jb::core::serialize_json(*expected);
    REQUIRE(actual_text);
    REQUIRE(expected_text);
    CHECK(*actual_text == *expected_text);
}
