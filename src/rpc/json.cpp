#include "json.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

namespace jb::rpc {

namespace {

using CodecJson = nlohmann::json;

enum class JsonFailure : std::uint8_t {
    Syntax,
    InvalidUtf8,
    DuplicateMember,
    DepthLimit,
    IntegerOverflow,
    NonFinite,
    Internal,
};

auto make_json_error(JsonFailure failure) -> jb::core::Error
{
    using jb::core::Error;
    using jb::core::ErrorCategory;

    switch (failure) {
        case JsonFailure::Syntax:
            return Error{
                .category = ErrorCategory::InvalidArgument,
                .code     = "rpc.json.syntax",
                .message  = "JSON text is invalid",
            };
        case JsonFailure::InvalidUtf8:
            return Error{
                .category = ErrorCategory::InvalidArgument,
                .code     = "rpc.json.invalid_utf8",
                .message  = "JSON contains invalid UTF-8",
            };
        case JsonFailure::DuplicateMember:
            return Error{
                .category = ErrorCategory::InvalidArgument,
                .code     = "rpc.json.duplicate_member",
                .message  = "JSON object contains a duplicate member",
            };
        case JsonFailure::DepthLimit:
            return Error{
                .category = ErrorCategory::ResourceExhausted,
                .code     = "rpc.json.depth_limit",
                .message  = "JSON nesting exceeds the configured limit",
            };
        case JsonFailure::IntegerOverflow:
            return Error{
                .category = ErrorCategory::InvalidArgument,
                .code     = "rpc.json.integer_overflow",
                .message  = "JSON integer is outside the supported range",
            };
        case JsonFailure::NonFinite:
            return Error{
                .category = ErrorCategory::InvalidArgument,
                .code     = "rpc.json.non_finite",
                .message  = "JSON number is not finite",
            };
        case JsonFailure::Internal:
            return Error{
                .category = ErrorCategory::Internal,
                .code     = "rpc.json.internal",
                .message  = "JSON codec failed",
            };
    }

    return Error{
        .category = ErrorCategory::Internal,
        .code     = "rpc.json.internal",
        .message  = "JSON codec failed",
    };
}

auto is_continuation(unsigned char byte) noexcept -> bool
{
    return byte >= 0x80U && byte <= 0xbfU;
}

auto is_valid_utf8(std::string_view text) noexcept -> bool
{
    auto const* bytes = reinterpret_cast<unsigned char const*>(text.data());
    auto        index = std::size_t{0};

    while (index < text.size()) {
        auto const first = bytes[index];
        if (first <= 0x7fU) {
            ++index;
            continue;
        }

        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1 >= text.size() || !is_continuation(bytes[index + 1])) {
                return false;
            }
            index += 2;
            continue;
        }

        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2 >= text.size() || !is_continuation(bytes[index + 2])) {
                return false;
            }

            auto const second       = bytes[index + 1];
            auto       valid_second = is_continuation(second);
            if (first == 0xe0U) {
                valid_second = second >= 0xa0U && second <= 0xbfU;
            }
            else if (first == 0xedU) {
                valid_second = second >= 0x80U && second <= 0x9fU;
            }
            if (!valid_second) {
                return false;
            }
            index += 3;
            continue;
        }

        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3 >= text.size() || !is_continuation(bytes[index + 2]) || !is_continuation(bytes[index + 3])) {
                return false;
            }

            auto const second       = bytes[index + 1];
            auto       valid_second = is_continuation(second);
            if (first == 0xf0U) {
                valid_second = second >= 0x90U && second <= 0xbfU;
            }
            else if (first == 0xf4U) {
                valid_second = second >= 0x80U && second <= 0x8fU;
            }
            if (!valid_second) {
                return false;
            }
            index += 4;
            continue;
        }

        return false;
    }

    return true;
}

auto token_is_integer(std::string const& token) noexcept -> bool
{
    return token.find_first_of(".eE") == std::string::npos;
}

class JsonSaxParser final : public nlohmann::json_sax<CodecJson> {
public:
    explicit JsonSaxParser(JsonLimits limits)
        : _limits{limits}
    {}

