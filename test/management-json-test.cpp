#include "management_json.hpp"

#include "attribute_registry.hpp"
#include "utc_timestamp.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::rpc;
using namespace std::chrono_literals;

namespace {

auto make_json(auto value) -> JsonValue
{
    JsonValue result;
    result.data = std::move(value);
    return result;
}

auto object(JsonValue& value) -> JsonValue::Object&
{
    return std::get<JsonValue::Object>(value.data);
}

auto parse_uuid(std::string_view text) -> Uuid
{
    auto parsed = Uuid::parse(text);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto parse_time(std::string_view text) -> UtcTimePoint
{
    auto parsed = parse_utc_timestamp(text);
    REQUIRE(parsed);
    return std::move(parsed).value();
}

auto queue_defaults(Duration timeout = 2500ms) -> AttributeSet
{
    return {
        {"job.timeout", {.data = timeout}},
    };
}

auto sample_queue(std::string name = "default") -> Queue
{
    return {
        .id                    = parse_uuid("00112233-4455-6677-8899-aabbccddeeff"),
        .name                  = std::move(name),
        .state                 = QueueState::Suspended,
        .weight                = 3,
        .concurrency_limit     = 4,
        .recovery_policy       = RecoveryPolicy::RetryInterrupted,
        .defaults              = queue_defaults(),
        .history_retention     = 2h,
        .runnable_wait_warning = 1500ms,
        .created_at            = parse_time("2026-07-21T08:00:00.000000Z"),
        .updated_at            = parse_time("2026-07-21T09:30:00.123456Z"),
        .deleted_at            = parse_time("2026-07-21T10:00:00.000000Z"),
    };
}

auto valid_queue_json(StandardAttributeRegistry const& registry) -> JsonValue
{
    auto encoded = queue_to_json(sample_queue(), registry);
    REQUIRE(encoded);
    return std::move(encoded).value();
}

class TrackingAttributeRegistry final : public AttributeRegistry {
public:
    [[nodiscard]] auto find(std::string_view name) const noexcept -> AttributeDefinition const* override
    {
        ++_find_calls;
        return _registry.find(name);
    }

    [[nodiscard]] auto validate(std::string_view name, AttributeValue const& value, AttributeScope scope) const
        -> Result<void, Error> override
    {
        return _registry.validate(name, value, scope);
    }

    [[nodiscard]] auto definitions() const -> std::span<AttributeDefinition const> override
    {
        return _registry.definitions();
    }

    [[nodiscard]] auto find_calls() const noexcept -> std::size_t { return _find_calls; }

private:
    StandardAttributeRegistry _registry;
    mutable std::size_t       _find_calls{0};
};

template <typename T>
auto check_invalid_request(Result<T, Error> const& result) -> void
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::InvalidArgument);
    CHECK(result.error().code == "jobu.protocol.invalid_request");
    CHECK(result.error().detail.empty());
}

template <typename T>
auto check_invalid_response(Result<T, Error> const& result) -> void
{
    REQUIRE_FALSE(result);
    CHECK(result.error().category == ErrorCategory::InvalidArgument);
    CHECK(result.error().code == "jobu.protocol.invalid_response");
    CHECK(result.error().detail.empty());
}

auto check_defaults(AttributeSet const& defaults, Duration timeout = 2500ms) -> void
{
    REQUIRE(defaults.size() == 1U);
    REQUIRE(defaults.contains("job.timeout"));
    CHECK(std::get<Duration>(defaults.at("job.timeout").data) == timeout);
}

} // anonymous namespace

