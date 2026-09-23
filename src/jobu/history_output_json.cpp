#include "history_json.hpp"

#include "text_validation_priv.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace jb::jobu {
namespace {

using jb::core::JsonValue;
template <typename T>
using CodecResult = jb::core::Result<T, jb::core::Error>;

constexpr std::size_t maximum_slice_bytes  = 65'536;
constexpr std::size_t maximum_base64_chars = ((maximum_slice_bytes + 2U) / 3U) * 4U;

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

auto unsigned_value(JsonValue const& value) -> std::optional<std::uint64_t>
{
    if (value.is_uint()) {
        return value.as_uint();
    }
    if (value.is_int() && value.as_int() >= 0) {
        return static_cast<std::uint64_t>(value.as_int());
    }
    return std::nullopt;
}

auto optional_unsigned(JsonValue const& value, std::optional<std::uint64_t>& target) -> bool
{
    if (value.is_null()) {
        target.reset();
        return true;
    }
    target = unsigned_value(value);
    return target.has_value();
}

auto channel_text(OutputChannel channel) -> std::optional<std::string_view>
{
    switch (channel) {
        case OutputChannel::Stdout:
            return "stdout";
        case OutputChannel::Stderr:
            return "stderr";
        case OutputChannel::Body:
            return "body";
        case OutputChannel::Headers:
            return "headers";
    }
    return std::nullopt;
}

auto parse_channel(std::string_view text) -> std::optional<OutputChannel>
{
    if (text == "stdout") {
        return OutputChannel::Stdout;
    }
    if (text == "stderr") {
        return OutputChannel::Stderr;
    }
    if (text == "body") {
        return OutputChannel::Body;
    }
    if (text == "headers") {
        return OutputChannel::Headers;
    }
    return std::nullopt;
}

auto status_text(OutputStatus status) -> std::optional<std::string_view>
{
    switch (status) {
        case OutputStatus::Available:
            return "available";
        case OutputStatus::Pending:
            return "pending";
        case OutputStatus::NotCaptured:
            return "not_captured";
        case OutputStatus::Lost:
            return "lost";
    }
    return std::nullopt;
}

auto parse_status(std::string_view text) -> std::optional<OutputStatus>
{
    if (text == "available") {
        return OutputStatus::Available;
    }
    if (text == "pending") {
        return OutputStatus::Pending;
    }
    if (text == "not_captured") {
        return OutputStatus::NotCaptured;
    }
    if (text == "lost") {
        return OutputStatus::Lost;
    }
    return std::nullopt;
}

auto encoding_text(OutputEncoding encoding) -> std::optional<std::string_view>
{
    switch (encoding) {
        case OutputEncoding::Utf8:
            return "utf8";
        case OutputEncoding::Base64:
            return "base64";
    }
    return std::nullopt;
}

auto parse_encoding(std::string_view text) -> std::optional<OutputEncoding>
{
    if (text == "utf8") {
        return OutputEncoding::Utf8;
    }
    if (text == "base64") {
        return OutputEncoding::Base64;
    }
    return std::nullopt;
}

auto encode_base64(jb::core::ByteView bytes) -> std::string
{
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto                       encoded  = std::string{};
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (auto offset = std::size_t{0}; offset < bytes.size(); offset += 3U) {
        auto const first  = std::to_integer<unsigned>(bytes[offset]);
        auto const second = offset + 1U < bytes.size() ? std::to_integer<unsigned>(bytes[offset + 1U]) : 0U;
        auto const third  = offset + 2U < bytes.size() ? std::to_integer<unsigned>(bytes[offset + 2U]) : 0U;
        encoded.push_back(alphabet[first >> 2U]);
        encoded.push_back(alphabet[((first & 3U) << 4U) | (second >> 4U)]);
        encoded.push_back(offset + 1U < bytes.size() ? alphabet[((second & 15U) << 2U) | (third >> 6U)] : '=');
        encoded.push_back(offset + 2U < bytes.size() ? alphabet[third & 63U] : '=');
    }
    return encoded;
}

auto base64_value(unsigned char value) -> std::optional<std::uint8_t>
{
    if (value >= 'A' && value <= 'Z') {
        return static_cast<std::uint8_t>(value - 'A');
    }
    if (value >= 'a' && value <= 'z') {
        return static_cast<std::uint8_t>(value - 'a' + 26U);
    }
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0' + 52U);
    }
    if (value == '+') {
        return 62U;
    }
    if (value == '/') {
        return 63U;
    }
    return std::nullopt;
}

