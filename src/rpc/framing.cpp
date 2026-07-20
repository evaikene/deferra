#include "framing.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace jb::rpc {

namespace {

using AppendResult = jb::core::Result<std::vector<std::string>, jb::core::Error>;
using FrameResult  = jb::core::Result<std::string, jb::core::Error>;
using LengthResult = jb::core::Result<std::size_t, jb::core::Error>;

enum class FramingFailure {
    HeaderTooLarge,
    InvalidHeader,
    MissingContentLength,
    DuplicateContentLength,
    InvalidContentLength,
    BodyTooLarge,
    UnsupportedContentType,
};

auto make_framing_error(FramingFailure failure) -> jb::core::Error
{
    using jb::core::Error;
    using jb::core::ErrorCategory;

    switch (failure) {
        case FramingFailure::HeaderTooLarge:
            return Error{
                .category = ErrorCategory::ResourceExhausted,
                .code     = "rpc.framing.header_too_large",
                .message  = "Framing header exceeds the configured limit",
            };
        case FramingFailure::InvalidHeader:
            return Error{
                .category = ErrorCategory::InvalidArgument,
                .code     = "rpc.framing.invalid_header",
                .message  = "Framing header is invalid",
            };
        case FramingFailure::MissingContentLength:
            return Error{
                .category = ErrorCategory::InvalidArgument,
                .code     = "rpc.framing.missing_content_length",
                .message  = "Framing header requires Content-Length",
            };
        case FramingFailure::DuplicateContentLength:
            return Error{
                .category = ErrorCategory::InvalidArgument,
                .code     = "rpc.framing.duplicate_content_length",
                .message  = "Framing header contains duplicate Content-Length fields",
            };
        case FramingFailure::InvalidContentLength:
            return Error{
                .category = ErrorCategory::InvalidArgument,
                .code     = "rpc.framing.invalid_content_length",
                .message  = "Framing Content-Length is invalid",
            };
        case FramingFailure::BodyTooLarge:
            return Error{
                .category = ErrorCategory::ResourceExhausted,
                .code     = "rpc.framing.body_too_large",
                .message  = "Framing body exceeds the configured limit",
            };
        case FramingFailure::UnsupportedContentType:
            return Error{
                .category = ErrorCategory::Unsupported,
                .code     = "rpc.framing.unsupported_content_type",
                .message  = "Framing Content-Type must specify UTF-8",
            };
    }

    return Error{
        .category = ErrorCategory::InvalidArgument,
        .code     = "rpc.framing.invalid_header",
        .message  = "Framing header is invalid",
    };
}

auto is_ows(char value) noexcept -> bool
{
    return value == ' ' || value == '\t';
}

auto trim_ows(std::string_view value) noexcept -> std::string_view
{
    while (!value.empty() && is_ows(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_ows(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

auto is_token_character(unsigned char value) noexcept -> bool
{
    if ((value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')) {
        return true;
    }

    switch (value) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

auto is_field_value_character(unsigned char value) noexcept -> bool
{
    return value == '\t' || (value >= 0x20U && value != 0x7fU);
}

auto ascii_equal(std::string_view lhs, std::string_view rhs) noexcept -> bool
{
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (auto index = std::size_t{0}; index < lhs.size(); ++index) {
        auto left  = static_cast<unsigned char>(lhs[index]);
        auto right = static_cast<unsigned char>(rhs[index]);
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<unsigned char>(left + ('a' - 'A'));
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<unsigned char>(right + ('a' - 'A'));
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

auto parse_content_length(std::string_view value) -> LengthResult
{
    value = trim_ows(value);
    if (value.empty()) {
        return LengthResult::failure(make_framing_error(FramingFailure::InvalidContentLength));
    }

    auto length = std::size_t{0};
    for (auto character : value) {
        auto const byte = static_cast<unsigned char>(character);
        if (byte < '0' || byte > '9') {
            return LengthResult::failure(make_framing_error(FramingFailure::InvalidContentLength));
        }

        auto const digit = static_cast<std::size_t>(byte - '0');
        if (length > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            return LengthResult::failure(make_framing_error(FramingFailure::InvalidContentLength));
        }
        length = length * 10U + digit;
    }

    return LengthResult::success(length);
}

class ContentTypeParser final {
public:
    explicit ContentTypeParser(std::string_view value)
        : _value{trim_ows(value)}
    {}

    enum class Status {
        Supported,
        Invalid,
        Unsupported,
    };

    auto parse() -> Status
    {
        if (!parse_token() || !consume('/') || !parse_token()) {
            return Status::Invalid;
        }

        auto charset_seen      = false;
        auto charset_supported = false;
        skip_ows();
        while (!at_end()) {
            if (!consume(';')) {
                return Status::Invalid;
            }
            skip_ows();

            auto const name = parse_token();
            if (!name) {
                return Status::Invalid;
            }
            skip_ows();
            if (!consume('=')) {
                return Status::Invalid;
            }
            skip_ows();

            auto parameter_value = parse_parameter_value();
            if (!parameter_value) {
                return Status::Invalid;
            }

            if (ascii_equal(*name, "charset")) {
                if (charset_seen) {
                    return Status::Unsupported;
                }
                charset_seen      = true;
                charset_supported = ascii_equal(*parameter_value, "utf-8") || ascii_equal(*parameter_value, "utf8");
            }

            skip_ows();
        }

        return charset_seen && charset_supported ? Status::Supported : Status::Unsupported;
    }

private:
    auto at_end() const noexcept -> bool { return _position == _value.size(); }

    void skip_ows() noexcept
    {
        while (!at_end() && is_ows(_value[_position])) {
            ++_position;
        }
    }

    auto consume(char expected) noexcept -> bool
    {
        if (at_end() || _value[_position] != expected) {
            return false;
        }
        ++_position;
        return true;
    }

    auto parse_token() noexcept -> std::optional<std::string_view>
    {
        auto const begin = _position;
        while (!at_end() && is_token_character(static_cast<unsigned char>(_value[_position]))) {
            ++_position;
        }
        if (_position == begin) {
            return std::nullopt;
        }
        return _value.substr(begin, _position - begin);
    }

    auto parse_parameter_value() -> std::optional<std::string>
    {
        if (at_end()) {
            return std::nullopt;
        }
        if (_value[_position] != '"') {
            auto token = parse_token();
            if (!token) {
                return std::nullopt;
            }
            return std::string{*token};
        }

        ++_position;
        auto result = std::string{};
        while (!at_end()) {
            auto const byte = static_cast<unsigned char>(_value[_position++]);
            if (byte == '"') {
                return result;
            }
            if (byte == '\\') {
                if (at_end()) {
                    return std::nullopt;
                }
                auto const escaped = static_cast<unsigned char>(_value[_position++]);
                if (!is_field_value_character(escaped)) {
                    return std::nullopt;
                }
                result.push_back(static_cast<char>(escaped));
                continue;
            }
            if (!is_field_value_character(byte) || byte == '"') {
                return std::nullopt;
            }
            result.push_back(static_cast<char>(byte));
        }
        return std::nullopt;
    }

    std::string_view _value;
    std::size_t      _position{0};
};

auto parse_header(std::string_view header, FramingLimits const& limits) -> LengthResult
{
    auto content_length = std::optional<std::string_view>{};
    auto content_type   = std::optional<std::string_view>{};

    auto remaining = header.substr(0, header.size() - 4U);
    while (!remaining.empty()) {
        auto const line_end = remaining.find("\r\n");
        auto const line     = remaining.substr(0, line_end);
        remaining           = line_end == std::string_view::npos ? std::string_view{} : remaining.substr(line_end + 2U);

        auto const colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            return LengthResult::failure(make_framing_error(FramingFailure::InvalidHeader));
        }

        auto const name  = line.substr(0, colon);
        auto const value = line.substr(colon + 1U);
        if (!std::all_of(name.begin(),
                         name.end(),
                         [](char character) { return is_token_character(static_cast<unsigned char>(character)); }) ||
            !std::all_of(value.begin(), value.end(), [](char character) {
                return is_field_value_character(static_cast<unsigned char>(character));
            })) {
            return LengthResult::failure(make_framing_error(FramingFailure::InvalidHeader));
        }

        if (ascii_equal(name, "Content-Length")) {
            if (content_length) {
                return LengthResult::failure(make_framing_error(FramingFailure::DuplicateContentLength));
            }
            content_length = value;
        }
        else if (ascii_equal(name, "Content-Type")) {
            if (content_type) {
                return LengthResult::failure(make_framing_error(FramingFailure::InvalidHeader));
            }
            content_type = value;
        }
    }

    if (!content_length) {
        return LengthResult::failure(make_framing_error(FramingFailure::MissingContentLength));
    }

    auto parsed_length = parse_content_length(*content_length);
    if (!parsed_length) {
        return parsed_length;
    }
    if (*parsed_length > limits.max_body_bytes) {
        return LengthResult::failure(make_framing_error(FramingFailure::BodyTooLarge));
    }

    if (content_type) {
        switch (ContentTypeParser{*content_type}.parse()) {
            case ContentTypeParser::Status::Supported:
                break;
            case ContentTypeParser::Status::Invalid:
                return LengthResult::failure(make_framing_error(FramingFailure::InvalidHeader));
            case ContentTypeParser::Status::Unsupported:
                return LengthResult::failure(make_framing_error(FramingFailure::UnsupportedContentType));
        }
    }

    return parsed_length;
}

auto has_header_terminator(std::string const& header) noexcept -> bool
{
    auto const size = header.size();
    return size >= 4U && header[size - 4U] == '\r' && header[size - 3U] == '\n' && header[size - 2U] == '\r' &&
           header[size - 1U] == '\n';
}

} // anonymous namespace

struct StreamFramer::Private {
    enum class State {
        Header,
        Body,
    };

    explicit Private(FramingLimits limits)
        : limits{limits}
    {}

    auto append(std::string_view bytes) -> AppendResult
    {
        if (failure) {
            return AppendResult::failure(*failure);
        }

        auto messages = std::vector<std::string>{};
        while (!bytes.empty()) {
            if (state == State::Header) {
                if (header.size() >= limits.max_header_bytes) {
                    return poison(make_framing_error(FramingFailure::HeaderTooLarge));
                }

                auto const character = bytes.front();
                bytes.remove_prefix(1);
                if ((!header.empty() && header.back() == '\r' && character != '\n') ||
                    (character == '\n' && (header.empty() || header.back() != '\r'))) {
                    return poison(make_framing_error(FramingFailure::InvalidHeader));
                }

                header.push_back(character);
                if (!has_header_terminator(header)) {
                    continue;
                }

                auto length = parse_header(header, limits);
                if (!length) {
                    return poison(std::move(length).error());
                }

                expected_body_bytes = *length;
                header.clear();
                if (expected_body_bytes == 0U) {
                    messages.emplace_back();
                    continue;
                }

                state = State::Body;
                continue;
            }

            auto const remaining = expected_body_bytes - body.size();
            auto const count     = std::min(remaining, bytes.size());
            body.append(bytes.data(), count);
            bytes.remove_prefix(count);
            if (body.size() != expected_body_bytes) {
                continue;
            }

            messages.push_back(std::move(body));
            body                = {};
            expected_body_bytes = 0U;
            state               = State::Header;
        }

        return AppendResult::success(std::move(messages));
    }

    auto poison(jb::core::Error error) -> AppendResult
    {
        std::string{}.swap(header);
        std::string{}.swap(body);
        expected_body_bytes = 0U;
        state               = State::Header;
        failure             = std::move(error);
        return AppendResult::failure(*failure);
    }

    void reset() noexcept
    {
        std::string{}.swap(header);
        std::string{}.swap(body);
        expected_body_bytes = 0U;
        state               = State::Header;
        failure.reset();
    }

    [[nodiscard]] auto buffered_bytes() const noexcept -> std::size_t { return header.size() + body.size(); }

    FramingLimits                  limits;
    State                          state{State::Header};
    std::string                    header;
    std::string                    body;
    std::size_t                    expected_body_bytes{0};
    std::optional<jb::core::Error> failure;
};

StreamFramer::StreamFramer(FramingLimits limits)
    : _data{std::make_unique<Private>(limits)}
{}

StreamFramer::~StreamFramer() = default;

StreamFramer::StreamFramer(StreamFramer&& other) noexcept = default;

auto StreamFramer::operator=(StreamFramer&& other) noexcept -> StreamFramer& = default;

auto StreamFramer::append(std::string_view bytes) -> AppendResult
{
    return _data->append(bytes);
}

void StreamFramer::reset() noexcept
{
    _data->reset();
}

auto StreamFramer::buffered_bytes() const noexcept -> std::size_t
{
    return _data->buffered_bytes();
}

auto StreamFramer::limits() const noexcept -> FramingLimits const&
{
    return _data->limits;
}

auto frame_message(std::string_view body, FramingLimits limits) -> FrameResult
{
    if (body.size() > limits.max_body_bytes) {
        return FrameResult::failure(make_framing_error(FramingFailure::BodyTooLarge));
    }

    auto header = std::string{"Content-Length: "} + std::to_string(body.size()) + "\r\n\r\n";
    if (header.size() > limits.max_header_bytes) {
        return FrameResult::failure(make_framing_error(FramingFailure::HeaderTooLarge));
    }

    header.append(body.data(), body.size());
    return FrameResult::success(std::move(header));
}

} // namespace jb::rpc