TEST_CASE("Queue JSON uses the stable shape and round trips owning values", "[jobu][management][json][queue]")
{
    StandardAttributeRegistry registry;
    auto                      encoded = valid_queue_json(registry);
    auto const&               wire    = encoded.as_object();

    REQUIRE(wire.size() == 12U);
    CHECK(wire.at("id").as_string() == "00112233-4455-6677-8899-aabbccddeeff");
    CHECK(wire.at("name").as_string() == "default");
    CHECK(wire.at("state").as_string() == "suspended");
    CHECK(wire.at("weight").as_uint() == 3U);
    CHECK(wire.at("concurrency_limit").as_uint() == 4U);
    CHECK(wire.at("recovery_policy").as_string() == "retry_interrupted");
    CHECK(wire.at("defaults").as_object().at("job.timeout").as_int() == 2500);
    CHECK(wire.at("history_retention_seconds").as_int() == 7200);
    CHECK(wire.at("runnable_wait_warning_ms").as_int() == 1500);
    CHECK(wire.at("created_at").as_string() == "2026-07-21T08:00:00.000000Z");
    CHECK(wire.at("updated_at").as_string() == "2026-07-21T09:30:00.123456Z");
    CHECK(wire.at("deleted_at").as_string() == "2026-07-21T10:00:00.000000Z");

    auto text = serialize_json(encoded);
    REQUIRE(text);
    auto parsed = parse_json(*text);
    REQUIRE(parsed);
    object(*parsed).emplace("future", make_json(true));
    auto decoded = queue_from_json(*parsed, registry);
    REQUIRE(decoded);
    CHECK(decoded->id == parse_uuid("00112233-4455-6677-8899-aabbccddeeff"));
    CHECK(decoded->name == "default");
    CHECK(decoded->state == QueueState::Suspended);
    CHECK(decoded->weight == 3U);
    CHECK(decoded->concurrency_limit == 4U);
    CHECK(decoded->recovery_policy == RecoveryPolicy::RetryInterrupted);
    check_defaults(decoded->defaults);
    REQUIRE(decoded->history_retention);
    CHECK(*decoded->history_retention == 2h);
    CHECK(decoded->runnable_wait_warning == 1500ms);
    REQUIRE(decoded->deleted_at);

    object(*parsed).at("name") = make_json(std::string{"changed"});
    CHECK(decoded->name == "default");

    auto without_optionals = sample_queue("minimal");
    without_optionals.history_retention.reset();
    without_optionals.deleted_at.reset();
    auto minimal = queue_to_json(without_optionals, registry);
    REQUIRE(minimal);
    CHECK(minimal->as_object().at("history_retention_seconds").is_null());
    CHECK(minimal->as_object().at("deleted_at").is_null());
    auto minimal_decoded = queue_from_json(*minimal, registry);
    REQUIRE(minimal_decoded);
    CHECK_FALSE(minimal_decoded->history_retention);
    CHECK_FALSE(minimal_decoded->deleted_at);
}

TEST_CASE("Queue JSON covers every stable enum spelling", "[jobu][management][json][queue][enum]")
{
    StandardAttributeRegistry registry;
    auto                      queue = sample_queue();

    auto const states = {
        std::pair{QueueState::Active,     "active"    },
        std::pair{QueueState::Suspending, "suspending"},
        std::pair{QueueState::Suspended,  "suspended" },
        std::pair{QueueState::Deleted,    "deleted"   },
    };
    for (auto const& [state, text] : states) {
        queue.state  = state;
        auto encoded = queue_to_json(queue, registry);
        REQUIRE(encoded);
        CHECK(encoded->as_object().at("state").as_string() == text);
        auto decoded = queue_from_json(*encoded, registry);
        REQUIRE(decoded);
        CHECK(decoded->state == state);
    }

    auto const policies = {
        std::pair{RecoveryPolicy::FailInterrupted,  "fail_interrupted" },
        std::pair{RecoveryPolicy::RetryInterrupted, "retry_interrupted"},
    };
    for (auto const& [policy, text] : policies) {
        queue.recovery_policy = policy;
        auto encoded          = queue_to_json(queue, registry);
        REQUIRE(encoded);
        CHECK(encoded->as_object().at("recovery_policy").as_string() == text);
        auto decoded = queue_from_json(*encoded, registry);
        REQUIRE(decoded);
        CHECK(decoded->recovery_policy == policy);
    }
}