    auto null() -> bool override { return add_value(JsonValue{}); }

    auto boolean(bool value) -> bool override
    {
        JsonValue result;
        result.data = value;
        return add_value(std::move(result));
    }

    auto number_integer(number_integer_t value) -> bool override
    {
        JsonValue result;
        result.data = static_cast<std::int64_t>(value);
        return add_value(std::move(result));
    }

    auto number_unsigned(number_unsigned_t value) -> bool override
    {
        JsonValue result;
        result.data = static_cast<std::uint64_t>(value);
        return add_value(std::move(result));
    }

    auto number_float(number_float_t value, string_t const& token) -> bool override
    {
        if (token_is_integer(token)) {
            return fail(JsonFailure::IntegerOverflow);
        }
        if (!std::isfinite(value)) {
            return fail(JsonFailure::NonFinite);
        }

        JsonValue result;
        result.data = static_cast<double>(value);
        return add_value(std::move(result));
    }

    auto string(string_t& value) -> bool override
    {
        if (!is_valid_utf8(value)) {
            return fail(JsonFailure::InvalidUtf8);
        }

        JsonValue result;
        result.data = std::move(value);
        return add_value(std::move(result));
    }

    auto binary(binary_t& value) -> bool override
    {
        static_cast<void>(value);
        return fail(JsonFailure::Internal);
    }

    auto start_object(std::size_t elements) -> bool override
    {
        static_cast<void>(elements);
        return start_container(JsonValue::Object{});
    }

    auto key(string_t& value) -> bool override
    {
        if (_frames.empty() || !std::holds_alternative<JsonValue::Object>(_frames.back().value.data)) {
            return fail(JsonFailure::Internal);
        }
        if (!is_valid_utf8(value)) {
            return fail(JsonFailure::InvalidUtf8);
        }

        auto& frame  = _frames.back();
        auto& object = std::get<JsonValue::Object>(frame.value.data);
        if (frame.key || object.contains(value)) {
            return fail(JsonFailure::DuplicateMember);
        }
        frame.key = std::move(value);
        return true;
    }

    auto end_object() -> bool override
    {
        if (_frames.empty() || !std::holds_alternative<JsonValue::Object>(_frames.back().value.data) ||
            _frames.back().key) {
            return fail(JsonFailure::Internal);
        }
        return finish_container();
    }

    auto start_array(std::size_t elements) -> bool override
    {
        static_cast<void>(elements);
        return start_container(JsonValue::Array{});
    }

    auto end_array() -> bool override
    {
        if (_frames.empty() || !std::holds_alternative<JsonValue::Array>(_frames.back().value.data)) {
            return fail(JsonFailure::Internal);
        }
        return finish_container();
    }

    auto parse_error(std::size_t position, std::string const& token, nlohmann::detail::exception const& exception)
        -> bool override
    {
        static_cast<void>(position);
        if (exception.id == 406) {
            return fail(token_is_integer(token) ? JsonFailure::IntegerOverflow : JsonFailure::NonFinite);
        }
        return fail(JsonFailure::Syntax);
    }

    [[nodiscard]] auto failure() const noexcept -> std::optional<JsonFailure> { return _failure; }

    auto take_result() -> JsonValue { return std::move(*_root); }

    [[nodiscard]] auto has_result() const noexcept -> bool { return _root.has_value(); }

private:
    struct Frame {
        JsonValue                  value;
        std::optional<std::string> key;
    };

    template <typename Container>
    auto start_container(Container container) -> bool
    {
        if (_depth >= _limits.max_depth) {
            return fail(JsonFailure::DepthLimit);
        }

        JsonValue value;
        value.data = std::move(container);
        _frames.push_back(Frame{.value = std::move(value)});
        ++_depth;
        return true;
    }

    auto finish_container() -> bool
    {
        auto value = std::move(_frames.back().value);
        _frames.pop_back();
        --_depth;
        return add_value(std::move(value));
    }

