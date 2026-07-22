#include "management_json.hpp"

#include "attribute_registry.hpp"
#include "utc_timestamp.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace jb::jobu {

namespace {

template <typename T>
using ConversionResult = jb::core::Result<T, jb::core::Error>;

inline constexpr std::size_t maximum_page_size{200};

auto make_json(auto value) -> jb::rpc::JsonValue
{
    jb::rpc::JsonValue result;
    result.data = std::move(value);
    return result;
}

auto protocol_error(bool request) -> jb::core::Error
{
    return {
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = request ? "jobu.protocol.invalid_request" : "jobu.protocol.invalid_response",
        .message  = request ? "The JobU management request is invalid" : "The JobU management response is invalid",
    };
}

template <typename T>
auto invalid(bool request) -> ConversionResult<T>
{
    return ConversionResult<T>::failure(protocol_error(request));
}

auto checked_json(jb::rpc::JsonValue value, bool request) -> ConversionResult<jb::rpc::JsonValue>
{
    if (!jb::rpc::serialize_json(value)) {
        return invalid<jb::rpc::JsonValue>(request);
    }
    return ConversionResult<jb::rpc::JsonValue>::success(std::move(value));
}

auto find_member(jb::rpc::JsonValue::Object const& object, std::string_view name) -> jb::rpc::JsonValue const*
{
    auto const iterator = object.find(name);
    return iterator == object.end() ? nullptr : &iterator->second;
}

auto has_only_members(jb::rpc::JsonValue::Object const& object, std::initializer_list<std::string_view> allowed) -> bool
{
    for (auto const& [name, value] : object) {
        static_cast<void>(value);
        auto found = false;
        for (auto const candidate : allowed) {
            if (name == candidate) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

template <typename T>
requires(std::is_integral_v<T>&& std::is_unsigned_v<T>) auto decode_unsigned(jb::rpc::JsonValue const& value, T& result)
    -> bool
{
    auto decoded = std::uint64_t{};
    if (value.is_uint()) {
        decoded = value.as_uint();
    }
    else if (value.is_int() && value.as_int() >= 0) {
        decoded = static_cast<std::uint64_t>(value.as_int());
    }
    else {
        return false;
    }
    if (decoded > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
        return false;
    }
    result = static_cast<T>(decoded);
    return true;
}

template <typename T>
requires(std::is_integral_v<T>&& std::is_signed_v<T>) auto decode_signed(jb::rpc::JsonValue const& value, T& result)
    -> bool
{
    auto decoded = std::int64_t{};
    if (value.is_int()) {
        decoded = value.as_int();
    }
    else if (value.is_uint() &&
             value.as_uint() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        decoded = static_cast<std::int64_t>(value.as_uint());
    }
    else {
        return false;
    }
    if (decoded < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
        decoded > static_cast<std::int64_t>(std::numeric_limits<T>::max())) {
        return false;
    }
    result = static_cast<T>(decoded);
    return true;
}

auto queue_state_text(QueueState state) -> std::optional<std::string_view>
{
    switch (state) {
        case QueueState::Active:
            return "active";
        case QueueState::Suspending:
            return "suspending";
        case QueueState::Suspended:
            return "suspended";
        case QueueState::Deleted:
            return "deleted";
    }
    return std::nullopt;
}

auto queue_state_from_json(jb::rpc::JsonValue const& value, QueueState& state) -> bool
{
    if (!value.is_string()) {
        return false;
    }
    if (value.as_string() == "active") {
        state = QueueState::Active;
    }
    else if (value.as_string() == "suspending") {
        state = QueueState::Suspending;
    }
    else if (value.as_string() == "suspended") {
        state = QueueState::Suspended;
    }
    else if (value.as_string() == "deleted") {
        state = QueueState::Deleted;
    }
    else {
        return false;
    }
    return true;
}

auto recovery_policy_text(RecoveryPolicy policy) -> std::optional<std::string_view>
{
    switch (policy) {
        case RecoveryPolicy::FailInterrupted:
            return "fail_interrupted";
        case RecoveryPolicy::RetryInterrupted:
            return "retry_interrupted";
    }
    return std::nullopt;
}

auto recovery_policy_from_json(jb::rpc::JsonValue const& value, RecoveryPolicy& policy) -> bool
{
    if (!value.is_string()) {
        return false;
    }
    if (value.as_string() == "fail_interrupted") {
        policy = RecoveryPolicy::FailInterrupted;
    }
    else if (value.as_string() == "retry_interrupted") {
        policy = RecoveryPolicy::RetryInterrupted;
    }
    else {
        return false;
    }
    return true;
}

auto decode_uuid(jb::rpc::JsonValue const& value, jb::core::Uuid& result) -> bool
{
    if (!value.is_string()) {
        return false;
    }
    auto parsed = jb::core::Uuid::parse(value.as_string());
    if (!parsed || parsed->to_string() != value.as_string()) {
        return false;
    }
    result = std::move(parsed).value();
    return true;
}

auto decode_time(jb::rpc::JsonValue const& value, jb::core::UtcTimePoint& result) -> bool
{
    if (!value.is_string()) {
        return false;
    }
    auto parsed = parse_utc_timestamp(value.as_string());
    if (!parsed) {
        return false;
    }
    result = std::move(parsed).value();
    return true;
}

auto encode_time(jb::core::UtcTimePoint value) -> std::optional<jb::rpc::JsonValue>
{
    auto formatted = format_utc_timestamp(value);
    if (!formatted) {
        return std::nullopt;
    }
    return make_json(std::move(formatted).value());
}

auto add_selector_members(jb::rpc::JsonValue::Object& object,
                          QueueSelector const&        selector,
                          std::string                 id_name   = "queue_id",
                          std::string                 name_name = "queue_name") -> bool
{
    if (auto const* id = std::get_if<jb::core::Uuid>(&selector)) {
        object.emplace(std::move(id_name), make_json(id->to_string()));
        return true;
    }
    if (auto const* name = std::get_if<std::string>(&selector)) {
        object.emplace(std::move(name_name), make_json(*name));
        return true;
    }
    return false;
}

auto decode_selector(jb::rpc::JsonValue::Object const& object,
                     std::string_view                  id_name   = "queue_id",
                     std::string_view                  name_name = "queue_name") -> ConversionResult<QueueSelector>
{
    auto const* id   = find_member(object, id_name);
    auto const* name = find_member(object, name_name);
    if ((id == nullptr) == (name == nullptr)) {
        return invalid<QueueSelector>(true);
    }
    if (id != nullptr) {
        auto decoded = jb::core::Uuid{};
        if (!decode_uuid(*id, decoded)) {
            return invalid<QueueSelector>(true);
        }
        return ConversionResult<QueueSelector>::success(QueueSelector{decoded});
    }
    if (!name->is_string()) {
        return invalid<QueueSelector>(true);
    }
    return ConversionResult<QueueSelector>::success(QueueSelector{name->as_string()});
}

auto decode_nullable_seconds(jb::rpc::JsonValue const& value, std::optional<std::chrono::seconds>& result) -> bool
{
    if (value.is_null()) {
        result.reset();
        return true;
    }
    auto count = std::chrono::seconds::rep{};
    if (!decode_signed(value, count)) {
        return false;
    }
    result = std::chrono::seconds{count};
    return true;
}

auto encode_nullable_seconds(std::optional<std::chrono::seconds> value) -> jb::rpc::JsonValue
{
    if (!value) {
        return make_json(jb::rpc::JsonNull{});
    }
    return make_json(static_cast<std::int64_t>(value->count()));
}

auto has_queue_update(UpdateQueueRequest const& request) -> bool
{
    return request.name || request.weight || request.concurrency_limit || request.recovery_policy || request.defaults ||
           request.history_retention || request.runnable_wait_warning;
}

auto valid_queue_page(QueuePage const& page) -> bool
{
    if (page.items.size() > maximum_page_size) {
        return false;
    }
    for (auto index = std::size_t{1}; index < page.items.size(); ++index) {
        if (!(page.items[index - 1].id < page.items[index].id)) {
            return false;
        }
    }
    return !page.next_after_id || (!page.items.empty() && *page.next_after_id == page.items.back().id);
}

} // anonymous namespace

auto queue_to_json(Queue const& queue, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>
{
    auto const state    = queue_state_text(queue.state);
    auto const recovery = recovery_policy_text(queue.recovery_policy);
    auto       defaults = attribute_set_to_json(queue.defaults, registry, AttributeScope::QueueDefault);
    auto       created  = encode_time(queue.created_at);
    auto       updated  = encode_time(queue.updated_at);
    if (!state || !recovery || !defaults || !created || !updated || queue.weight == 0 || queue.concurrency_limit == 0 ||
        (queue.history_retention && *queue.history_retention < std::chrono::seconds::zero()) ||
        queue.runnable_wait_warning < std::chrono::milliseconds::zero()) {
        return invalid<jb::rpc::JsonValue>(false);
    }

    auto deleted = make_json(jb::rpc::JsonNull{});
    if (queue.deleted_at) {
        auto encoded = encode_time(*queue.deleted_at);
        if (!encoded) {
            return invalid<jb::rpc::JsonValue>(false);
        }
        deleted = std::move(*encoded);
    }

    return checked_json(
        make_json(jb::rpc::JsonValue::Object{
            {"concurrency_limit",         make_json(static_cast<std::uint64_t>(queue.concurrency_limit))           },
            {"created_at",                std::move(*created)                                                      },
            {"defaults",                  std::move(defaults).value()                                              },
            {"deleted_at",                std::move(deleted)                                                       },
            {"history_retention_seconds", encode_nullable_seconds(queue.history_retention)                         },
            {"id",                        make_json(queue.id.to_string())                                          },
            {"name",                      make_json(queue.name)                                                    },
            {"recovery_policy",           make_json(std::string{*recovery})                                        },
            {"runnable_wait_warning_ms",  make_json(static_cast<std::int64_t>(queue.runnable_wait_warning.count()))},
            {"state",                     make_json(std::string{*state})                                           },
            {"updated_at",                std::move(*updated)                                                      },
            {"weight",                    make_json(static_cast<std::uint64_t>(queue.weight))                      },
    }),
        false);
}

auto queue_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<Queue, jb::core::Error>
{
    if (!value.is_object()) {
        return invalid<Queue>(false);
    }
    auto const& object      = value.as_object();
    auto const* id          = find_member(object, "id");
    auto const* name        = find_member(object, "name");
    auto const* state       = find_member(object, "state");
    auto const* weight      = find_member(object, "weight");
    auto const* concurrency = find_member(object, "concurrency_limit");
    auto const* recovery    = find_member(object, "recovery_policy");
    auto const* defaults    = find_member(object, "defaults");
    auto const* retention   = find_member(object, "history_retention_seconds");
    auto const* warning     = find_member(object, "runnable_wait_warning_ms");
    auto const* created     = find_member(object, "created_at");
    auto const* updated     = find_member(object, "updated_at");
    auto const* deleted     = find_member(object, "deleted_at");
    if (!id || !name || !state || !weight || !concurrency || !recovery || !defaults || !retention || !warning ||
        !created || !updated || !deleted || !name->is_string()) {
        return invalid<Queue>(false);
    }

    auto result = Queue{};
    if (!decode_uuid(*id, result.id) || !queue_state_from_json(*state, result.state) ||
        !decode_unsigned(*weight, result.weight) || result.weight == 0 ||
        !decode_unsigned(*concurrency, result.concurrency_limit) || result.concurrency_limit == 0 ||
        !recovery_policy_from_json(*recovery, result.recovery_policy) ||
        !decode_nullable_seconds(*retention, result.history_retention) ||
        (result.history_retention && *result.history_retention < std::chrono::seconds::zero())) {
        return invalid<Queue>(false);
    }

    result.name           = name->as_string();
    auto decoded_defaults = attribute_set_from_json(*defaults, registry, AttributeScope::QueueDefault);
    if (!decoded_defaults || !decode_time(*created, result.created_at) || !decode_time(*updated, result.updated_at)) {
        return invalid<Queue>(false);
    }
    result.defaults = std::move(decoded_defaults).value();

    auto warning_count = std::chrono::milliseconds::rep{};
    if (!decode_signed(*warning, warning_count) || warning_count < 0) {
        return invalid<Queue>(false);
    }
    result.runnable_wait_warning = std::chrono::milliseconds{warning_count};

    if (deleted->is_null()) {
        result.deleted_at.reset();
    }
    else {
        auto deleted_time = jb::core::UtcTimePoint{};
        if (!decode_time(*deleted, deleted_time)) {
            return invalid<Queue>(false);
        }
        result.deleted_at = deleted_time;
    }
    return ConversionResult<Queue>::success(std::move(result));
}

auto queue_page_to_json(QueuePage const& page, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>
{
    if (!valid_queue_page(page)) {
        return invalid<jb::rpc::JsonValue>(false);
    }
    auto items = jb::rpc::JsonValue::Array{};
    items.reserve(page.items.size());
    for (auto const& queue : page.items) {
        auto encoded = queue_to_json(queue, registry);
        if (!encoded) {
            return invalid<jb::rpc::JsonValue>(false);
        }
        items.push_back(std::move(encoded).value());
    }

    auto next = make_json(jb::rpc::JsonNull{});
    if (page.next_after_id) {
        next = make_json(page.next_after_id->to_string());
    }
    return checked_json(make_json(jb::rpc::JsonValue::Object{
                            {"items",         make_json(std::move(items))},
                            {"next_after_id", std::move(next)            },
    }),
                        false);
}

auto queue_page_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<QueuePage, jb::core::Error>
{
    if (!value.is_object()) {
        return invalid<QueuePage>(false);
    }
    auto const* items = find_member(value.as_object(), "items");
    auto const* next  = find_member(value.as_object(), "next_after_id");
    if (!items || !items->is_array() || !next || items->as_array().size() > maximum_page_size) {
        return invalid<QueuePage>(false);
    }

    auto const& item_array = items->as_array();
    auto        result     = QueuePage{};
    result.items.reserve(item_array.size());
    for (auto const& item : item_array) {
        auto decoded = queue_from_json(item, registry);
        if (!decoded) {
            return invalid<QueuePage>(false);
        }
        result.items.push_back(std::move(decoded).value());
    }
    if (next->is_null()) {
        result.next_after_id.reset();
    }
    else {
        auto id = jb::core::Uuid{};
        if (!decode_uuid(*next, id)) {
            return invalid<QueuePage>(false);
        }
        result.next_after_id = id;
    }
    if (!valid_queue_page(result)) {
        return invalid<QueuePage>(false);
    }
    return ConversionResult<QueuePage>::success(std::move(result));
}

auto queue_selector_to_json(QueueSelector const& selector) -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>
{
    auto object = jb::rpc::JsonValue::Object{};
    if (!add_selector_members(object, selector)) {
        return invalid<jb::rpc::JsonValue>(true);
    }
    return checked_json(make_json(std::move(object)), true);
}

auto queue_selector_from_json(jb::rpc::JsonValue const& value) -> jb::core::Result<QueueSelector, jb::core::Error>
{
    if (!value.is_object() || !has_only_members(value.as_object(), {"queue_id", "queue_name"})) {
        return invalid<QueueSelector>(true);
    }
    return decode_selector(value.as_object());
}

auto create_queue_request_to_json(CreateQueueRequest const& request, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>
{
    auto const recovery = recovery_policy_text(request.recovery_policy);
    auto       defaults = attribute_set_to_json(request.defaults, registry, AttributeScope::QueueDefault);
    if (!recovery || !defaults) {
        return invalid<jb::rpc::JsonValue>(true);
    }
    auto object = jb::rpc::JsonValue::Object{
        {"concurrency_limit",         make_json(static_cast<std::uint64_t>(request.concurrency_limit))           },
        {"defaults",                  std::move(defaults).value()                                                },
        {"history_retention_seconds", encode_nullable_seconds(request.history_retention)                         },
        {"name",                      make_json(request.name)                                                    },
        {"recovery_policy",           make_json(std::string{*recovery})                                          },
        {"runnable_wait_warning_ms",  make_json(static_cast<std::int64_t>(request.runnable_wait_warning.count()))},
        {"weight",                    make_json(static_cast<std::uint64_t>(request.weight))                      },
    };
    if (request.idempotency_key) {
        object.emplace("idempotency_key", make_json(*request.idempotency_key));
    }
    return checked_json(make_json(std::move(object)), true);
}

auto create_queue_request_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<CreateQueueRequest, jb::core::Error>
{
    if (!value.is_object() || !has_only_members(value.as_object(),
                                                {"name",
                                                 "weight",
                                                 "concurrency_limit",
                                                 "recovery_policy",
                                                 "defaults",
                                                 "history_retention_seconds",
                                                 "runnable_wait_warning_ms",
                                                 "idempotency_key"})) {
        return invalid<CreateQueueRequest>(true);
    }
    auto const& object = value.as_object();
    auto const* name   = find_member(object, "name");
    if (!name || !name->is_string()) {
        return invalid<CreateQueueRequest>(true);
    }

    auto result = CreateQueueRequest{.name = name->as_string()};
    if (auto const* weight = find_member(object, "weight"); weight && !decode_unsigned(*weight, result.weight)) {
        return invalid<CreateQueueRequest>(true);
    }
    if (auto const* concurrency = find_member(object, "concurrency_limit");
        concurrency && !decode_unsigned(*concurrency, result.concurrency_limit)) {
        return invalid<CreateQueueRequest>(true);
    }
    if (auto const* recovery = find_member(object, "recovery_policy");
        recovery && !recovery_policy_from_json(*recovery, result.recovery_policy)) {
        return invalid<CreateQueueRequest>(true);
    }
    if (auto const* defaults = find_member(object, "defaults")) {
        auto decoded = attribute_set_from_json(*defaults, registry, AttributeScope::QueueDefault);
        if (!decoded) {
            return invalid<CreateQueueRequest>(true);
        }
        result.defaults = std::move(decoded).value();
    }
    if (auto const* retention = find_member(object, "history_retention_seconds");
        retention && !decode_nullable_seconds(*retention, result.history_retention)) {
        return invalid<CreateQueueRequest>(true);
    }
    if (auto const* warning = find_member(object, "runnable_wait_warning_ms")) {
        auto count = std::chrono::milliseconds::rep{};
        if (!decode_signed(*warning, count)) {
            return invalid<CreateQueueRequest>(true);
        }
        result.runnable_wait_warning = std::chrono::milliseconds{count};
    }
    if (auto const* key = find_member(object, "idempotency_key")) {
        if (!key->is_string()) {
            return invalid<CreateQueueRequest>(true);
        }
        result.idempotency_key = key->as_string();
    }
    return ConversionResult<CreateQueueRequest>::success(std::move(result));
}

auto queue_list_request_to_json(QueueListRequest const& request)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>
{
    if (request.page.limit == 0 || request.page.limit > maximum_page_size) {
        return invalid<jb::rpc::JsonValue>(true);
    }
    auto object = jb::rpc::JsonValue::Object{
        {"include_deleted", make_json(request.include_deleted)                       },
        {"limit",           make_json(static_cast<std::uint64_t>(request.page.limit))},
    };
    if (request.state) {
        auto const state = queue_state_text(*request.state);
        if (!state) {
            return invalid<jb::rpc::JsonValue>(true);
        }
        object.emplace("state", make_json(std::string{*state}));
    }
    if (request.page.after_id) {
        object.emplace("after_id", make_json(request.page.after_id->to_string()));
    }
    return checked_json(make_json(std::move(object)), true);
}

auto queue_list_request_from_json(jb::rpc::JsonValue const& value)
    -> jb::core::Result<QueueListRequest, jb::core::Error>
{
    if (!value.is_object() || !has_only_members(value.as_object(), {"include_deleted", "state", "limit", "after_id"})) {
        return invalid<QueueListRequest>(true);
    }
    auto const& object = value.as_object();
    auto        result = QueueListRequest{};
    if (auto const* include_deleted = find_member(object, "include_deleted")) {
        if (!include_deleted->is_bool()) {
            return invalid<QueueListRequest>(true);
        }
        result.include_deleted = include_deleted->as_bool();
    }
    if (auto const* state = find_member(object, "state")) {
        auto decoded = QueueState{};
        if (!queue_state_from_json(*state, decoded)) {
            return invalid<QueueListRequest>(true);
        }
        result.state = decoded;
    }
    if (auto const* limit = find_member(object, "limit"); limit && !decode_unsigned(*limit, result.page.limit)) {
        return invalid<QueueListRequest>(true);
    }
    if (result.page.limit == 0 || result.page.limit > maximum_page_size) {
        return invalid<QueueListRequest>(true);
    }
    if (auto const* after = find_member(object, "after_id")) {
        auto id = jb::core::Uuid{};
        if (!decode_uuid(*after, id)) {
            return invalid<QueueListRequest>(true);
        }
        result.page.after_id = id;
    }
    return ConversionResult<QueueListRequest>::success(std::move(result));
}

auto update_queue_request_to_json(UpdateQueueRequest const& request, AttributeRegistry const& registry)
    -> jb::core::Result<jb::rpc::JsonValue, jb::core::Error>
{
    if (!has_queue_update(request)) {
        return invalid<jb::rpc::JsonValue>(true);
    }
    auto object = jb::rpc::JsonValue::Object{};
    if (!add_selector_members(object, request.queue)) {
        return invalid<jb::rpc::JsonValue>(true);
    }
    if (request.name) {
        object.emplace("name", make_json(*request.name));
    }
    if (request.weight) {
        object.emplace("weight", make_json(static_cast<std::uint64_t>(*request.weight)));
    }
    if (request.concurrency_limit) {
        object.emplace("concurrency_limit", make_json(static_cast<std::uint64_t>(*request.concurrency_limit)));
    }
    if (request.recovery_policy) {
        auto const recovery = recovery_policy_text(*request.recovery_policy);
        if (!recovery) {
            return invalid<jb::rpc::JsonValue>(true);
        }
        object.emplace("recovery_policy", make_json(std::string{*recovery}));
    }
    if (request.defaults) {
        auto defaults = attribute_set_to_json(*request.defaults, registry, AttributeScope::QueueDefault);
        if (!defaults) {
            return invalid<jb::rpc::JsonValue>(true);
        }
        object.emplace("defaults", std::move(defaults).value());
    }
    if (request.history_retention) {
        object.emplace("history_retention_seconds", encode_nullable_seconds(*request.history_retention));
    }
    if (request.runnable_wait_warning) {
        object.emplace("runnable_wait_warning_ms",
                       make_json(static_cast<std::int64_t>(request.runnable_wait_warning->count())));
    }
    return checked_json(make_json(std::move(object)), true);
}

auto update_queue_request_from_json(jb::rpc::JsonValue const& value, AttributeRegistry const& registry)
    -> jb::core::Result<UpdateQueueRequest, jb::core::Error>
{
    if (!value.is_object() || !has_only_members(value.as_object(),
                                                {"queue_id",
                                                 "queue_name",
                                                 "name",
                                                 "weight",
                                                 "concurrency_limit",
                                                 "recovery_policy",
                                                 "defaults",
                                                 "history_retention_seconds",
                                                 "runnable_wait_warning_ms"})) {
        return invalid<UpdateQueueRequest>(true);
    }
    auto const& object   = value.as_object();
    auto        selector = decode_selector(object);
    if (!selector) {
        return invalid<UpdateQueueRequest>(true);
    }
    auto result = UpdateQueueRequest{.queue = std::move(selector).value()};
    if (auto const* name = find_member(object, "name")) {
        if (!name->is_string()) {
            return invalid<UpdateQueueRequest>(true);
        }
        result.name = name->as_string();
    }
    if (auto const* weight = find_member(object, "weight")) {
        auto decoded = std::uint32_t{};
        if (!decode_unsigned(*weight, decoded)) {
            return invalid<UpdateQueueRequest>(true);
        }
        result.weight = decoded;
    }
    if (auto const* concurrency = find_member(object, "concurrency_limit")) {
        auto decoded = std::uint32_t{};
        if (!decode_unsigned(*concurrency, decoded)) {
            return invalid<UpdateQueueRequest>(true);
        }
        result.concurrency_limit = decoded;
    }
    if (auto const* recovery = find_member(object, "recovery_policy")) {
        auto decoded = RecoveryPolicy{};
        if (!recovery_policy_from_json(*recovery, decoded)) {
            return invalid<UpdateQueueRequest>(true);
        }
        result.recovery_policy = decoded;
    }
    if (auto const* defaults = find_member(object, "defaults")) {
        auto decoded = attribute_set_from_json(*defaults, registry, AttributeScope::QueueDefault);
        if (!decoded) {
            return invalid<UpdateQueueRequest>(true);
        }
        result.defaults = std::move(decoded).value();
    }
    if (auto const* retention = find_member(object, "history_retention_seconds")) {
        auto decoded = std::optional<std::chrono::seconds>{};
        if (!decode_nullable_seconds(*retention, decoded)) {
            return invalid<UpdateQueueRequest>(true);
        }
        result.history_retention = decoded;
    }
    if (auto const* warning = find_member(object, "runnable_wait_warning_ms")) {
        auto count = std::chrono::milliseconds::rep{};
        if (!decode_signed(*warning, count)) {
            return invalid<UpdateQueueRequest>(true);
        }
        result.runnable_wait_warning = std::chrono::milliseconds{count};
    }
    if (!has_queue_update(result)) {
        return invalid<UpdateQueueRequest>(true);
    }
    return ConversionResult<UpdateQueueRequest>::success(std::move(result));
}

} // namespace jb::jobu