TEST_CASE("Queue response decoding rejects invalid known fields", "[jobu][management][json][queue][invalid]")
{
    StandardAttributeRegistry registry;

    check_invalid_response(queue_from_json(make_json(JsonValue::Array{}), registry));

    auto missing = valid_queue_json(registry);
    object(missing).erase("created_at");
    check_invalid_response(queue_from_json(missing, registry));

    auto wrong_name               = valid_queue_json(registry);
    object(wrong_name).at("name") = make_json(std::uint64_t{1});
    check_invalid_response(queue_from_json(wrong_name, registry));

    auto bad_id             = valid_queue_json(registry);
    object(bad_id).at("id") = make_json(std::string{"not-a-uuid"});
    check_invalid_response(queue_from_json(bad_id, registry));

    auto noncanonical_id             = valid_queue_json(registry);
    object(noncanonical_id).at("id") = make_json(std::string{"00112233-4455-6677-8899-AABBCCDDEEFF"});
    check_invalid_response(queue_from_json(noncanonical_id, registry));

    auto bad_state                = valid_queue_json(registry);
    object(bad_state).at("state") = make_json(std::string{"ACTIVE"});
    check_invalid_response(queue_from_json(bad_state, registry));

    auto zero_weight                 = valid_queue_json(registry);
    object(zero_weight).at("weight") = make_json(std::uint64_t{0});
    check_invalid_response(queue_from_json(zero_weight, registry));

    auto weight_overflow = valid_queue_json(registry);
    object(weight_overflow).at("weight") =
        make_json(static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U);
    check_invalid_response(queue_from_json(weight_overflow, registry));

    auto bad_recovery                          = valid_queue_json(registry);
    object(bad_recovery).at("recovery_policy") = make_json(std::string{"retry"});
    check_invalid_response(queue_from_json(bad_recovery, registry));

    auto bad_defaults                   = valid_queue_json(registry);
    object(bad_defaults).at("defaults") = make_json(JsonValue::Object{
        {"unknown", make_json(true)}
    });
    check_invalid_response(queue_from_json(bad_defaults, registry));

    auto negative_retention                                    = valid_queue_json(registry);
    object(negative_retention).at("history_retention_seconds") = make_json(std::int64_t{-1});
    check_invalid_response(queue_from_json(negative_retention, registry));

    auto negative_warning                                   = valid_queue_json(registry);
    object(negative_warning).at("runnable_wait_warning_ms") = make_json(std::int64_t{-1});
    check_invalid_response(queue_from_json(negative_warning, registry));

    auto bad_time                     = valid_queue_json(registry);
    object(bad_time).at("updated_at") = make_json(std::string{"2026-07-21T09:30:00+03:00"});
    check_invalid_response(queue_from_json(bad_time, registry));

    auto bad_deleted                     = valid_queue_json(registry);
    object(bad_deleted).at("deleted_at") = make_json(false);
    check_invalid_response(queue_from_json(bad_deleted, registry));
}

TEST_CASE("Queue response encoding rejects invalid typed values", "[jobu][management][json][queue][encode]")
{
    StandardAttributeRegistry registry;

    auto invalid_state  = sample_queue();
    invalid_state.state = static_cast<QueueState>(255);
    check_invalid_response(queue_to_json(invalid_state, registry));

    auto invalid_defaults     = sample_queue();
    invalid_defaults.defaults = queue_defaults(1ns);
    check_invalid_response(queue_to_json(invalid_defaults, registry));

    auto invalid_weight   = sample_queue();
    invalid_weight.weight = 0;
    check_invalid_response(queue_to_json(invalid_weight, registry));

    auto invalid_retention              = sample_queue();
    invalid_retention.history_retention = -1s;
    check_invalid_response(queue_to_json(invalid_retention, registry));

    auto invalid_utf8 = sample_queue(std::string{"\xc3\x28", 2U});
    check_invalid_response(queue_to_json(invalid_utf8, registry));
}