    auto add_value(JsonValue value) -> bool
    {
        if (_frames.empty()) {
            if (_root) {
                return fail(JsonFailure::Internal);
            }
            _root = std::move(value);
            return true;
        }

        auto& frame = _frames.back();
        if (auto* array = std::get_if<JsonValue::Array>(&frame.value.data)) {
            array->push_back(std::move(value));
            return true;
        }

        auto* object = std::get_if<JsonValue::Object>(&frame.value.data);
        if (object == nullptr || !frame.key) {
            return fail(JsonFailure::Internal);
        }
        object->emplace(std::move(*frame.key), std::move(value));
        frame.key.reset();
        return true;
    }

    auto fail(JsonFailure failure) -> bool
    {
        if (!_failure) {
            _failure = failure;
        }
        return false;
    }

    JsonLimits                 _limits;
    std::size_t                _depth{0};
    std::vector<Frame>         _frames;
    std::optional<JsonValue>   _root;
    std::optional<JsonFailure> _failure;
};

auto encode_json(JsonValue const& value, CodecJson& output) -> std::optional<JsonFailure>
{
    return std::visit(
        [&output](auto const& stored) -> std::optional<JsonFailure> {
            using Stored = std::decay_t<decltype(stored)>;

            if constexpr (std::is_same_v<Stored, JsonNull>) {
                output = nullptr;
            }
            else if constexpr (std::is_same_v<Stored, bool> || std::is_same_v<Stored, std::int64_t> ||
                               std::is_same_v<Stored, std::uint64_t>) {
                output = stored;
            }
            else if constexpr (std::is_same_v<Stored, double>) {
                if (!std::isfinite(stored)) {
                    return JsonFailure::NonFinite;
                }
                output = stored;
            }
            else if constexpr (std::is_same_v<Stored, std::string>) {
                if (!is_valid_utf8(stored)) {
                    return JsonFailure::InvalidUtf8;
                }
                output = stored;
            }
            else if constexpr (std::is_same_v<Stored, JsonValue::Array>) {
                output = CodecJson::array();
                for (auto const& child : stored) {
                    CodecJson encoded;
                    if (auto failure = encode_json(child, encoded)) {
                        return failure;
                    }
                    output.push_back(std::move(encoded));
                }
            }
            else if constexpr (std::is_same_v<Stored, JsonValue::Object>) {
                output = CodecJson::object();
                for (auto const& [key, child] : stored) {
                    if (!is_valid_utf8(key)) {
                        return JsonFailure::InvalidUtf8;
                    }
                    CodecJson encoded;
                    if (auto failure = encode_json(child, encoded)) {
                        return failure;
                    }
                    output.emplace(key, std::move(encoded));
                }
            }
            return std::nullopt;
        },
        value.data);
}

} // anonymous namespace

auto parse_json(std::string_view text, JsonLimits limits) -> jb::core::Result<JsonValue, jb::core::Error>
{
    using Result = jb::core::Result<JsonValue, jb::core::Error>;

    if (!is_valid_utf8(text)) {
        return Result::failure(make_json_error(JsonFailure::InvalidUtf8));
    }

    JsonSaxParser parser{limits};
    try {
        auto const parsed = CodecJson::sax_parse(text.begin(), text.end(), &parser);
        if (parsed && parser.has_result()) {
            return Result::success(parser.take_result());
        }
        return Result::failure(make_json_error(parser.failure().value_or(JsonFailure::Syntax)));
    }
    catch (nlohmann::json::exception const&) {
        return Result::failure(make_json_error(JsonFailure::Internal));
    }
}

auto serialize_json(JsonValue const& value) -> jb::core::Result<std::string, jb::core::Error>
{
    using Result = jb::core::Result<std::string, jb::core::Error>;

    try {
        CodecJson encoded;
        if (auto failure = encode_json(value, encoded)) {
            return Result::failure(make_json_error(*failure));
        }
        return Result::success(encoded.dump());
    }
    catch (nlohmann::json::exception const&) {
        return Result::failure(make_json_error(JsonFailure::Internal));
    }
}

} // namespace jb::rpc