auto decode_base64(std::string_view encoded) -> std::optional<jb::core::ByteBuffer>
{
    if (encoded.size() > maximum_base64_chars || encoded.size() % 4U != 0U) {
        return std::nullopt;
    }
    auto bytes = jb::core::ByteBuffer{};
    bytes.reserve((encoded.size() / 4U) * 3U);
    for (auto index = std::size_t{0}; index < encoded.size(); index += 4U) {
        auto const first  = base64_value(static_cast<unsigned char>(encoded[index]));
        auto const second = base64_value(static_cast<unsigned char>(encoded[index + 1U]));
        auto const final  = index + 4U == encoded.size();
        if (!first || !second) {
            return std::nullopt;
        }
        if (encoded[index + 2U] == '=') {
            if (!final || encoded[index + 3U] != '=' || (*second & 15U) != 0U) {
                return std::nullopt;
            }
            bytes.push_back(static_cast<std::byte>((*first << 2U) | (*second >> 4U)));
            continue;
        }
        auto const third = base64_value(static_cast<unsigned char>(encoded[index + 2U]));
        if (!third) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::byte>((*first << 2U) | (*second >> 4U)));
        bytes.push_back(static_cast<std::byte>(((*second & 15U) << 4U) | (*third >> 2U)));
        if (encoded[index + 3U] == '=') {
            if (!final || (*third & 3U) != 0U) {
                return std::nullopt;
            }
            continue;
        }
        auto const fourth = base64_value(static_cast<unsigned char>(encoded[index + 3U]));
        if (!fourth) {
            return std::nullopt;
        }
        bytes.push_back(static_cast<std::byte>(((*third & 3U) << 6U) | *fourth));
    }
    return bytes.size() <= maximum_slice_bytes ? std::optional{std::move(bytes)} : std::nullopt;
}

auto valid_request(AttemptOutputRequest const& request) -> bool
{
    return request.attempt.attempt_number > 0 && channel_text(request.channel).has_value() && request.limit >= 1 &&
           request.limit <= maximum_slice_bytes &&
           request.offset <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - 1);
}

auto valid_chunk(AttemptOutputChunk const& chunk) -> bool
{
    if (chunk.attempt.attempt_number == 0 || !channel_text(chunk.channel) || !status_text(chunk.status) ||
        !encoding_text(chunk.encoding) || chunk.bytes_returned != chunk.data.size() ||
        chunk.bytes_returned > maximum_slice_bytes || chunk.offset > chunk.retained_bytes ||
        chunk.bytes_returned > chunk.retained_bytes - chunk.offset ||
        (chunk.total_bytes.has_value() != chunk.omitted_bytes.has_value()) ||
        (chunk.total_bytes && (*chunk.total_bytes < chunk.retained_bytes ||
                               *chunk.omitted_bytes != *chunk.total_bytes - chunk.retained_bytes))) {
        return false;
    }
    auto const end = chunk.offset + chunk.bytes_returned;
    if (chunk.next_offset != (end < chunk.retained_bytes ? std::optional{end} : std::nullopt)) {
        return false;
    }
    if ((chunk.status == OutputStatus::Lost) != chunk.capture_lost ||
        ((chunk.status == OutputStatus::Pending || chunk.status == OutputStatus::NotCaptured) &&
         (!chunk.data.empty() || chunk.retained_bytes != 0))) {
        return false;
    }
    return chunk.encoding != OutputEncoding::Utf8 || detail::is_valid_utf8(jb::core::as_string_view(chunk.data));
}