TEST_CASE("Queue page JSON round trips and is forward compatible", "[jobu][management][json][queue][page]")
{
    StandardAttributeRegistry registry;
    auto const                next = parse_uuid("10112233-4455-6677-8899-aabbccddeeff");
    auto                      page = QueuePage{
        .items         = {sample_queue("first"), sample_queue("second")},
        .next_after_id = next,
    };
    page.items[1].id = next;

    auto encoded = queue_page_to_json(page, registry);
    REQUIRE(encoded);
    REQUIRE(encoded->as_object().at("items").as_array().size() == 2U);
    CHECK(encoded->as_object().at("next_after_id").as_string() == next.to_string());
    object(*encoded).emplace("future_page", make_json(true));
    object(std::get<JsonValue::Array>(object(*encoded).at("items").data)[0]).emplace("future_queue", make_json(true));

    auto decoded = queue_page_from_json(*encoded, registry);
    REQUIRE(decoded);
    REQUIRE(decoded->items.size() == 2U);
    CHECK(decoded->items[0].name == "first");
    CHECK(decoded->items[1].name == "second");
    REQUIRE(decoded->next_after_id);
    CHECK(*decoded->next_after_id == next);

    auto empty      = QueuePage{};
    auto empty_json = queue_page_to_json(empty, registry);
    REQUIRE(empty_json);
    CHECK(empty_json->as_object().at("items").as_array().empty());
    CHECK(empty_json->as_object().at("next_after_id").is_null());
    REQUIRE(queue_page_from_json(*empty_json, registry));
}

TEST_CASE("Queue page decoding rejects invalid known fields", "[jobu][management][json][queue][page][invalid]")
{
    StandardAttributeRegistry registry;
    auto                      encoded = queue_page_to_json({.items = {sample_queue()}}, registry);
    REQUIRE(encoded);

    auto missing = *encoded;
    object(missing).erase("next_after_id");
    check_invalid_response(queue_page_from_json(missing, registry));

    auto wrong_items                = *encoded;
    object(wrong_items).at("items") = make_json(JsonValue::Object{});
    check_invalid_response(queue_page_from_json(wrong_items, registry));

    auto bad_item                                                    = *encoded;
    std::get<JsonValue::Array>(object(bad_item).at("items").data)[0] = make_json(false);
    check_invalid_response(queue_page_from_json(bad_item, registry));

    auto bad_next                        = *encoded;
    object(bad_next).at("next_after_id") = make_json(std::string{"bad"});
    check_invalid_response(queue_page_from_json(bad_next, registry));

    auto mismatched_cursor                        = *encoded;
    object(mismatched_cursor).at("next_after_id") = make_json(std::string{"20112233-4455-6677-8899-aabbccddeeff"});
    check_invalid_response(queue_page_from_json(mismatched_cursor, registry));

    auto out_of_order = QueuePage{
        .items = {sample_queue("first"), sample_queue("second")}
    };
    check_invalid_response(queue_page_to_json(out_of_order, registry));

    auto oversized = QueuePage{};
    oversized.items.resize(201U, sample_queue());
    check_invalid_response(queue_page_to_json(oversized, registry));

    auto oversized_items   = JsonValue::Array(201U, valid_queue_json(registry));
    auto tracking_registry = TrackingAttributeRegistry{};
    check_invalid_response(queue_page_from_json(make_json(JsonValue::Object{
                                                    {"items",         make_json(std::move(oversized_items))},
                                                    {"next_after_id", make_json(JsonNull{})                },
    }),
                                                tracking_registry));
    CHECK(tracking_registry.find_calls() == 0U);
}

