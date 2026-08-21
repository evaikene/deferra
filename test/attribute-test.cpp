#include "attribute.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono> // IWYU pragma: keep for std::chrono_literals
#include <string_view>
#include <variant>

using namespace jb::core;
using namespace jb::jobu;
using namespace std::chrono_literals;

namespace {

auto attribute_error(std::string code) -> Result<void, Error>
{
    return Result<void, Error>::failure({
        .category = ErrorCategory::InvalidArgument,
        .code     = std::move(code),
        .message  = "Invalid test attribute",
    });
}

class TestAttributeRegistry final : public AttributeRegistry {
public:
    [[nodiscard]] auto find(std::string_view name) const noexcept -> AttributeDefinition const* override
    {
        for (auto const& definition : _definitions) {
            if (definition.name == name) {
                return &definition;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto validate(std::string_view name, AttributeValue const& value, AttributeScope scope) const
        -> Result<void, Error> override
    {
        if (!is_valid_attribute_name(name)) {
            return attribute_error("jobu.attribute.invalid_name");
        }
        auto const* definition = find(name);
        if (definition == nullptr) {
            return attribute_error("jobu.attribute.unknown");
        }
        if (!definition->scopes.test(scope)) {
            return attribute_error("jobu.attribute.wrong_scope");
        }
        if (!has_type(value, definition->type)) {
            return attribute_error("jobu.attribute.wrong_type");
        }
        if (name == "job.timeout" && std::get<std::int64_t>(value.data) <= 0) {
            return attribute_error("jobu.attribute.constraint");
        }
        return Result<void, Error>::success();
    }

    [[nodiscard]] auto definitions() const -> std::span<AttributeDefinition const> override { return _definitions; }

private:
    static auto has_type(AttributeValue const& value, AttributeType type) -> bool
    {
        switch (type) {
            case AttributeType::Boolean:
                return std::holds_alternative<bool>(value.data);
            case AttributeType::Integer:
                return std::holds_alternative<std::int64_t>(value.data);
            case AttributeType::Number:
                return std::holds_alternative<double>(value.data);
            case AttributeType::String:
                return std::holds_alternative<std::string>(value.data);
            case AttributeType::Duration:
                return std::holds_alternative<Duration>(value.data);
            case AttributeType::Bytes:
                return std::holds_alternative<ByteBuffer>(value.data);
            case AttributeType::List:
                return std::holds_alternative<AttributeValue::List>(value.data);
            case AttributeType::Map:
                return std::holds_alternative<AttributeValue::Map>(value.data);
        }
        return false;
    }

    std::array<AttributeDefinition, 2> _definitions{
        {
         {
                .name = "job.timeout",
                .type = AttributeType::Integer,
                .scopes =
                    AttributeScopes{AttributeScope::DaemonDefault, AttributeScope::QueueDefault, AttributeScope::Job},
                .built_in_default = {.data = std::int64_t{120}},
                .description      = "Job timeout in seconds",
            }, {
                .name             = "capture.enabled",
                .type             = AttributeType::Boolean,
                .scopes           = AttributeScopes{AttributeScope::Job},
                .built_in_default = {.data = true},
                .description      = "Capture runner output",
            }, }
    };
};

} // anonymous namespace

TEST_CASE("Attribute names use lower-case dotted segments", "[jobu][attribute]")
{
    CHECK(is_valid_attribute_name("job.timeout"));
    CHECK(is_valid_attribute_name("retry.max_attempts"));
    CHECK(is_valid_attribute_name("http.header_2"));

    CHECK_FALSE(is_valid_attribute_name(""));
    CHECK_FALSE(is_valid_attribute_name("Job.timeout"));
    CHECK_FALSE(is_valid_attribute_name("job..timeout"));
    CHECK_FALSE(is_valid_attribute_name("job.timeout."));
    CHECK_FALSE(is_valid_attribute_name("job.-timeout"));
}

TEST_CASE("AttributeValue supports every project-owned value type", "[jobu][attribute]")
{
    CHECK(std::holds_alternative<bool>(AttributeValue{.data = true}.data));
    CHECK(std::holds_alternative<std::int64_t>(AttributeValue{.data = std::int64_t{1}}.data));
    CHECK(std::holds_alternative<double>(AttributeValue{.data = 1.5}.data));
    CHECK(std::holds_alternative<std::string>(AttributeValue{.data = std::string{"value"}}.data));
    CHECK(std::holds_alternative<Duration>(AttributeValue{.data = 5s}.data));
    CHECK(std::holds_alternative<ByteBuffer>(AttributeValue{.data = ByteBuffer{}}.data));
    CHECK(std::holds_alternative<AttributeValue::List>(AttributeValue{.data = AttributeValue::List{}}.data));
    CHECK(std::holds_alternative<AttributeValue::Map>(AttributeValue{.data = AttributeValue::Map{}}.data));
}

TEST_CASE("Attribute registry returns stable validation errors", "[jobu][attribute]")
{
    TestAttributeRegistry registry;

    CHECK(registry.find("job.timeout") != nullptr);
    CHECK(registry.definitions().size() == 2);
    CHECK(registry.validate("job.timeout", {.data = std::int64_t{30}}, AttributeScope::Job));
    CHECK(registry.validate("missing.value", {.data = true}, AttributeScope::Job).error().code ==
          "jobu.attribute.unknown");
    CHECK(registry.validate("Job.timeout", {.data = std::int64_t{30}}, AttributeScope::Job).error().code ==
          "jobu.attribute.invalid_name");
    CHECK(registry.validate("capture.enabled", {.data = true}, AttributeScope::QueueDefault).error().code ==
          "jobu.attribute.wrong_scope");
    CHECK(registry.validate("job.timeout", {.data = true}, AttributeScope::Job).error().code ==
          "jobu.attribute.wrong_type");
    CHECK(registry.validate("job.timeout", {.data = std::int64_t{0}}, AttributeScope::Job).error().code ==
          "jobu.attribute.constraint");
}