auto nullable_uint(std::optional<std::uint64_t> value) -> JsonValue
{
    return value ? json(*value) : json(jb::core::JsonNull{});
}

} // namespace

auto attempt_output_request_to_json(AttemptOutputRequest const& request) -> CodecResult<JsonValue>
{
    if (!valid_request(request)) {
        return reject<JsonValue>(true);
    }
    return CodecResult<JsonValue>::success(json(JsonValue::Object{
        {"run_id",         json(request.attempt.run_id.to_string())         },
        {"attempt_number", json(request.attempt.attempt_number)             },
        {"channel",        json(std::string{*channel_text(request.channel)})},
        {"offset",         json(request.offset)                             },
        {"limit",          json(static_cast<std::uint64_t>(request.limit))  },
    }));
}

auto attempt_output_request_from_json(JsonValue const& value) -> CodecResult<AttemptOutputRequest>
{
    if (!value.is_object()) {
        return reject<AttemptOutputRequest>(true);
    }
    auto const& object = value.as_object();
    if (!only_members(object, {"run_id", "attempt_number", "channel", "offset", "limit"})) {
        return reject<AttemptOutputRequest>(true);
    }
    auto const* run_id  = member(object, "run_id");
    auto const* number  = member(object, "attempt_number");
    auto const* channel = member(object, "channel");
    if (!run_id || !run_id->is_string() || !number || !channel || !channel->is_string()) {
        return reject<AttemptOutputRequest>(true);
    }
    auto parsed_id      = jb::core::Uuid::parse(run_id->as_string());
    auto parsed_number  = unsigned_value(*number);
    auto parsed_channel = parse_channel(channel->as_string());
    if (!parsed_id || parsed_id->to_string() != run_id->as_string() || !parsed_number || !parsed_channel) {
        return reject<AttemptOutputRequest>(true);
    }
    auto request = AttemptOutputRequest{
        .attempt = {.run_id = *parsed_id, .attempt_number = *parsed_number},
        .channel = *parsed_channel
    };
    if (auto const* offset = member(object, "offset")) {
        auto parsed = unsigned_value(*offset);
        if (!parsed) {
            return reject<AttemptOutputRequest>(true);
        }
        request.offset = *parsed;
    }
    if (auto const* limit = member(object, "limit")) {
        auto parsed = unsigned_value(*limit);
        if (!parsed || *parsed > maximum_slice_bytes) {
            return reject<AttemptOutputRequest>(true);
        }
        request.limit = static_cast<std::size_t>(*parsed);
    }
    if (!valid_request(request)) {
        return reject<AttemptOutputRequest>(true);
    }
    return CodecResult<AttemptOutputRequest>::success(request);
}

auto attempt_output_chunk_to_json(AttemptOutputChunk const& chunk) -> CodecResult<JsonValue>
{
    if (!valid_chunk(chunk)) {
        return reject<JsonValue>(false);
    }
    auto data = chunk.encoding == OutputEncoding::Utf8 ? std::string{jb::core::as_string_view(chunk.data)}
                                                       : encode_base64(chunk.data);
    return CodecResult<JsonValue>::success(json(JsonValue::Object{
        {"run_id",         json(chunk.attempt.run_id.to_string())                },
        {"attempt_number", json(chunk.attempt.attempt_number)                    },
        {"channel",        json(std::string{*channel_text(chunk.channel)})       },
        {"status",         json(std::string{*status_text(chunk.status)})         },
        {"offset",         json(chunk.offset)                                    },
        {"bytes_returned", json(static_cast<std::uint64_t>(chunk.bytes_returned))},
        {"next_offset",    nullable_uint(chunk.next_offset)                      },
        {"retained_bytes", json(chunk.retained_bytes)                            },
        {"total_bytes",    nullable_uint(chunk.total_bytes)                      },
        {"omitted_bytes",  nullable_uint(chunk.omitted_bytes)                    },
        {"truncated",      json(chunk.truncated)                                 },
        {"capture_lost",   json(chunk.capture_lost)                              },
        {"encoding",       json(std::string{*encoding_text(chunk.encoding)})     },
        {"data",           json(std::move(data))                                 },
    }));
}