TEST_CASE("Queue selector JSON uses one shared strict shape", "[jobu][management][json][queue][selector]")
{
    auto const id      = parse_uuid("00112233-4455-6677-8899-aabbccddeeff");
    auto       id_json = queue_selector_to_json(QueueSelector{id});
    REQUIRE(id_json);
    REQUIRE(id_json->as_object().size() == 1U);
    CHECK(id_json->as_object().at("queue_id").as_string() == id.to_string());
    auto id_selector = queue_selector_from_json(*id_json);
    REQUIRE(id_selector);
    REQUIRE(std::holds_alternative<Uuid>(*id_selector));
    CHECK(std::get<Uuid>(*id_selector) == id);

    auto name_json = queue_selector_to_json(QueueSelector{std::string{"default"}});
    REQUIRE(name_json);
    REQUIRE(name_json->as_object().size() == 1U);
    CHECK(name_json->as_object().at("queue_name").as_string() == "default");
    auto name_selector = queue_selector_from_json(*name_json);
    REQUIRE(name_selector);
    REQUIRE(std::holds_alternative<std::string>(*name_selector));
    CHECK(std::get<std::string>(*name_selector) == "default");

    check_invalid_request(queue_selector_from_json(make_json(JsonNull{})));
    check_invalid_request(queue_selector_from_json(make_json(JsonValue::Object{})));
    check_invalid_request(queue_selector_from_json(make_json(JsonValue::Object{
        {"queue_id",   make_json(id.to_string())        },
        {"queue_name", make_json(std::string{"default"})},
    })));
    check_invalid_request(queue_selector_from_json(make_json(JsonValue::Object{
        {"queue_name", make_json(std::string{"default"})},
        {"unknown",    make_json(true)                  },
    })));
    check_invalid_request(queue_selector_from_json(make_json(JsonValue::Object{
        {"queue_id", make_json(std::string{"bad"})},
    })));
    check_invalid_request(queue_selector_from_json(make_json(JsonValue::Object{
        {"queue_id", make_json(std::string{"00112233-4455-6677-8899-AABBCCDDEEFF"})},
    })));
    check_invalid_request(queue_selector_from_json(make_json(JsonValue::Object{
        {"queue_name", make_json(std::uint64_t{1})},
    })));
    check_invalid_request(queue_selector_to_json(QueueSelector{
        std::string{"\xc3\x28", 2U}
    }));
}

TEST_CASE("Create queue request JSON round trips full and defaulted forms", "[jobu][management][json][queue][create]")
{
    StandardAttributeRegistry registry;
    auto                      request = CreateQueueRequest{
        .name                  = "default",
        .weight                = 3,
        .concurrency_limit     = 4,
        .recovery_policy       = RecoveryPolicy::RetryInterrupted,
        .defaults              = queue_defaults(),
        .history_retention     = 2h,
        .runnable_wait_warning = 1500ms,
        .idempotency_key       = "create-default",
    };

    auto encoded = create_queue_request_to_json(request, registry);
    REQUIRE(encoded);
    auto const& wire = encoded->as_object();
    CHECK(wire.at("name").as_string() == "default");
    CHECK(wire.at("weight").as_uint() == 3U);
    CHECK(wire.at("concurrency_limit").as_uint() == 4U);
    CHECK(wire.at("recovery_policy").as_string() == "retry_interrupted");
    CHECK(wire.at("defaults").as_object().at("job.timeout").as_int() == 2500);
    CHECK(wire.at("history_retention_seconds").as_int() == 7200);
    CHECK(wire.at("runnable_wait_warning_ms").as_int() == 1500);
    CHECK(wire.at("idempotency_key").as_string() == "create-default");

    auto decoded = create_queue_request_from_json(*encoded, registry);
    REQUIRE(decoded);
    CHECK(decoded->name == request.name);
    CHECK(decoded->weight == request.weight);
    CHECK(decoded->concurrency_limit == request.concurrency_limit);
    CHECK(decoded->recovery_policy == request.recovery_policy);
    check_defaults(decoded->defaults);
    CHECK(decoded->history_retention == request.history_retention);
    CHECK(decoded->runnable_wait_warning == request.runnable_wait_warning);
    CHECK(decoded->idempotency_key == request.idempotency_key);

    auto minimal = create_queue_request_from_json(make_json(JsonValue::Object{
                                                      {"name", make_json(std::string{"minimal"})}
    }),
                                                  registry);
    REQUIRE(minimal);
    CHECK(minimal->weight == 1U);
    CHECK(minimal->concurrency_limit == 1U);
    CHECK(minimal->recovery_policy == RecoveryPolicy::FailInterrupted);
    CHECK(minimal->defaults.empty());
    CHECK_FALSE(minimal->history_retention);
    CHECK(minimal->runnable_wait_warning == 10000ms);
    CHECK_FALSE(minimal->idempotency_key);

    auto inherited = request;
    inherited.history_retention.reset();
    inherited.idempotency_key.reset();
    auto inherited_json = create_queue_request_to_json(inherited, registry);
    REQUIRE(inherited_json);
    CHECK(inherited_json->as_object().at("history_retention_seconds").is_null());
    CHECK_FALSE(inherited_json->as_object().contains("idempotency_key"));
}

