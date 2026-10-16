#include "http_job_method_priv.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobu::detail {

namespace {

struct MethodSpelling {
    HttpJobMethod    method;
    std::string_view text;
};

constexpr auto kMethods = std::array{
    MethodSpelling{.method = HttpJobMethod::Get,    .text = "GET"   },
    MethodSpelling{.method = HttpJobMethod::Post,   .text = "POST"  },
    MethodSpelling{.method = HttpJobMethod::Put,    .text = "PUT"   },
    MethodSpelling{.method = HttpJobMethod::Patch,  .text = "PATCH" },
    MethodSpelling{.method = HttpJobMethod::Delete, .text = "DELETE"},
    MethodSpelling{.method = HttpJobMethod::Head,   .text = "HEAD"  },
};

constexpr auto ascii_upper(unsigned char value) noexcept -> unsigned char
{
    if (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z')) {
        return static_cast<unsigned char>(value - ('a' - 'A'));
    }
    return value;
}

auto matches_submitted_method(std::string_view value, std::string_view canonical) noexcept -> bool
{
    return value.size() == canonical.size() && std::ranges::equal(value, canonical, [](char left, char right) {
               return ascii_upper(static_cast<unsigned char>(left)) == static_cast<unsigned char>(right);
           });
}

auto quoted_method(std::string_view value) -> std::string
{
    // Bound inspected bytes before escaping. Non-ASCII bytes are escaped too, keeping diagnostics valid UTF-8.
    constexpr std::size_t kMaximumDisplayedBytes{64};
    constexpr auto        hex    = std::string_view{"0123456789abcdef"};
    auto                  result = std::string{"\""};
    for (char character : value.substr(0, kMaximumDisplayedBytes)) {
        auto const byte = static_cast<unsigned char>(character);
        if (byte < 0x20U || byte >= 0x7fU) {
            result += "\\x";
            result.push_back(hex[byte >> 4U]);
            result.push_back(hex[byte & 0x0fU]);
        }
        else {
            if (character == '"' || character == '\\') {
                result.push_back('\\');
            }
            result.push_back(character);
        }
    }
    if (value.size() > kMaximumDisplayedBytes) {
        result += "...";
    }
    result.push_back('"');
    return result;
}

} // anonymous namespace

auto parse_submitted_http_method(std::string_view value) -> jb::core::Result<HttpJobMethod, jb::core::Error>
{
    using ParseResult = jb::core::Result<HttpJobMethod, jb::core::Error>;
    for (auto const& entry : kMethods) {
        if (matches_submitted_method(value, entry.text)) {
            return ParseResult::success(entry.method);
        }
    }

    return ParseResult::failure({
        .category = jb::core::ErrorCategory::InvalidArgument,
        .code     = "jobu.job.invalid_payload",
        .message  = fmt::format("Unsupported HTTP method {}. Supported methods: {}.",
                                quoted_method(value),
                                supported_http_job_methods),
        .detail   = "reason=invalid_method",
    });
}

auto http_method_text(HttpJobMethod method) noexcept -> std::string_view
{
    for (auto const& entry : kMethods) {
        if (entry.method == method) {
            return entry.text;
        }
    }
    return {};
}

auto decode_stored_http_method(std::string value) -> DecodedHttpMethod
{
    // Exact comparison is deliberate: lowercase HEAD and custom tokens keep their previous wire behavior.
    for (auto const& entry : kMethods) {
        if (value == entry.text) {
            return entry.method;
        }
    }
    return LegacyHttpMethod{.wire_value = std::move(value)};
}

auto http_method_wire_text(DecodedHttpMethod const& method) noexcept -> std::string_view
{
    if (auto const* canonical = std::get_if<HttpJobMethod>(&method)) {
        return http_method_text(*canonical);
    }
    return std::get<LegacyHttpMethod>(method).wire_value;
}

} // namespace jb::jobu::detail
