#include "history_json.hpp"

#include "json.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {
namespace {

using jb::core::JsonValue;
template <typename T>
using CodecResult = jb::core::Result<T, jb::core::Error>;

auto invalid(bool request) -> jb::core::Error
{
    return {.category = jb::core::ErrorCategory::InvalidArgument,
            .code     = request ? "jobu.protocol.invalid_request" : "jobu.protocol.invalid_response",
            .message  = request ? "The JobU history request is invalid" : "The JobU history response is invalid"};
}

template <typename T>
auto reject(bool request) -> CodecResult<T>
{
    return CodecResult<T>::failure(invalid(request));
}

auto json(auto value) -> JsonValue
{
    return JsonValue{.data = std::move(value)};
}

auto member(JsonValue::Object const& object, std::string_view name) -> JsonValue const*
{
    auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

auto only_members(JsonValue::Object const& object, std::initializer_list<std::string_view> allowed) -> bool
{
    for (auto const& [name, value] : object) {
        static_cast<void>(value);
        auto permitted = false;
        for (auto candidate : allowed) {
            if (name == candidate) {
                permitted = true;
                break;
            }
        }
        if (!permitted) {
            return false;
        }
    }
    return true;
}

auto decode_uuid(JsonValue const& value, jb::core::Uuid& id) -> bool
{
    if (!value.is_string()) {
        return false;
    }
    auto parsed = jb::core::Uuid::parse(value.as_string());
    if (!parsed || parsed->is_nil() || parsed->to_string() != value.as_string()) {
        return false;
    }
    id = std::move(parsed).value();
    return true;
}

auto decode_positive_number(JsonValue const& value, AttemptNumber& number) -> bool
{
    if (value.is_uint()) {
        number = value.as_uint();
    }
    else if (value.is_int() && value.as_int() > 0) {
        number = static_cast<std::uint64_t>(value.as_int());
    }
    else {
        return false;
    }
    return number != 0;
}

auto checked_json(JsonValue value, bool request) -> CodecResult<JsonValue>
{
    if (!jb::core::serialize_json(value)) {
        return reject<JsonValue>(request);
    }
    return CodecResult<JsonValue>::success(std::move(value));
}

template <typename Page, typename Encode>
auto encode_page(Page const& page, Encode&& encode) -> CodecResult<JsonValue>
{
    if (page.items.size() > 200U || (page.next_cursor && (page.next_cursor->empty() || page.items.empty()))) {
        return reject<JsonValue>(false);
    }
    auto items = JsonValue::Array{};
    items.reserve(page.items.size());
    for (auto const& item : page.items) {
        auto encoded = encode(item);
        if (!encoded) {
            return reject<JsonValue>(false);
        }
        items.push_back(std::move(encoded).value());
    }
    return checked_json(json(JsonValue::Object{
                            {"items",       json(std::move(items))                                                 },
                            {"next_cursor", page.next_cursor ? json(*page.next_cursor) : json(jb::core::JsonNull{})},
    }),
                        false);
}

template <typename Page, typename Decode>
auto decode_page(JsonValue const& value, Decode&& decode) -> CodecResult<Page>
{
    if (!value.is_object()) {
        return reject<Page>(false);
    }
    auto const* items  = member(value.as_object(), "items");
    auto const* cursor = member(value.as_object(), "next_cursor");
    if (!items || !items->is_array() || items->as_array().size() > 200U || !cursor ||
        (!cursor->is_null() && (!cursor->is_string() || cursor->as_string().empty())) ||
        (items->as_array().empty() && !cursor->is_null())) {
        return reject<Page>(false);
    }
    auto page = Page{};
    page.items.reserve(items->as_array().size());
    for (auto const& item : items->as_array()) {
        auto decoded = decode(item);
        if (!decoded) {
            return reject<Page>(false);
        }
        page.items.push_back(std::move(decoded).value());
    }
    if (cursor->is_string()) {
        page.next_cursor = cursor->as_string();
    }
    return CodecResult<Page>::success(std::move(page));
}

} // namespace

auto run_get_request_to_json(jb::core::Uuid const& id) -> CodecResult<JsonValue>
{
    if (id.is_nil()) {
        return reject<JsonValue>(true);
    }
    return CodecResult<JsonValue>::success(json(JsonValue::Object{
        {"run_id", json(id.to_string())}
    }));
}

auto run_get_request_from_json(JsonValue const& value) -> CodecResult<jb::core::Uuid>
{
    if (!value.is_object() || !only_members(value.as_object(), {"run_id"})) {
        return reject<jb::core::Uuid>(true);
    }
    auto        id    = jb::core::Uuid{};
    auto const* field = member(value.as_object(), "run_id");
    if (!field || !decode_uuid(*field, id)) {
        return reject<jb::core::Uuid>(true);
    }
    return CodecResult<jb::core::Uuid>::success(id);
}

auto attempt_get_request_to_json(AttemptKey const& key) -> CodecResult<JsonValue>
{
    if (key.run_id.is_nil() || key.attempt_number == 0) {
        return reject<JsonValue>(true);
    }
    return CodecResult<JsonValue>::success(json(JsonValue::Object{
        {"run_id",         json(key.run_id.to_string())},
        {"attempt_number", json(key.attempt_number)    },
    }));
}

auto attempt_get_request_from_json(JsonValue const& value) -> CodecResult<AttemptKey>
{
    if (!value.is_object() || !only_members(value.as_object(), {"run_id", "attempt_number"})) {
        return reject<AttemptKey>(true);
    }
    auto        key    = AttemptKey{};
    auto const* id     = member(value.as_object(), "run_id");
    auto const* number = member(value.as_object(), "attempt_number");
    if (!id || !decode_uuid(*id, key.run_id) || !number || !decode_positive_number(*number, key.attempt_number)) {
        return reject<AttemptKey>(true);
    }
    return CodecResult<AttemptKey>::success(key);
}

auto run_page_to_json(RunPage const& page) -> CodecResult<JsonValue>
{
    return encode_page(page, run_summary_to_json);
}

auto run_page_from_json(JsonValue const& value) -> CodecResult<RunPage>
{
    return decode_page<RunPage>(value, run_summary_from_json);
}

auto attempt_details_to_json(AttemptDetails const& details) -> CodecResult<JsonValue>
{
    auto summary = attempt_summary_to_json(details);
    if (!summary || (details.result && !details.result->is_object())) {
        return reject<JsonValue>(false);
    }
    auto  view   = std::move(summary).value();
    auto& fields = std::get<JsonValue::Object>(view.data);
    fields.emplace("result", details.result ? *details.result : json(jb::core::JsonNull{}));
    return checked_json(std::move(view), false);
}

auto attempt_details_from_json(JsonValue const& value) -> CodecResult<AttemptDetails>
{
    auto summary = attempt_summary_from_json(value);
    if (!summary) {
        return reject<AttemptDetails>(false);
    }
    auto const* result = member(value.as_object(), "result");
    if (!result || (!result->is_null() && !result->is_object())) {
        return reject<AttemptDetails>(false);
    }
    auto details                          = AttemptDetails{};
    static_cast<AttemptSummary&>(details) = std::move(summary).value();
    if (!result->is_null()) {
        details.result = *result;
    }
    return CodecResult<AttemptDetails>::success(std::move(details));
}

auto attempt_page_to_json(AttemptPage const& page) -> CodecResult<JsonValue>
{
    return encode_page(page, attempt_summary_to_json);
}

auto attempt_page_from_json(JsonValue const& value) -> CodecResult<AttemptPage>
{
    return decode_page<AttemptPage>(value, attempt_summary_from_json);
}

} // namespace jb::jobu