TEST_CASE("Create queue request decoding is strict and range checked",
          "[jobu][management][json][queue][create][invalid]")
{
    StandardAttributeRegistry registry;
    auto                      valid = make_json(JsonValue::Object{
        {"name", make_json(std::string{"default"})}
    });

    check_invalid_request(create_queue_request_from_json(make_json(JsonValue::Array{}), registry));
    check_invalid_request(create_queue_request_from_json(make_json(JsonValue::Object{}), registry));

    auto unknown = valid;
    object(unknown).emplace("future", make_json(true));
    check_invalid_request(create_queue_request_from_json(unknown, registry));

    auto wrong_name               = valid;
    object(wrong_name).at("name") = make_json(JsonNull{});
    check_invalid_request(create_queue_request_from_json(wrong_name, registry));

    auto negative_weight = valid;
    object(negative_weight).emplace("weight", make_json(std::int64_t{-1}));
    check_invalid_request(create_queue_request_from_json(negative_weight, registry));

    auto overflow_weight = valid;
    object(overflow_weight)
        .emplace("weight", make_json(static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U));
    check_invalid_request(create_queue_request_from_json(overflow_weight, registry));

    auto bad_recovery = valid;
    object(bad_recovery).emplace("recovery_policy", make_json(std::string{"FAIL_INTERRUPTED"}));
    check_invalid_request(create_queue_request_from_json(bad_recovery, registry));

    auto bad_defaults = valid;
    object(bad_defaults)
        .emplace("defaults",
                 make_json(JsonValue::Object{
                     {"missing", make_json(true)}
    }));
    check_invalid_request(create_queue_request_from_json(bad_defaults, registry));

    auto bad_warning = valid;
    object(bad_warning).emplace("runnable_wait_warning_ms", make_json(std::string{"1000"}));
    check_invalid_request(create_queue_request_from_json(bad_warning, registry));

    auto null_key = valid;
    object(null_key).emplace("idempotency_key", make_json(JsonNull{}));
    check_invalid_request(create_queue_request_from_json(null_key, registry));

    auto invalid_enum            = CreateQueueRequest{.name = "default"};
    invalid_enum.recovery_policy = static_cast<RecoveryPolicy>(255);
    check_invalid_request(create_queue_request_to_json(invalid_enum, registry));
}

