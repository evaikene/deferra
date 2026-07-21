#include "queue_validation_priv.hpp"

#include <cstddef>

namespace jb::jobu::detail {

namespace {

constexpr std::size_t kMaximumQueueNameBytes      = 128;
constexpr std::size_t kMaximumIdempotencyKeyBytes = 128;

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
            auto const valid_second = first == 0xe0U ? second >= 0xa0U && second <= 0xbfU
                                    : first == 0xedU ? second >= 0x80U && second <= 0x9fU
                                                     : is_continuation(second);
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
            auto const valid_second = first == 0xf0U ? second >= 0x90U && second <= 0xbfU
                                    : first == 0xf4U ? second >= 0x80U && second <= 0x8fU
                                                     : is_continuation(second);
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

auto has_ascii_control(std::string_view text) noexcept -> bool
{
    for (auto const character : text) {
        auto const byte = static_cast<unsigned char>(character);
        if (byte <= 0x1fU || byte == 0x7fU) {
            return true;
        }
    }
    return false;
}

auto is_hexadecimal(char character) noexcept -> bool
{
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

auto is_uuid_text(std::string_view text) noexcept -> bool
{
    if (text.size() != 36U) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (text[index] != '-') {
                return false;
            }
        }
        else if (!is_hexadecimal(text[index])) {
            return false;
        }
    }
    return true;
}

auto has_reserved_deletion_suffix(std::string_view name) noexcept -> bool
{
    constexpr std::string_view marker{"-deleted#"};
    auto const                 position = name.rfind(marker);
    return position != std::string_view::npos && is_uuid_text(name.substr(position + marker.size()));
}

} // anonymous namespace

auto is_valid_queue_name(std::string_view name) noexcept -> bool
{
    return !name.empty() && name.size() <= kMaximumQueueNameBytes && is_valid_utf8(name) && !has_ascii_control(name) &&
           !has_reserved_deletion_suffix(name);
}

auto is_valid_idempotency_key(std::string_view key) noexcept -> bool
{
    return !key.empty() && key.size() <= kMaximumIdempotencyKeyBytes && is_valid_utf8(key);
}

} // namespace jb::jobu::detail