auto attempt_output_chunk_from_json(JsonValue const& value) -> CodecResult<AttemptOutputChunk>
{
    if (!value.is_object()) {
        return reject<AttemptOutputChunk>(false);
    }
    auto const& object    = value.as_object();
    auto const* run_id    = member(object, "run_id");
    auto const* number    = member(object, "attempt_number");
    auto const* channel   = member(object, "channel");
    auto const* status    = member(object, "status");
    auto const* offset    = member(object, "offset");
    auto const* count     = member(object, "bytes_returned");
    auto const* next      = member(object, "next_offset");
    auto const* retained  = member(object, "retained_bytes");
    auto const* total     = member(object, "total_bytes");
    auto const* omitted   = member(object, "omitted_bytes");
    auto const* truncated = member(object, "truncated");
    auto const* lost      = member(object, "capture_lost");
    auto const* encoding  = member(object, "encoding");
    auto const* data      = member(object, "data");
    if (!run_id || !run_id->is_string() || !number || !channel || !channel->is_string() || !status ||
        !status->is_string() || !offset || !count || !next || !retained || !total || !omitted || !truncated ||
        !truncated->is_bool() || !lost || !lost->is_bool() || !encoding || !encoding->is_string() || !data ||
        !data->is_string()) {
        return reject<AttemptOutputChunk>(false);
    }
    auto parsed_id       = jb::core::Uuid::parse(run_id->as_string());
    auto parsed_number   = unsigned_value(*number);
    auto parsed_channel  = parse_channel(channel->as_string());
    auto parsed_status   = parse_status(status->as_string());
    auto parsed_offset   = unsigned_value(*offset);
    auto parsed_count    = unsigned_value(*count);
    auto parsed_retained = unsigned_value(*retained);
    auto parsed_encoding = parse_encoding(encoding->as_string());
    if (!parsed_id || parsed_id->to_string() != run_id->as_string() || !parsed_number || !parsed_channel ||
        !parsed_status || !parsed_offset || !parsed_count || !parsed_retained || !parsed_encoding ||
        *parsed_count > maximum_slice_bytes) {
        return reject<AttemptOutputChunk>(false);
    }
    auto chunk = AttemptOutputChunk{
        .attempt        = {.run_id = *parsed_id, .attempt_number = *parsed_number},
        .channel        = *parsed_channel,
        .status         = *parsed_status,
        .offset         = *parsed_offset,
        .bytes_returned = static_cast<std::size_t>(*parsed_count),
        .retained_bytes = *parsed_retained,
        .truncated      = truncated->as_bool(),
        .capture_lost   = lost->as_bool(),
        .encoding       = *parsed_encoding,
    };
    if (!optional_unsigned(*next, chunk.next_offset) || !optional_unsigned(*total, chunk.total_bytes) ||
        !optional_unsigned(*omitted, chunk.omitted_bytes)) {
        return reject<AttemptOutputChunk>(false);
    }
    if (chunk.encoding == OutputEncoding::Utf8) {
        if (data->as_string().size() > maximum_slice_bytes || !detail::is_valid_utf8(data->as_string())) {
            return reject<AttemptOutputChunk>(false);
        }
        auto const view = jb::core::as_bytes(data->as_string());
        chunk.data.assign(view.begin(), view.end());
    }
    else {
        auto decoded = decode_base64(data->as_string());
        if (!decoded) {
            return reject<AttemptOutputChunk>(false);
        }
        chunk.data = std::move(*decoded);
    }
    if (!valid_chunk(chunk)) {
        return reject<AttemptOutputChunk>(false);
    }
    return CodecResult<AttemptOutputChunk>::success(std::move(chunk));
}

} // namespace jb::jobu