TEST_CASE("Queue list request JSON round trips filters and defaults", "[jobu][management][json][queue][list]")
{
    auto const after   = parse_uuid("00112233-4455-6677-8899-aabbccddeeff");
    auto       request = QueueListRequest{
        .include_deleted = true,
        .state           = QueueState::Deleted,
        .page            = {.limit = 25, .after_id = after},
    };

    auto encoded = queue_list_request_to_json(request);
    REQUIRE(encoded);
    CHECK(encoded->as_object().at("include_deleted").as_bool());
    CHECK(encoded->as_object().at("state").as_string() == "deleted");
    CHECK(encoded->as_object().at("limit").as_uint() == 25U);
    CHECK(encoded->as_object().at("after_id").as_string() == after.to_string());

    auto decoded = queue_list_request_from_json(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->include_deleted);
    CHECK(decoded->state == QueueState::Deleted);
    CHECK(decoded->page.limit == 25U);
    CHECK(decoded->page.after_id == after);

    auto defaults = queue_list_request_from_json(make_json(JsonValue::Object{}));
    REQUIRE(defaults);
    CHECK_FALSE(defaults->include_deleted);
    CHECK_FALSE(defaults->state);
    CHECK(defaults->page.limit == 100U);
    CHECK_FALSE(defaults->page.after_id);
}

TEST_CASE("Queue list request decoding rejects unknown and malformed values",
          "[jobu][management][json][queue][list][invalid]")
{
    check_invalid_request(queue_list_request_from_json(make_json(JsonNull{})));
    check_invalid_request(queue_list_request_from_json(make_json(JsonValue::Object{
        {"future", make_json(true)}
    })));
    check_invalid_request(queue_list_request_from_json(make_json(JsonValue::Object{
        {"include_deleted", make_json(std::uint64_t{1})}
    })));
    check_invalid_request(queue_list_request_from_json(make_json(JsonValue::Object{
        {"state", make_json(std::string{"ACTIVE"})}
    })));
    check_invalid_request(queue_list_request_from_json(make_json(JsonValue::Object{
        {"limit", make_json(std::int64_t{-1})}
    })));
    check_invalid_request(queue_list_request_from_json(make_json(JsonValue::Object{
        {"limit", make_json(std::uint64_t{0})}
    })));
    check_invalid_request(queue_list_request_from_json(make_json(JsonValue::Object{
        {"limit", make_json(std::uint64_t{201})}
    })));
    check_invalid_request(queue_list_request_from_json(make_json(JsonValue::Object{
        {"after_id", make_json(std::string{"not-a-uuid"})}
    })));

    auto invalid_state = QueueListRequest{.state = static_cast<QueueState>(255)};
    check_invalid_request(queue_list_request_to_json(invalid_state));

    auto invalid_limit       = QueueListRequest{};
    invalid_limit.page.limit = 201U;
    check_invalid_request(queue_list_request_to_json(invalid_limit));
}

TEST_CASE("Update queue request JSON preserves omitted and null retention", "[jobu][management][json][queue][update]")
{
    StandardAttributeRegistry registry;
    auto const                id      = parse_uuid("00112233-4455-6677-8899-aabbccddeeff");
    auto                      request = UpdateQueueRequest{
        .queue                 = QueueSelector{id},
        .name                  = "renamed",
        .weight                = 5,
        .concurrency_limit     = 6,
        .recovery_policy       = RecoveryPolicy::RetryInterrupted,
        .defaults              = queue_defaults(3s),
        .history_retention     = std::optional<std::chrono::seconds>{4h},
        .runnable_wait_warning = 2500ms,
    };

    auto encoded = update_queue_request_to_json(request, registry);
    REQUIRE(encoded);
    auto const& wire = encoded->as_object();
    CHECK(wire.at("queue_id").as_string() == id.to_string());
    CHECK(wire.at("name").as_string() == "renamed");
    CHECK(wire.at("weight").as_uint() == 5U);
    CHECK(wire.at("concurrency_limit").as_uint() == 6U);
    CHECK(wire.at("recovery_policy").as_string() == "retry_interrupted");
    CHECK(wire.at("defaults").as_object().at("job.timeout").as_int() == 3000);
    CHECK(wire.at("history_retention_seconds").as_int() == 14400);
    CHECK(wire.at("runnable_wait_warning_ms").as_int() == 2500);

    auto decoded = update_queue_request_from_json(*encoded, registry);
    REQUIRE(decoded);
    REQUIRE(std::holds_alternative<Uuid>(decoded->queue));
    CHECK(std::get<Uuid>(decoded->queue) == id);
    CHECK(decoded->name == request.name);
    CHECK(decoded->weight == request.weight);
    CHECK(decoded->concurrency_limit == request.concurrency_limit);
    CHECK(decoded->recovery_policy == request.recovery_policy);
    REQUIRE(decoded->defaults);
    check_defaults(*decoded->defaults, 3s);
    REQUIRE(decoded->history_retention);
    REQUIRE(*decoded->history_retention);
    CHECK(**decoded->history_retention == 4h);
    CHECK(decoded->runnable_wait_warning == request.runnable_wait_warning);

    auto clear_retention = make_json(JsonValue::Object{
        {"history_retention_seconds", make_json(JsonNull{})            },
        {"queue_name",                make_json(std::string{"default"})},
    });
    auto cleared         = update_queue_request_from_json(clear_retention, registry);
    REQUIRE(cleared);
    REQUIRE(cleared->history_retention);
    CHECK_FALSE(*cleared->history_retention);
    auto cleared_json = update_queue_request_to_json(*cleared, registry);
    REQUIRE(cleared_json);
    CHECK(cleared_json->as_object().at("history_retention_seconds").is_null());

    auto omitted = update_queue_request_from_json(make_json(JsonValue::Object{
                                                      {"queue_name", make_json(std::string{"default"})},
                                                      {"weight",     make_json(std::uint64_t{2})      },
    }),
                                                  registry);
    REQUIRE(omitted);
    CHECK_FALSE(omitted->history_retention);
}

TEST_CASE("Update queue request decoding enforces its strict structure",
          "[jobu][management][json][queue][update][invalid]")
{
    StandardAttributeRegistry registry;
    auto const                id = parse_uuid("00112233-4455-6677-8899-aabbccddeeff");

    check_invalid_request(update_queue_request_from_json(make_json(JsonValue::Object{
                                                             {"queue_id", make_json(id.to_string())},
    }),
                                                         registry));
    check_invalid_request(update_queue_request_from_json(make_json(JsonValue::Object{
                                                             {"queue_id",   make_json(id.to_string())        },
                                                             {"queue_name", make_json(std::string{"default"})},
                                                             {"weight",     make_json(std::uint64_t{2})      },
    }),
                                                         registry));
    check_invalid_request(update_queue_request_from_json(make_json(JsonValue::Object{
                                                             {"queue_name", make_json(std::string{"default"})},
                                                             {"unknown",    make_json(true)                  },
                                                             {"weight",     make_json(std::uint64_t{2})      },
    }),
                                                         registry));
    check_invalid_request(update_queue_request_from_json(make_json(JsonValue::Object{
                                                             {"name",       make_json(JsonNull{})            },
                                                             {"queue_name", make_json(std::string{"default"})},
    }),
                                                         registry));
    check_invalid_request(update_queue_request_from_json(make_json(JsonValue::Object{
                                                             {"defaults",   make_json(JsonValue::Array{})    },
                                                             {"queue_name", make_json(std::string{"default"})},
    }),
                                                         registry));

    auto empty = UpdateQueueRequest{.queue = QueueSelector{std::string{"default"}}};
    check_invalid_request(update_queue_request_to_json(empty, registry));

    auto invalid_enum = UpdateQueueRequest{
        .queue           = QueueSelector{std::string{"default"}},
        .recovery_policy = static_cast<RecoveryPolicy>(255),
    };
    check_invalid_request(update_queue_request_to_json(invalid_enum, registry));
}
